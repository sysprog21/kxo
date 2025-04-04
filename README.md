# kxo: A Tic-Tac-Toe Game Engine implemented as Linux kernel module

## Introduction
`kxo` is a Linux kernel module that implements the [tic-tac-toe game](https://en.wikipedia.org/wiki/Tic-tac-toe)
(aka XO Game) as kernel threads.
This educational module demonstrates several essential Linux kernel programming concepts:
  - Circular buffer implementation
  - Mutex lock synchronization
  - IRQ handling
  - SoftIRQ processing
  - Tasklet scheduling
  - Workqueue management
  - Kernel thread creation and execution

The module supports multiple AI algorithms for game strategy, allowing kernel threads to compete against each other in tic-tac-toe matches.
`kxo` implements two advanced algorithms for tic-tac-toe gameplay:
- Monte Carlo Tree Search (MCTS): A probabilistic algorithm that uses random sampling to evaluate moves and determine optimal game strategies
- Negamax Algorithm: A depth-first minimax variant that efficiently evaluates game positions by alternating between maximizing and minimizing players

## Build and Run
After the source code is downloaded, go into the directory and do as the following
```
$ make
```

Make sure the kernel object file (`kxo.ko`) is built correctly, then you can insert the kernel module
```
$ sudo insmod kxo.ko
```

`kxo` provides an interface for userspace interaction through the companion tool `xo-user`.
This utility offers the following functionality:
- Display the current status of the `kxo` module (loaded/unloaded)
- Real-time visualization of the tic-tac-toe game board
- Control commands:
  - `Ctrl + P`: Toggle pause/resume of the game board display
  - `Ctrl + Q`: Terminate all tic-tac-toe games running in kernel space

Simply run the command below after the kernel module is loaded:
```
$ sudo ./xo-user
```

To unload the kernel module, use the command:
```
$ sudo rmmod kxo
```

## License

`kxo` is released under the MIT license. Use of this source code is governed
by a MIT-style license that can be found in the LICENSE file.
