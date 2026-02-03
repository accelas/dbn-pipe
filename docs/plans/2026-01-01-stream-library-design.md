# Stream Library Extraction Design

Extract generic streaming components from dbn-pipe into `lib/stream/` for reuse.

## Decisions

- **Use case**: Personal reuse (simpler API)
- **Scope**: Core streaming + event loop
- **Structure**: Subdirectory `lib/stream/` now, separate repo later
- **Namespace**: `dbn_pipe` (unchanged, rename during repo split)
- **Design**: Keep existing patterns (CRTP, Error struct, .cpp files)
- **Approach**: File reorganization with minimal code changes

## Library Structure

```
lib/stream/
├── buffer_chain.hpp      # Move from src/ (unchanged)
├── component.hpp         # Move from src/pipeline_component.hpp (unchanged)
├── error.hpp             # Move from src/ (unchanged)
├── event_loop.hpp        # Move from src/ (unchanged)
├── event_loop.cpp        # Move from src/ (unchanged)
├── reactor.hpp           # Move from src/ (unchanged)
├── reactor.cpp           # Move from src/ (unchanged)
├── tcp_socket.hpp        # Move from src/ (unchanged)
└── BUILD.bazel
```

Note: `epoll_event_loop.hpp/.cpp` merged into `reactor.hpp/.cpp` since they're tightly coupled.

## Unchanged Components

The following are moved without modification:

- **Error handling**: Keep `Error` struct with `ErrorCode` enum
- **Component base**: Keep CRTP `PipelineComponent<Derived>`
- **Concepts**: Keep `Suspendable`, `Downstream`, `Upstream`, `TerminalDownstream` as-is
- **Reactor**: Keep separate `.cpp` files for compilation

## dbn-pipe Integration

### Re-export header

```cpp
// src/stream.hpp
#pragma once

#include "dbn_pipe/stream/buffer_chain.hpp"
#include "dbn_pipe/stream/component.hpp"
#include "dbn_pipe/stream/error.hpp"
#include "dbn_pipe/stream/event_loop.hpp"
#include "dbn_pipe/stream/reactor.hpp"
#include "dbn_pipe/stream/tcp_socket.hpp"
```

Note: Concepts (`Suspendable`, `Downstream`, etc.) are defined in `component.hpp`.

### Files remaining in src/

```
src/
├── stream.hpp              # Re-export header
├── client.hpp
├── cram_auth.hpp
├── dbn_parser_component.hpp
├── dns_resolver.hpp
├── historical_protocol.hpp
├── http_client.hpp
├── live_protocol.hpp
├── pipeline.hpp
├── pipeline_sink.hpp
├── protocol_driver.hpp
├── record_batch.hpp
├── symbol_map.hpp
├── tls_transport.hpp
├── zstd_decompressor.hpp
└── BUILD.bazel
```

### Files to delete from src/

- `buffer_chain.hpp`
- `pipeline_component.hpp`
- `tcp_socket.hpp`
- `event_loop.hpp`, `event_loop.cpp`
- `reactor.hpp`, `reactor.cpp`
- `epoll_event_loop.hpp`, `epoll_event_loop.cpp`
- `error.hpp`

### Include updates

Internal src/ files:
```cpp
// Before
#include "event_loop.hpp"
#include "pipeline_component.hpp"

// After
#include "dbn_pipe/stream/event_loop.hpp"
#include "dbn_pipe/stream/component.hpp"
```

## Documentation Updates

Update `docs/libuv-integration.md`:

```cpp
// Before
#include "event_loop.hpp"

// After
#include "dbn_pipe/stream.hpp"
```

Error handling unchanged - `dbn_pipe::Error` stays the same.

## Implementation Order

1. Create `lib/stream/` directory and `BUILD.bazel`
2. Move `error.hpp` to `lib/stream/`
3. Move `buffer_chain.hpp` to `lib/stream/`
4. Move `event_loop.hpp/.cpp` to `lib/stream/`
5. Move and merge `reactor.hpp/.cpp` + `epoll_event_loop.hpp/.cpp` to `lib/stream/`
6. Move `pipeline_component.hpp` to `lib/stream/component.hpp`
7. Move `tcp_socket.hpp` to `lib/stream/`
8. Create `src/stream.hpp` re-export header
9. Update src/ includes to use `lib/stream/...`
10. Update tests includes
11. Update docs/libuv-integration.md
12. Delete old files from src/
13. Run tests and verify
