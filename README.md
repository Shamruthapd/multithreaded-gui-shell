# Multithreaded GUI Shell with Real-Time Job Monitoring and Process Control

## Overview

This project implements a GTK-based multithreaded shell in C.

The shell supports concurrent execution of multiple commands using `&&`, process creation using `fork()`, command execution using `execvp()`, inter-process communication using pipes, and synchronization using `waitpid()` and pthread mutexes.

The project also provides real-time job monitoring and process control.

## Features

- GTK-based graphical shell
- Multithreaded command execution
- Parallel execution using `&&`
- Process creation using `fork()`
- Program execution using `execvp()`
- Pipe-based communication
- Job ID and PID monitoring
- Job status monitoring
- Execution-time monitoring
- Individual process termination using `kill <job_id>`
- Termination of all active jobs using `Ctrl+B`

## Requirements

- Ubuntu/Linux
- GCC
- GTK+ 3
- pthread
- pkg-config

## Installation

Install the required packages:

```bash
sudo apt update
sudo apt install build-essential libgtk-3-dev pkg-config
