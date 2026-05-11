#include <linux/slab.h>
#include <linux/string.h>

#include "game.h"
#include "mcts.h"
#include "util.h"

struct node {
    int move;
    char player;
    int n_visits;
    u64 score;
    struct node *parent;
    struct node *children[N_GRIDS];
};

static struct mcts_info mcts_obj;

static struct node *new_node(int move, char player, struct node *parent)
{
    struct node *node = kzalloc(sizeof(struct node), GFP_KERNEL);
    if (!node)
        return NULL;
    node->move = move;
    node->player = player;
    node->parent = parent;
    return node;
}

static void free_node(struct node *node)
{
    for (int i = 0; i < N_GRIDS; i++)
        if (node->children[i])
            free_node(node->children[i]);
    kfree(node);
}

static fixed_point_t fixed_sqrt(fixed_point_t x)
{
    if (!x || x == (1U << FIXED_SCALE_BITS))
        return x;

    fixed_point_t s = 0U;
    for (int i = (31 - __builtin_clz(x | 1)); i >= 0; i--) {
        fixed_point_t t = (1U << i);
        u64 candidate = (u64) s + t;
        if (((candidate * candidate) >> FIXED_SCALE_BITS) <= x)
            s += t;
    }
    return s;
}

#define LOG2_TABLE_SIZE 10
static unsigned log2_table[1U << LOG2_TABLE_SIZE];
static void log2_table_init(void)
{
    /*
     * Q0.32 fixed-point representation of
     * 2^{1/2}-1, 2^{1/4}-1, ..., 2^{2^{-32}}-1
     */
    const unsigned jump[32] = {
        1779033704, 812638371, 388727752, 190154448, 94047537, 46769127,
        23321248,   11644838,  5818478,   2908254,   1453881,  726879,
        363424,     181708,    90853,     45426,     22713,    11357,
        5678,       2839,      1420,      710,       355,      177,
        89,         44,        22,        11,        6,        3,
        1,          1};

    for (unsigned i = 0; i < (1U << LOG2_TABLE_SIZE); ++i) {
        /*
         * Use binary search to find the largest log2(1+x) <= log2(1+i/1024)
         */
        u64 target = (u64) i << (64 - LOG2_TABLE_SIZE), now = 0;
        unsigned log = 0;
        for (unsigned j = 0; j < 32; ++j) {
            /* (1+now) * (1+jump) = 1 + now + jump + now*jump */
            u64 t = ((now + (1U << 31)) >> 32) * jump[j];
            if (now + ((u64) jump[j] << 32) + t <= target) {
                now += ((u64) jump[j] << 32) + t;
                log |= 1U << (31 - j);
            }
        }
        log2_table[i] = log;
    }
}

static fixed_point_t fixed_log2(fixed_point_t v)
{
    if (!v || v == (1U << FIXED_SCALE_BITS))
        return 0;

    int log2_v = 15 - __builtin_clz(v);
    v <<= (15 - log2_v);
    fixed_point_t int_part = (unsigned) log2_v << FIXED_SCALE_BITS;

    unsigned index = (v ^ (1U << 31)) >> (31 - LOG2_TABLE_SIZE);
    unsigned lower = log2_table[index];
    unsigned upper =
        index == (1 << LOG2_TABLE_SIZE) - 1 ? 0 : log2_table[index + 1];

    unsigned offset = v & ((1U << (31 - LOG2_TABLE_SIZE)) - 1);
    u64 frac_part =
        lower +
        (((u64) (upper - lower) * offset + (1U << (30 - LOG2_TABLE_SIZE))) >>
         (31 - LOG2_TABLE_SIZE));

    unsigned result = int_part + ((frac_part + (1U << 15)) >> 16);

    /* Convert from 2's complement to signed representation */
    if (GET_SIGN(result))
        result = SET_SIGN(-result);

    return result;
}

#define SQRT_LOG_2 54562
#define EXPLORATION_FACTOR \
    (fixed_sqrt(1U << (FIXED_SCALE_BITS + 1)) * SQRT_LOG_2 >> FIXED_SCALE_BITS)

