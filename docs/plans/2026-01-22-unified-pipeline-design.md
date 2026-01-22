# Unified Pipeline Design v2

**Goal:** Unify API pipeline with Historical/Live pipelines under a single `Pipeline<Protocol>` template with flexible sink types.

**Key Insight:** The sink should be a generic concept - the terminal component that consumes data flow. Different protocols need different sinks (streaming records, single JSON result, file output).

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                        User Code                            │
│                                                             │
│  auto p = HistoricalProtocol::Create(loop, key, req,        │
│               on_data, on_error, on_complete);              │
│  p->Connect();                                              │
│  p->Start();                                                │
└─────────────────────────────────────────────────────────────┘
                            │
                ┌───────────┴───────────┐
                ▼                       ▼
┌───────────────────────────┐   ┌───────────────────────────┐
│   Protocol (static)       │   │   Pipeline (runtime)      │
│                           │   │                           │
│ • Create()                │   │ • Connect()               │
│ • BuildChain()            │   │ • Start()                 │
│ • SendRequest()           │   │ • Stop()                  │
│ • Teardown()              │   │ • Suspend()/Resume()      │
│ • Types: Sink, Chain      │   │                           │
└───────────────────────────┘   └───────────────────────────┘
                                          │
                                          ▼
                    ┌─────────────────────────────────────┐
                    │              Sink                   │
                    │                                     │
                    │  RecordSink:       ResultSink<T>:   │
                    │  • OnData()        • OnResult()     │
                    │  • OnError()       • OnError()→exp  │
                    │  • OnComplete()    • OnComplete()   │
                    │  • Invalidate()    • Invalidate()   │
                    └─────────────────────────────────────┘
                                          ▲
                                          │
                    ┌─────────────────────────────────────┐
                    │              Chain                  │
                    │  TcpSocket → Tls → Http → ... → Sink│
                    └─────────────────────────────────────┘
```

**Separation of concerns:**

| Component | Responsibility | Lifetime |
|-----------|---------------|----------|
| Protocol | Construction: creates sink, builds chain, wires callbacks | Stateless (static methods only) |
| Pipeline | Runtime: lifecycle orchestration (Connect, Start, Stop) | Owned by user via shared_ptr |
| Sink | Dispatch: receives data/errors from chain, invokes callbacks | Owned by Pipeline |

---

## Ownership Model

**Pipeline is the root object.** It owns both chain and sink, and outlives them.

```
User holds shared_ptr<Pipeline>
    └── Pipeline owns (via shared_ptr):
            ├── chain_  ──references──► sink (raw ref, safe because Pipeline outlives both)
            └── sink_
```

**Lifetime guarantees:**
- Pipeline destructor controls teardown order
- Chain can safely hold raw reference to sink (Pipeline guarantees sink outlives chain)
- No detached async operations may hold chain without Pipeline

**Teardown order:**
1. Invalidate sink (stops callbacks from firing)
2. Teardown chain (may trigger final events, but sink ignores them)
3. Destroy chain
4. Destroy sink

---

## Threading Model

All Pipeline public methods and sink callbacks must be called on the event loop thread.

**Enforcement:**
- `Pipeline` methods use `RequireLoopThread()` - fail-fast check that works in release builds
- Chain components only call sink from event loop callbacks
- `Invalidate()` uses atomic store with release semantics for cross-thread visibility

**Contract:**
- `Pipeline::Connect/Start/Stop/Suspend/Resume` - event loop thread only (enforced, terminates if violated)
- `Pipeline::IsSuspended()` - thread-safe, can be called from any thread
- `OnData`/`OnResult`, `OnError`, `OnComplete` - called on event loop thread only
- `Invalidate()` - may be called from any thread (safe for destructor scenarios)

---

## 1. Sink Concept

All sinks implement lifecycle methods. Data methods vary by sink type.

```cpp
// Base sink concept - lifecycle methods required for all sinks
template<typename S>
concept Sink = requires(S& s, const Error& e) {
    { s.OnError(e) } -> std::same_as<void>;
    { s.OnComplete() } -> std::same_as<void>;
    { s.Invalidate() } -> std::same_as<void>;
};

// Streaming sink - receives batches of records
template<typename S>
concept StreamingSink = Sink<S> && requires(S& s, RecordBatch&& batch) {
    { s.OnData(std::move(batch)) } -> std::same_as<void>;
};

