# TcpSocket Zero-Copy Design

## Problem

TcpSocket reads into a fixed 64KB buffer and delivers `span<const std::byte>` to callbacks. The unified pipeline copies this span into `pmr::vector<std::byte>` before passing to protocol chains, causing an unnecessary copy for every read.

## Solution

TcpSocket reads directly into Segment buffers from a SegmentPool and delivers `BufferChain` to callbacks. Downstream components pass the chain through without copying.

## Interface Changes

### TcpSocket

```cpp
// Before
using ReadCallback = std::function<void(std::span<const std::byte>)>;
std::vector<std::byte> read_buffer_;  // Fixed 64KB, reused

// After
using ReadCallback = std::function<void(BufferChain)>;
SegmentPool segment_pool_{4};  // Pool for read segments
```

### ProtocolDriver Concept

```cpp
// Before
{ P::OnRead(chain, std::move(data)) } -> std::same_as<bool>;  // pmr::vector<byte>

// After
{ P::OnRead(chain, std::move(data)) } -> std::same_as<bool>;  // BufferChain
```

### CramAuth

Changes from `Downstream` to `ChainSink` concept for downstream. In streaming mode, forwards BufferChain directly (zero-copy). In text mode (auth handshake), extracts bytes for line parsing (acceptable - small data, once per connection).

## Files to Modify

| File | Change |
|------|--------|
| `tcp_socket.hpp/cpp` | Read into Segments, deliver BufferChain |
| `protocol_driver.hpp` | OnRead concept takes BufferChain |
| `unified_pipeline.hpp` | HandleRead takes BufferChain, no copy |
| `live_protocol.hpp` | OnRead forwards BufferChain |
| `historical_protocol.hpp` | OnRead forwards BufferChain |
| `cram_auth.hpp` | Read(BufferChain), ChainSink downstream |
| `tls_socket.hpp` | Read(BufferChain) input |
| `tcp_socket_test.cpp` | Update callback signatures |

## Data Flow

```
Kernel -> TcpSocket(Segment) -> BufferChain -> Protocol -> ... -> DbnParser
           ^                                                          |
           |_________________ segment recycling ______________________|
```

## Result

Zero-copy from kernel to parsing for both Live and Historical paths:
- Live: TCP -> CramAuth(streaming) -> DbnParser
- Historical: TCP -> TLS -> HTTP -> Zstd -> DbnParser
