# ISR - I Send & Receive

A simple, lightweight file transfer tool written in C17. No SCP, no SFTP, no cloud drives — just send and receive files directly between machines.

## Features

- Simple client-server model over TCP
- Optional LZ4 block compression
- Recursive directory upload/download
- Cross-platform: Linux, macOS, Windows (MinGW)
- Server is jail-rooted to a specified directory

## Build

```bash
make                      # Linux/macOS (output: build/isr)
make -f Makefile.mingw    # Windows cross-compile (output: build-mingw/isr.exe)
make clean                # Remove build artifacts
```

## Usage

### Start a server

```bash
./build/isr serve <directory> [-p port]
```

Default port is `42069`.

### Send a file

```bash
./build/isr send <host:port> <local_file> <remote_path> [-c blocksize] [-o]
```

- `-c blocksize` — enable LZ4 compression with block size in KB
- `-o` — overwrite if the file already exists

### Receive a file or list a directory

```bash
./build/isr recv <host:port> <remote_path> [local_path] [-c blocksize]
```

- If `remote_path` is a directory, its contents are listed.
- If `remote_path` is a file, it is downloaded.
- `local_path` defaults to the basename of `remote_path`.

## Protocol

ISR uses a custom binary protocol over TCP. Each connection handles one operation.

1. Client sends a command byte: `RECV` (`0x00`) or `SEND` (`0x01`)
2. Client sends a variable-length header with flags, path, and compression parameters
3. Server responds with a status and (for recv) file metadata
4. Data is streamed, optionally LZ4-compressed in prefixed blocks
5. An 8-byte checksum (sum of all byte values) is appended at the end

All multi-byte fields are big-endian. Directory listings are always uncompressed.

## Security

The server resolves all paths against its root directory. Requests containing `..` components or paths that escape the root are rejected. The server handles one connection at a time.