// Single-result sink - receives one result (success or error via expected)
template<typename S>
concept SingleResultSink = Sink<S> && requires(S& s) {
    typename S::ResultType;
    { s.OnResult(std::declval<typename S::ResultType>()) } -> std::same_as<void>;
};
```

**Behavioral contracts:**
- `OnData`/`OnResult`: May be called zero or more times (streaming) or exactly once (single-result)
- `OnError`: Terminal for streaming sinks; for single-result, delivers error via `OnResult(unexpected(e))`
- `OnComplete`: Terminal signal; no-op for single-result (result already delivered)
- `Invalidate`: Makes all subsequent calls no-ops; safe to call multiple times

---

## 2. Sink Implementations

### RecordSink (streaming protocols)

```cpp
class RecordSink {
public:
    RecordSink(
        std::function<void(RecordBatch&&)> on_data,
        std::function<void(const Error&)> on_error,
        std::function<void()> on_complete
    ) : on_data_(std::move(on_data)),
        on_error_(std::move(on_error)),
        on_complete_(std::move(on_complete)) {}

    void OnData(RecordBatch&& batch) {
        if (valid_) on_data_(std::move(batch));
    }

    void OnError(const Error& e) {
        if (valid_) on_error_(e);
    }

    void OnComplete() {
        if (valid_) on_complete_();
    }

    void Invalidate() { valid_.store(false, std::memory_order_release); }

private:
    std::function<void(RecordBatch&&)> on_data_;
    std::function<void(const Error&)> on_error_;
    std::function<void()> on_complete_;
    std::atomic<bool> valid_{true};
};
```

### ResultSink (single-result protocols)

```cpp
template<typename Result>
class ResultSink {
public:
    using ResultType = Result;

    ResultSink(std::function<void(std::expected<Result, Error>)> on_result)
        : on_result_(std::move(on_result)) {}

    void OnResult(Result&& result) {
        if (!valid_ || delivered_) return;
        delivered_ = true;
        on_result_(std::move(result));
    }

    void OnError(const Error& e) {
        if (!valid_ || delivered_) return;
        delivered_ = true;
        on_result_(std::unexpected(e));
    }

    void OnComplete() {
        // No-op for single-result - result already delivered
    }

    void Invalidate() { valid_.store(false, std::memory_order_release); }

private:
    std::function<void(std::expected<Result, Error>)> on_result_;
    std::atomic<bool> valid_{true};
    bool delivered_ = false;  // Ensures exactly-once delivery
};
```

**Behavioral notes:**
- `delivered_` flag ensures callback fires at most once
- Late errors (after result delivered) are silently dropped
- `OnComplete()` is no-op since result delivery implies completion

| Sink | OnData/OnResult | OnError | OnComplete |
|------|-----------------|---------|------------|
| RecordSink | Delivers batch to callback | Calls error callback | Calls complete callback |
| ResultSink | Delivers result to callback | Wraps in `unexpected`, delivers | No-op |

---

## 3. Protocol Concept

Protocol is a static factory that defines types and construction.

```cpp
// Forward declaration to break circular dependency
template<typename P> class Pipeline;

template<typename P>
concept Protocol = requires {
    // Required type aliases
    typename P::Request;
    typename P::SinkType;
    typename P::ChainType;

    // SinkType must satisfy Sink concept
    requires Sink<typename P::SinkType>;

    // Chain building (sink passed by reference - Pipeline owns sink)
    requires requires(
        IEventLoop& loop,
        typename P::SinkType& sink,
        const std::string& api_key
    ) {
        { P::BuildChain(loop, sink, api_key) }
            -> std::same_as<std::shared_ptr<typename P::ChainType>>;
    };

    // Chain operations
    requires requires(
        std::shared_ptr<typename P::ChainType>& chain,
        const typename P::Request& request
    ) {
        { P::SendRequest(chain, request) } -> std::same_as<void>;
        { P::Teardown(chain) } -> std::same_as<void>;
        { P::GetHostname(request) } -> std::convertible_to<std::string>;
        { P::GetPort(request) } -> std::convertible_to<uint16_t>;
    };
};

