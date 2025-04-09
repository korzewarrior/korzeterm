# KorzeTerm

A minimal, blazingly fast terminal emulator written in C++ with Qt5.

## Features

- Fast, responsive terminal emulation
- Real PTY (pseudo-terminal) support
- Proper handling of terminal control sequences
- Monospace font support
- Clean, minimal interface

## Requirements

- Qt5 (qt5-base, qt5-tools)
- C++11 compiler (g++)
- libutil/libutil (part of glibc)

## Building

```bash
# Compile the build script
g++ build.cpp -o build

# Run the build script to compile and start KorzeTerm
./build
```

## Direct Compilation

If you prefer to compile directly:

```bash
# Generate the Meta-Object file
moc main.cpp -o main.moc

# Compile KorzeTerm
g++ -std=c++11 -o korzeterm main.cpp $(pkg-config --cflags --libs Qt5Widgets Qt5Core Qt5Gui) -lutil -Wall -Wextra
```

## Usage

Once compiled, run KorzeTerm:

```bash
./korzeterm
```

## Implementation Details

KorzeTerm uses a proper PTY implementation with the following features:

1. Full terminal emulation with proper escape sequence handling
2. Direct PTY access using forkpty() for true terminal integration
3. Proper window resizing notifications to child processes
4. Efficient text rendering with Qt's QPainter
5. Minimal memory footprint

## License

MIT 