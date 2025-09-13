# SystemHW25

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

This repository contains various homework assignments and projects related to system programming, including client-server applications, file management, concurrency, and more. Each subfolder represents a different assignment or project from the System Programming course.

## Table of Contents

- [Repository Structure](#repository-structure)
- [Prerequisites](#prerequisites)
- [Getting Started](#getting-started)
- [Assignments Overview](#assignments-overview)
- [Build & Run](#build--run)
- [Contributing](#contributing)
- [License](#license)
- [Author](#author)

## Repository Structure

- `final/` - Final project: Client-server chat application with logging and file transfer.
- `hw1/` - Homework 1: File management utilities.
- `hw2/` - Homework 2: System programming assignment (details in subfolder).
- `hw3/` - Homework 3: System programming assignment (details in subfolder).
- `hw4/` - Homework 4: Buffer implementation and logging.
- `midterm/` - Midterm project: Bank client-server simulation with concurrency and testing.

## Prerequisites

- GCC or Clang compiler
- Make build system
- Basic knowledge of C programming
- Unix-like environment (Linux/macOS)

## Getting Started

1. Clone the repository:
   ```sh
   git clone https://github.com/rfurkan37/SystemHW25.git
   cd SystemHW25
   ```
2. Navigate to the relevant assignment folder and follow the instructions in its README or Makefile.

## Assignments Overview

### Homework 1 (hw1/)
A comprehensive file management utility written in C that provides essential file and directory operations with built-in logging capabilities and file locking mechanisms. Features include directory creation/deletion, file operations, directory listing with extension filtering, process management, and thread-safe operations using `flock()`.

### Homework 2 (hw2/)
An inter-process communication program using named pipes (FIFOs), signal handling, and daemon processes. Demonstrates advanced system programming concepts including multi-process architecture, FIFO communication, proper signal handling, zombie process prevention, and background daemon logging.

### Homework 3 (hw3/)
Advanced system programming assignment focusing on core system concepts. Implementation details available in the main source file and accompanying report.

### Homework 4 (hw4/)
Buffer implementation project with logging capabilities. Includes buffer management, system programming concepts, and comprehensive logging functionality for debugging and monitoring.

### Midterm (midterm/)
Bank client-server simulation with automated testing. Features client-server architecture, teller processes, concurrency handling, comprehensive test suites including memory leak detection, and various test cases for error handling and stress testing.

### Final Project (final/)
Complete client-server chat application with advanced features including room management, file transfer capabilities, logging system, and multi-client support. Includes separate client and server implementations with shared utilities and automated launch scripts.

## Build & Run

Most assignments can be built using `make` or the provided build scripts. For example:

```sh
cd hw1
make
./main
```

Refer to each subfolder's README.md for specific build and run instructions.

## Contributing

Contributions are welcome! Please follow these steps:

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/AmazingFeature`)
3. Commit your changes (`git commit -m 'Add some AmazingFeature'`)
4. Push to the branch (`git push origin feature/AmazingFeature`)
5. Open a Pull Request

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

## Author

Recep Furkan Akın - [GitHub](https://github.com/rfurkan37)