// Note: Create() is not in the concept because its signature varies per protocol
// (different callback parameters). Each protocol provides its own Create() factory.
```

**Circular dependency resolution:**
- `pipeline_fwd.hpp`: Forward declares `Pipeline<P>`
- Protocol headers include `pipeline_fwd.hpp` for return type
- `pipeline.hpp` includes protocol headers

---

## 4. Protocol Implementations

### HistoricalProtocol

```cpp
struct HistoricalProtocol {
    using Request = HistoricalRequest;
    using SinkType = RecordSink;
    using ChainType = TcpSocket<TlsTransport<HttpClient<
                        ZstdDecompressor<DbnParserComponent<RecordSink>>>>>;

    static auto Create(
        IEventLoop& loop,
        std::string api_key,
        Request request,
        std::function<void(RecordBatch&&)> on_data,
        std::function<void(const Error&)> on_error,
        std::function<void()> on_complete
    ) -> std::shared_ptr<Pipeline<HistoricalProtocol>>
    {
        auto sink = std::make_shared<RecordSink>(
            std::move(on_data), std::move(on_error), std::move(on_complete));
        auto chain = BuildChain(loop, *sink, api_key);
        return std::make_shared<Pipeline<HistoricalProtocol>>(
            Pipeline<HistoricalProtocol>::PrivateTag{},
            loop, std::move(chain), std::move(sink), std::move(request));
    }

    static auto BuildChain(IEventLoop& loop, RecordSink& sink,
                           const std::string& api_key)
        -> std::shared_ptr<ChainType>;

    static void SendRequest(std::shared_ptr<ChainType>& chain, const Request& req);
    static void Teardown(std::shared_ptr<ChainType>& chain);
    static std::string GetHostname(const Request&) { return "hist.databento.com"; }
    static uint16_t GetPort(const Request&) { return 443; }
};
```

### ApiProtocol

```cpp
template<typename Builder>
struct ApiProtocol {
    using Request = ApiRequest;
    using Result = typename Builder::Result;
    using SinkType = ResultSink<Result>;
    using ChainType = TcpSocket<TlsTransport<HttpClient<JsonParser<Builder>>>>;

    static auto Create(
        IEventLoop& loop,
        std::string api_key,
        Request request,
        std::function<void(std::expected<Result, Error>)> on_result
    ) -> std::shared_ptr<Pipeline<ApiProtocol<Builder>>>
    {
        auto sink = std::make_shared<SinkType>(std::move(on_result));
        auto chain = BuildChain(loop, *sink, api_key);
        return std::make_shared<Pipeline<ApiProtocol<Builder>>>(
            typename Pipeline<ApiProtocol<Builder>>::PrivateTag{},
            loop, std::move(chain), std::move(sink), std::move(request));
    }

    static auto BuildChain(IEventLoop& loop, SinkType& sink,
                           const std::string& api_key)
        -> std::shared_ptr<ChainType>;

    static void SendRequest(std::shared_ptr<ChainType>& chain, const Request& req);
    static void Teardown(std::shared_ptr<ChainType>& chain);
    static std::string GetHostname(const Request&) { return "hist.databento.com"; }
    static uint16_t GetPort(const Request&) { return 443; }
};
```

---

## 5. Pipeline (Runtime Shell)

Pipeline is a thin runtime wrapper - just lifecycle management.

```cpp
template<typename P>
    requires Protocol<P>
class Pipeline {
public:
    struct PrivateTag {};  // Force use of Protocol::Create()

    Pipeline(
        PrivateTag,
        IEventLoop& loop,
        std::shared_ptr<typename P::ChainType> chain,
        std::shared_ptr<typename P::SinkType> sink,
        typename P::Request request
    ) : loop_(loop),
        chain_(std::move(chain)),
        sink_(std::move(sink)),
        request_(std::move(request)) {}

    ~Pipeline() {
        DoTeardown();
    }

    // Lifecycle - the only public API
    void Connect() {
        RequireLoopThread(__func__);
        if (torn_down_) return;
        auto addr = ResolveHostname(P::GetHostname(request_), P::GetPort(request_));
        if (addr) Connect(*addr);
    }

    void Connect(const sockaddr_storage& addr) {
        RequireLoopThread(__func__);
        if (torn_down_) return;
        chain_->Connect(addr);
    }

    void Start() {
        RequireLoopThread(__func__);
        if (torn_down_) return;
        P::SendRequest(chain_, request_);
    }

    void Stop() {
        RequireLoopThread(__func__);
        DoTeardown();
    }

