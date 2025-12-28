# databento-async

Async Databento client with libuv integration for custom event loop control.

## Features

- libuv-based async I/O
- Zero-copy DBN record parsing
- Full control over event loop integration
- Support for Live and Historical APIs

## Quick Start

### Prerequisites

- [Bazelisk](https://github.com/bazelbuild/bazelisk) (recommended) or Bazel 7.4.1+
- GCC 14+ or Clang 19+

### Build

```bash
# Build all targets
bazel build //...

# Run tests
bazel test //...
```

## Project Structure

```
├── src/
│   ├── dbn/           # DBN record definitions and parsing
│   ├── protocol/      # Live/Historical protocol implementation
│   └── io/            # libuv I/O layer
├── tests/             # Test suite
└── docs/              # Documentation
```

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│ Your Application                                            │
├─────────────────────────────────────────────────────────────┤
│ databento-async                                             │
│   ├── Protocol Layer (auth, subscribe, HTTP)               │
│   ├── DBN Parser (zero-copy record parsing)                │
│   └── I/O Layer (libuv integration)                        │
├─────────────────────────────────────────────────────────────┤
│ libuv event loop (user-controlled)                         │
└─────────────────────────────────────────────────────────────┘
```

## License

MIT