static inline fixed_point_t uct_score(int n_total, int n_visits, u64 score)
{
    if (n_visits == 0)
        return FIXED_MAX;

    fixed_point_t result = (fixed_point_t) (score / n_visits);
    fixed_point_t log_val = fixed_log2(
        (n_total < 65536) ? (n_total << FIXED_SCALE_BITS) : FIXED_MAX);
    fixed_point_t tmp =
        ((u64) EXPLORATION_FACTOR * fixed_sqrt(log_val / n_visits)) >>
        FIXED_SCALE_BITS;
    return result + tmp;
}

static struct node *select_move(struct node *node)
{
    struct node *best_node = NULL;
    fixed_point_t best_score = 0U;
    for (int i = 0; i < N_GRIDS; i++) {
        if (!node->children[i])
            continue;
        fixed_point_t score =
            uct_score(node->n_visits, node->children[i]->n_visits,
                      node->children[i]->score);
        if (score > best_score) {
            best_score = score;
            best_node = node->children[i];
        }
    }
    return best_node;
}

static fixed_point_t simulate(uint32_t table, char player)
{
    char current_player = player;
    uint32_t temp_table = table;
    xoro_jump(&(mcts_obj.xoro_obj));
    while (1) {
        int *moves = available_moves(temp_table);
        if (moves[0] == -1) {
            kfree(moves);
            break;
        }
        int n_moves = 0;
        while (n_moves < N_GRIDS && moves[n_moves] != -1)
            ++n_moves;
        int move = moves[xoro_next(&(mcts_obj.xoro_obj)) % n_moves];
        kfree(moves);
        temp_table = VAL_SET_CELL(temp_table, move, current_player);
        char win;
        if ((win = check_win(temp_table)) != CELL_EMPTY)
            return calculate_win_value(win, player);
        current_player ^= CELL_O ^ CELL_X;
    }
    return (fixed_point_t) (1UL << (FIXED_SCALE_BITS - 1));
}

static void backpropagate(struct node *node, fixed_point_t score)
{
    while (node) {
        node->n_visits++;
        node->score += score;
        node = node->parent;
        score = (1U << FIXED_SCALE_BITS) - score;
    }
}

static int expand(struct node *node, uint32_t table)
{
    int *moves = available_moves(table);
    int n_moves = 0;
    while (n_moves < N_GRIDS && moves[n_moves] != -1)
        ++n_moves;
    for (int i = 0; i < n_moves; i++) {
        node->children[i] =
            new_node(moves[i], node->player ^ CELL_O ^ CELL_X, node);
        if (!node->children[i]) {
            kfree(moves);
            return i;
        }
    }
    kfree(moves);
    return n_moves;
}

int mcts(uint32_t table, char player)
{
    char win;
    struct node *root = new_node(-1, player, NULL);
    if (!root)
        return -1;
    mcts_obj.nr_active_nodes = 1;
    for (int i = 0; i < ITERATIONS; i++) {
        if (READ_ONCE(kxo_stop_work))
            break;
        struct node *node = root;
        uint32_t temp_table = table;
        while (1) {
            if ((win = check_win(temp_table)) != CELL_EMPTY) {
                fixed_point_t score =
                    calculate_win_value(win, node->player ^ CELL_O ^ CELL_X);
                backpropagate(node, score);
                break;
            }
            if (node->n_visits == 0) {
                fixed_point_t score = simulate(temp_table, node->player);
                backpropagate(node, score);
                break;
            }
            if (node->children[0] == NULL)
                mcts_obj.nr_active_nodes += expand(node, temp_table);
            node = select_move(node);
            if (!node) {
                free_node(root);
                return -1;
            }
            temp_table = VAL_SET_CELL(temp_table, node->move,
                                      node->player ^ CELL_O ^ CELL_X);
        }
    }
    struct node *best_node = root;
    int most_visits = -1;
    for (int i = 0; i < N_GRIDS; i++) {
        if (root->children[i] && root->children[i]->n_visits > most_visits) {
            most_visits = root->children[i]->n_visits;
            best_node = root->children[i];
        }
    }
    int best_move = best_node->move;
    free_node(root);
    return best_move;
}

void mcts_init(void)
{
    xoro_init(&(mcts_obj.xoro_obj));
    log2_table_init();
    mcts_obj.nr_active_nodes = 0;
}