    // Backpressure
    void Suspend() {
        RequireLoopThread(__func__);
        if (!torn_down_) chain_->Suspend();
    }

    void Resume() {
        RequireLoopThread(__func__);
        if (!torn_down_) chain_->Resume();
    }

    bool IsSuspended() const {
        // Thread-safe read - can be called from any thread
        return chain_ && chain_->IsSuspended();
    }

private:
    friend P;

    // Fail-fast thread check - works in release builds
    void RequireLoopThread(const char* func) const {
        if (loop_.IsInEventLoopThread()) return;
        std::fprintf(stderr, "Pipeline::%s called off event loop thread\n", func);
        std::terminate();
    }

    // Idempotent teardown - safe to call multiple times
    void DoTeardown() {
        if (torn_down_) return;
        torn_down_ = true;

        // Order matters: invalidate sink first, then teardown chain
        if (sink_) sink_->Invalidate();
        if (chain_) P::Teardown(chain_);
    }

    IEventLoop& loop_;
    std::shared_ptr<typename P::ChainType> chain_;
    std::shared_ptr<typename P::SinkType> sink_;
    typename P::Request request_;
    bool torn_down_ = false;
};
```

**Key properties:**
- `requires Protocol<P>` enforces P satisfies the Protocol concept at instantiation
- `RequireLoopThread()` fail-fast check on all public methods (works in release builds)
- `IsSuspended()` is the exception - thread-safe atomic read, callable from any thread
- `DoTeardown()` is idempotent - safe to call from both `Stop()` and destructor
- Teardown order: invalidate sink → teardown chain (prevents late callbacks)
- All public methods guard against use after teardown

---

## 6. Usage Examples

### Streaming (Historical/Live)

```cpp
auto pipeline = HistoricalProtocol::Create(
    loop,
    api_key,
    HistoricalRequest{
        .dataset = "GLBX.MDP3",
        .symbols = "ESZ4",
        .schema = "mbp-1",
        .start = start_ns,
        .end = end_ns
    },
    [](RecordBatch&& batch) {
        for (const auto& rec : batch) {
            process(rec);
        }
    },
    [](const Error& e) {
        std::cerr << "Error: " << e.message << "\n";
    },
    []() {
        std::cout << "Done\n";
    }
);

pipeline->Connect();
pipeline->Start();
```

### Single Result (API)

```cpp
auto pipeline = ApiProtocol<MetadataBuilder>::Create(
    loop,
    api_key,
    ApiRequest{
        .method = "GET",
        .path = "/v0/metadata.get_dataset_range",
        .query_params = {{"dataset", "GLBX.MDP3"}}
    },
    [](std::expected<DatasetRange, Error> result) {
        if (result) {
            std::cout << "Range: " << result->start << " to " << result->end << "\n";
        } else {
            std::cerr << "Error: " << result.error().message << "\n";
        }
    }
);

pipeline->Connect();
pipeline->Start();
```

---

## 7. Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Sink abstraction | Concept with required lifecycle methods | Compiler enforces contract |
| Sink type ownership | Protocol-defined | Protocol knows its data model |
| Callback interface | Protocol-specific | `OnRecord` vs `OnResult` reads naturally |
| Callback timing | At construction | No invalid states |
| Construction | Protocol owns | Protocol knows how to wire everything |
| Runtime | Pipeline owns | Thin shell, just lifecycle |
| Error dispatch | Sink handles | No `is_streaming` flag needed |
| Streaming delivery | Batch only | User iterates if needed |
| Code location | Reusable components in `lib/stream/`, domain-specific in `src/` | Future extraction as standalone library |
| Thread enforcement | Fail-fast `RequireLoopThread()` | Works in release builds, clear contract |

---

## 8. Migration Path

1. Create `RecordSink` and `ResultSink` classes
2. Update `HistoricalProtocol` to use new pattern
3. Update `LiveProtocol` to use new pattern
4. Create `ApiProtocol<Builder>` replacing `ApiPipeline`
5. Remove old `ApiPipeline` and `ApiPipelineBase`
6. Remove `Sink<Record>` (replaced by `RecordSink`)
7. Update tests

---

## 9. Files to Modify

Generic streaming components go in `lib/stream/` for future extraction as standalone library.
Protocol-specific code stays in `src/`.

| File | Change |
|------|--------|
| `lib/stream/pipeline_fwd.hpp` | **New:** Forward declaration of `Pipeline<P>` |
| `lib/stream/sink.hpp` | **New:** `Sink` concept, `RecordSink`, `ResultSink` |
| `lib/stream/pipeline.hpp` | **New:** Generic `Pipeline<P>` runtime shell |
| `lib/stream/tls_transport.hpp` | **Move:** From `src/tls_transport.hpp` |
| `lib/stream/http_client.hpp` | **Move:** From `src/http_client.hpp` |
| `lib/stream/json_parser.hpp` | **Move:** From `src/api/json_parser.hpp` |
| `lib/stream/zstd_decompressor.hpp` | **Move:** From `src/zstd_decompressor.hpp` |
| `lib/stream/url_encode.hpp` | **Move:** From `src/api/url_encode.hpp` |
| `src/protocol_driver.hpp` | Update concept, add `BuildChain` requirement |
| `src/historical_protocol.hpp` | Add `Create()` factory |
| `src/live_protocol.hpp` | Add `Create()` factory |
| `src/api_protocol.hpp` | **New:** Replace `ApiPipeline` with `ApiProtocol<Builder>` |
| `src/api/metadata_client.hpp` | Update imports |
| `src/api/symbology_client.hpp` | Update imports |
| `src/pipeline_sink.hpp` | **Remove:** Replaced by `lib/stream/sink.hpp` |
| `src/pipeline.hpp` | **Remove:** Replaced by `lib/stream/pipeline.hpp` |
| `src/api/api_pipeline.hpp` | **Remove:** Replaced by `src/api_protocol.hpp` |

**Directory structure after migration:**

```
lib/stream/
├── component.hpp        # (existing) PipelineComponent base
├── pipeline_fwd.hpp     # Forward declaration
├── pipeline.hpp         # Generic Pipeline<P> template
├── sink.hpp             # Sink concepts and implementations
├── tcp_socket.hpp       # (existing) TCP socket component
├── tls_transport.hpp    # TLS/SSL transport
├── http_client.hpp      # HTTP client component
├── json_parser.hpp      # JSON SAX parser component
├── zstd_decompressor.hpp # Zstd decompression component
├── url_encode.hpp       # URL/Base64 encoding utilities
├── buffer_chain.hpp     # (existing) Zero-copy buffer
├── ...

src/
├── protocol_driver.hpp      # Protocol concept
├── historical_protocol.hpp  # HistoricalProtocol
├── live_protocol.hpp        # LiveProtocol
├── api_protocol.hpp         # ApiProtocol<Builder>
├── dbn_parser_component.hpp # DBN-specific parser
├── cram_auth.hpp            # CRAM auth (protocol-specific)
├── api/
│   ├── metadata_client.hpp  # High-level metadata API client
│   └── symbology_client.hpp # High-level symbology API client
└── ...
```

**Separation principle:**
- `lib/stream/` - Reusable components: networking, HTTP, TLS, compression, parsing, encoding
- `src/` - Domain-specific: protocols, DBN parsing, CRAM auth
- `src/api/` - High-level Databento API clients (convenience wrappers)

---

## 10. Design Review Issues Addressed

Issues identified during Codex review and their resolutions:

| Issue | Resolution |
|-------|------------|
| Lifetime safety (chain references sink) | Pipeline is root owner; outlives both chain and sink. Documented in Ownership Model section. |
| Double teardown hazard | `DoTeardown()` is idempotent with `torn_down_` guard. Safe to call from both `Stop()` and destructor. |
| `SingleResultSink` missing `OnResult` requirement | Added `OnResult` to concept definition. |
| `Protocol` concept missing `BuildChain` | Added `BuildChain` requirement to concept. |
| Pipeline doesn't constrain `P::SinkType` | Added `requires Sink<typename P::SinkType>` to Protocol concept. |
| Circular include issue | Resolved via `pipeline_fwd.hpp` forward declaration. |
| `enable_shared_from_this` unused | Removed - not needed for this design. |
| Late errors after successful result | `ResultSink` uses `delivered_` flag for exactly-once semantics. |
| Threading assumptions undocumented | Added Threading Model section with explicit contracts. |
| Thread safety not enforced in release | `RequireLoopThread()` fail-fast check replaces assert (works in release builds). |
