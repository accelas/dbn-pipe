# Unified Pipeline Design

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Unify LiveClient and HistoricalClient with a common Pipeline template, reducing code duplication while preserving protocol-specific behavior.

**Architecture:** Pipeline<Protocol> template with explicit ProtocolDriver hooks that match actual connection flows.

**Tech Stack:** C++23, concepts, CRTP

**Lifecycle:** Single-use. For reconnect, create a new Pipeline instance.

---

## Design Overview

Based on Codex review feedback, the design uses explicit hooks rather than hiding complexity behind a single `Start()` method.

### Core Insight

The two protocols have fundamentally different flows:

**Live:**
- TCP connect → protocol starts immediately → Subscribe → StartStreaming

**Historical:**
- TCP connect → TLS handshake → (wait for completion) → HTTP request → stream

The Pipeline must accommodate these different flows through explicit hooks.

---

## 1. ProtocolDriver Concept

```cpp
template <typename P>
concept ProtocolDriver = requires {
    // Request type for this protocol
    typename P::Request;

    // Build the component chain, returns entry point
    // Signature: shared_ptr<Chain> BuildChain(Reactor&, Sink&, const string& api_key)
    typename P::ChainType;

    // Required static methods
    requires requires(
        Reactor& reactor,
        Sink& sink,
        const std::string& api_key,
        TcpSocket& tcp,
        std::shared_ptr<typename P::ChainType> chain,
        const typename P::Request& request,
        std::pmr::vector<std::byte> data
    ) {
        // Build component chain
        { P::BuildChain(reactor, sink, api_key) }
            -> std::same_as<std::shared_ptr<typename P::ChainType>>;

        // Wire TCP write callbacks to chain
        { P::WireTcp(tcp, chain) } -> std::same_as<void>;

        // Handle TCP connect event - returns true if ready to send request immediately
        // Live: returns true (send on connect)
        // Historical: returns false (must wait for TLS handshake)
        { P::OnConnect(chain) } -> std::same_as<bool>;

        // Handle TCP read - returns true if ready to send request
        // Live: always returns true (already ready)
        // Historical: returns true after TLS handshake completes
        { P::OnRead(chain, std::move(data)) } -> std::same_as<bool>;

        // Send the protocol-specific request
        { P::SendRequest(chain, request) } -> std::same_as<void>;

        // Teardown chain components
        { P::Teardown(chain) } -> std::same_as<void>;
    };
};
```

---

## 2. Protocol Implementations

### LiveProtocol

```cpp
struct LiveRequest {
    std::string dataset;
    std::string symbols;
    std::string schema;
};

struct LiveProtocol {
    using Request = LiveRequest;
    using ParserType = DbnParserComponent<Sink>;
    using ChainType = CramAuth<ParserType>;  // Renamed from LiveProtocolHandler

    static std::shared_ptr<ChainType> BuildChain(
        Reactor& reactor, Sink& sink, const std::string& api_key
    ) {
        auto parser = std::make_shared<ParserType>(sink);
        return ChainType::Create(reactor, parser, api_key);
    }

    static void WireTcp(TcpSocket& tcp, std::shared_ptr<ChainType>& chain) {
        chain->SetWriteCallback([&tcp](std::pmr::vector<std::byte> data) {
            tcp.Write(std::span<const std::byte>(data.data(), data.size()));
        });
    }

    static bool OnConnect(std::shared_ptr<ChainType>&) {
        // Live protocol is ready immediately on connect
        return true;
    }

    static bool OnRead(std::shared_ptr<ChainType>& chain,
                       std::pmr::vector<std::byte> data) {
        chain->Read(std::move(data));
        // Live is always ready after connect (auth happens in protocol)
        return true;
    }

    static void SendRequest(std::shared_ptr<ChainType>& chain,
                           const Request& req) {
        chain->Subscribe(req.dataset, req.symbols, req.schema);
        chain->StartStreaming();
    }

    static void Teardown(std::shared_ptr<ChainType>& chain) {
        if (chain) chain->Close();
    }
};
```

### HistoricalProtocol

```cpp
struct HistoricalRequest {
    std::string dataset;
    std::string symbols;
    std::string schema;
    std::uint64_t start;
    std::uint64_t end;
};

struct HistoricalProtocol {
    using Request = HistoricalRequest;
    using ParserType = DbnParserComponent<Sink>;
    using ZstdType = ZstdDecompressor<ParserType>;
    using HttpType = HttpClient<ZstdType>;
    using TlsType = TlsSocket<HttpType>;

    // Wrapper to hold both TLS chain and api_key for HTTP auth
    struct ChainType {
        std::shared_ptr<TlsType> tls;
        std::string api_key;

        void StartHandshake() { tls->StartHandshake(); }
        bool IsHandshakeComplete() const { return tls->IsHandshakeComplete(); }
        void Read(std::pmr::vector<std::byte> data) { tls->Read(std::move(data)); }
        void Write(std::pmr::vector<std::byte> data) { tls->Write(std::move(data)); }
        void Close() { if (tls) tls->Close(); }
        void SetUpstreamWriteCallback(auto&& cb) { tls->SetUpstreamWriteCallback(std::forward<decltype(cb)>(cb)); }
    };

    static std::shared_ptr<ChainType> BuildChain(
        Reactor& reactor, Sink& sink, const std::string& api_key
    ) {
        auto parser = std::make_shared<ParserType>(sink);
        auto zstd = ZstdType::Create(reactor, parser);
        auto http = HttpType::Create(reactor, zstd);
        auto tls = TlsType::Create(reactor, http);
        tls->SetHostname("hist.databento.com");

        auto chain = std::make_shared<ChainType>();
        chain->tls = tls;
        chain->api_key = api_key;
        return chain;
    }

    static void WireTcp(TcpSocket& tcp, std::shared_ptr<ChainType>& chain) {
        chain->SetUpstreamWriteCallback([&tcp](std::pmr::vector<std::byte> data) {
            if (tcp.IsConnected()) {
                tcp.Write(std::span<const std::byte>(data.data(), data.size()));
            }
        });
    }

    static bool OnConnect(std::shared_ptr<ChainType>& chain) {
        chain->StartHandshake();
        // Not ready yet - must wait for TLS handshake to complete
        return false;
    }

    static bool OnRead(std::shared_ptr<ChainType>& chain,
                       std::pmr::vector<std::byte> data) {
        chain->Read(std::move(data));
        // Ready to send request only after TLS handshake completes
        return chain->IsHandshakeComplete();
    }

    static void SendRequest(std::shared_ptr<ChainType>& chain,
                           const Request& req) {
        // Build and send HTTP request through TLS with auth
        std::string http_request = BuildHttpRequest(req, chain->api_key);
        std::pmr::vector<std::byte> buffer;
        buffer.resize(http_request.size());
        std::memcpy(buffer.data(), http_request.data(), http_request.size());
        chain->Write(std::move(buffer));
    }

    static void Teardown(std::shared_ptr<ChainType>& chain) {
        if (chain) chain->Close();
    }

private:
    // URL-encode a string (RFC 3986)
    static std::string UrlEncode(const std::string& s) {
        std::string result;
        result.reserve(s.size() * 3);  // Worst case
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                result += c;
            } else {
                char buf[4];
                std::snprintf(buf, sizeof(buf), "%%%02X", c);
                result += buf;
            }
        }
        return result;
    }

    // Build HTTP GET request with Basic auth header
    static std::string BuildHttpRequest(const Request& req,
                                        const std::string& api_key) {
        // Format: GET /v0/timeseries.get_range?... HTTP/1.1
        // Authorization: Basic base64(api_key:)
        // All string parameters are URL-encoded for safety
        std::string path = "/v0/timeseries.get_range?"
            "dataset=" + UrlEncode(req.dataset) +
            "&symbols=" + UrlEncode(req.symbols) +
            "&schema=" + UrlEncode(req.schema) +
            "&start=" + std::to_string(req.start) +
            "&end=" + std::to_string(req.end) +
            "&encoding=dbn&compression=zstd";

        std::string auth = Base64Encode(api_key + ":");

        return "GET " + path + " HTTP/1.1\r\n"
               "Host: hist.databento.com\r\n"
               "Authorization: Basic " + auth + "\r\n"
               "Accept-Encoding: zstd\r\n"
               "Connection: close\r\n"
               "\r\n";
    }
};
```

---

## 3. Pipeline Template

```cpp
template <ProtocolDriver P>
class Pipeline : public Suspendable,
                 public PipelineBase,
                 public std::enable_shared_from_this<Pipeline<P>> {
    // Private constructor - must use Create() for shared_ptr safety
    // (weak_from_this requires shared_ptr ownership)
    struct PrivateTag {};

public:
    using Request = typename P::Request;

    // Factory method required for shared_from_this safety
    static std::shared_ptr<Pipeline> Create(Reactor& reactor, std::string api_key) {
        return std::make_shared<Pipeline>(PrivateTag{}, reactor, std::move(api_key));
    }

    // Constructor is "public" for make_shared but requires PrivateTag
    Pipeline(PrivateTag, Reactor& reactor, std::string api_key)
        : reactor_(reactor), api_key_(std::move(api_key)) {}

    // Destructor must run on reactor thread because DoTeardown touches
    // TcpSocket and chain components that have reactor-thread affinity.
    // Ensure the last shared_ptr release happens on reactor thread.
    ~Pipeline() {
        assert(reactor_.IsInReactorThread());
        // Invalidate sink first to prevent callbacks during teardown
        // (P::Teardown might trigger Close() which emits OnComplete)
        if (sink_) sink_->Invalidate();
        DoTeardown();  // Synchronous cleanup
    }

    // All public methods must be called from reactor thread.
    // This is enforced with assertions in debug builds.

    void SetRequest(Request params) {
        assert(reactor_.IsInReactorThread());
        request_ = std::move(params);
        request_set_ = true;
    }

    void Connect(const sockaddr_storage& addr) {
        assert(reactor_.IsInReactorThread());

        // Terminal state guard - can't connect after teardown or terminal error
        if (teardown_pending_ || state_ == State::Error ||
            state_ == State::Complete) {
            return;  // Silently ignore - pipeline is done
        }

        if (connected_) {
            // Invalid state is terminal - use HandlePipelineError for consistency
            HandlePipelineError(Error{ErrorCode::InvalidState,
                                      "Connect() can only be called once"});
            return;
        }
        BuildPipeline();
        state_ = State::Connecting;
        tcp_->Connect(addr);
    }

    void Start() {
        assert(reactor_.IsInReactorThread());

        // Terminal state guard - pipeline is single-use
        // Note: Disconnected is NOT terminal - it's the initial state.
        // teardown_pending_ distinguishes "initial" from "stopped".
        if (teardown_pending_ || state_ == State::Error ||
            state_ == State::Complete) {
            return;  // Silently ignore - pipeline is done
        }

        if (!request_set_) {
            HandlePipelineError(Error{ErrorCode::InvalidState,
                                      "SetRequest() must be called before Start()"});
            return;
        }
        if (!connected_) {
            HandlePipelineError(Error{ErrorCode::InvalidState,
                                      "Connect() must be called before Start()"});
            return;
        }

        if (ready_to_send_ && !request_sent_) {
            P::SendRequest(chain_, request_);
            request_sent_ = true;
            state_ = State::Streaming;
        }
        // If not ready yet, Start() is a no-op - request will be sent
        // when OnRead returns true (handshake complete)
        start_requested_ = true;
    }

    void Stop() {
        assert(reactor_.IsInReactorThread());
        TeardownPipeline();
        state_ = State::Disconnected;
    }

    // Suspendable interface - uses count-based semantics
    void Suspend() override {
        assert(reactor_.IsInReactorThread());
        if (++suspend_count_ == 1 && tcp_) {
            tcp_->DisableRead();
        }
    }

    void Resume() override {
        assert(reactor_.IsInReactorThread());
        assert(suspend_count_ > 0);
        if (--suspend_count_ == 0 && tcp_) {
            tcp_->EnableRead();
        }
    }

    void Close() override {
        assert(reactor_.IsInReactorThread());
        TeardownPipeline();
    }

    bool IsSuspended() const override {
        return suspend_count_.load(std::memory_order_acquire) > 0;
    }

    // Callbacks - must be set from reactor thread
    template <typename H> void OnRecord(H&& h) {
        assert(reactor_.IsInReactorThread());
        record_handler_ = std::forward<H>(h);
    }
    template <typename H> void OnError(H&& h) {
        assert(reactor_.IsInReactorThread());
        error_handler_ = std::forward<H>(h);
    }
    template <typename H> void OnComplete(H&& h) {
        assert(reactor_.IsInReactorThread());
        complete_handler_ = std::forward<H>(h);
    }

    // State accessor - must be called from reactor thread
    State GetState() const {
        assert(reactor_.IsInReactorThread());
        return state_;
    }

private:
    void BuildPipeline() {
        // Pipeline is single-use - assert Connect() called only once
        assert(!connected_ && "Pipeline::Connect() can only be called once");
        connected_ = true;

        // Create sink (bridges to callbacks)
        sink_ = std::make_shared<Sink>(reactor_, this);

        // Build protocol-specific chain
        chain_ = P::BuildChain(reactor_, *sink_, api_key_);

        // Create TCP socket
        tcp_ = std::make_unique<TcpSocket>(reactor_);

        // Wire TCP callbacks
        tcp_->OnConnect([this] { HandleConnect(); });
        tcp_->OnRead([this](auto data) { HandleRead(data); });
        tcp_->OnError([this](auto ec) { HandleError(ec); });

        // Wire chain write callback to TCP
        P::WireTcp(*tcp_, chain_);
    }

    void TeardownPipeline() {
        // Invalidate sink immediately to stop callbacks
        if (sink_) sink_->Invalidate();

        // If already tearing down, skip
        if (teardown_pending_) return;
        teardown_pending_ = true;

        // Defer actual teardown to avoid reentrancy (may be called from callbacks)
        // Use weak_ptr to safely handle destruction before deferred call runs
        auto weak_self = this->weak_from_this();
        reactor_.Defer([weak_self] {
            if (auto self = weak_self.lock()) {
                self->DoTeardown();
            }
        });
    }

    void DoTeardown() {
        // Teardown chain before TCP to ensure no pending writes
        P::Teardown(chain_);
        chain_.reset();

        // Close TCP socket - clears callbacks
        if (tcp_) {
            tcp_->ClearCallbacks();
            tcp_->Close();
        }
        tcp_.reset();
        sink_.reset();
    }

    void HandleConnect() {
        assert(reactor_.IsInReactorThread());

        // Terminal state guard
        if (teardown_pending_) return;

        ready_to_send_ = P::OnConnect(chain_);

        // If ready immediately (Live) and Start() was called, send request now
        if (ready_to_send_ && start_requested_ && !request_sent_) {
            P::SendRequest(chain_, request_);
            request_sent_ = true;
            state_ = State::Streaming;
        }
    }

    void HandleRead(std::span<const std::byte> data) {
        assert(reactor_.IsInReactorThread());

        // Terminal state guard
        if (teardown_pending_) return;

        std::pmr::vector<std::byte> buffer(&pool_);
        buffer.assign(data.begin(), data.end());

        bool now_ready = P::OnRead(chain_, std::move(buffer));

        // If just became ready and Start() was already called, send request
        if (now_ready && !ready_to_send_) {
            ready_to_send_ = true;
            if (start_requested_ && !request_sent_) {
                P::SendRequest(chain_, request_);
                request_sent_ = true;
                state_ = State::Streaming;
            }
        }
    }

    void HandleError(std::error_code ec) {
        assert(reactor_.IsInReactorThread());
        state_ = State::Error;
        if (error_handler_) {
            error_handler_(Error{ErrorCode::ConnectionFailed, ec.message(), ec.value()});
        }
        // Error is terminal for single-use pipeline
        TeardownPipeline();
    }

    // Called by Sink - must be on reactor thread
    void HandleRecord(const databento::Record& rec) override {
        assert(reactor_.IsInReactorThread());
        if (record_handler_) record_handler_(rec);
    }

    void HandlePipelineError(const Error& e) override {
        assert(reactor_.IsInReactorThread());
        state_ = State::Error;
        if (error_handler_) error_handler_(e);
        // Protocol errors are also terminal
        TeardownPipeline();
    }

    void HandlePipelineComplete() override {
        assert(reactor_.IsInReactorThread());
        state_ = State::Complete;
        if (complete_handler_) complete_handler_();
        // Completion is terminal
        TeardownPipeline();
    }

    friend class Sink;

    Reactor& reactor_;
    std::string api_key_;
    Request request_;
    State state_ = State::Disconnected;

    bool connected_ = false;        // Single-use guard
    bool request_set_ = false;      // SetRequest() called
    bool start_requested_ = false;
    bool request_sent_ = false;
    bool ready_to_send_ = false;
    bool teardown_pending_ = false; // Deferred teardown scheduled (weak_ptr guards lifetime)

    std::atomic<int> suspend_count_{0};

    std::unique_ptr<TcpSocket> tcp_;
    std::shared_ptr<Sink> sink_;
    std::shared_ptr<typename P::ChainType> chain_;

    std::pmr::unsynchronized_pool_resource pool_;

    std::function<void(const databento::Record&)> record_handler_;
    std::function<void(const Error&)> error_handler_;
    std::function<void()> complete_handler_;
};
```

---

## 4. Sink Class

```cpp
// Non-templated base for type erasure
class PipelineBase {
public:
    virtual ~PipelineBase() = default;
    virtual void HandleRecord(const databento::Record& rec) = 0;
    virtual void HandlePipelineError(const Error& e) = 0;
    virtual void HandlePipelineComplete() = 0;
};

// Thread-safety: ALL Sink methods must be called from reactor thread.
// This is enforced with assertions and by Pipeline's reactor-thread contract.
// No atomics needed since single-threaded access is guaranteed.
class Sink {
public:
    Sink(Reactor& reactor, PipelineBase* pipeline)
        : reactor_(reactor), pipeline_(pipeline) {}

    // Invalidate clears valid_ and nulls pipeline_.
    // Called from TeardownPipeline before deferred cleanup.
    void Invalidate() {
        assert(reactor_.IsInReactorThread());
        valid_ = false;
        pipeline_ = nullptr;
    }

    // RecordSink interface - only called from reactor thread
    void OnData(RecordBatch&& batch) {
        assert(reactor_.IsInReactorThread());
        if (!valid_ || !pipeline_) return;
        for (const auto& rec : batch.records) {
            pipeline_->HandleRecord(rec);
        }
    }

    void OnError(const Error& e) {
        assert(reactor_.IsInReactorThread());
        if (!valid_ || !pipeline_) return;
        pipeline_->HandlePipelineError(e);
    }

    void OnComplete() {
        assert(reactor_.IsInReactorThread());
        if (!valid_ || !pipeline_) return;
        pipeline_->HandlePipelineComplete();
    }

private:
    Reactor& reactor_;
    PipelineBase* pipeline_;
    bool valid_ = true;
};

// Pipeline inherits from PipelineBase
template <ProtocolDriver P>
class Pipeline : public Suspendable, public PipelineBase {
    // ... implementation above ...
};
```

---

## 5. Type Aliases for Backwards Compatibility

```cpp
using LiveClient = Pipeline<LiveProtocol>;
using HistoricalClient = Pipeline<HistoricalProtocol>;
```

---

## Summary of Changes from Original Design

| Issue | Fix |
|-------|-----|
| Protocol concept incomplete | Added full requirements with BuildChain, WireTcp, OnConnect, OnRead, SendRequest, Teardown |
| No TcpSocket access | Added WireTcp hook that receives TcpSocket reference |
| Live needs send-on-connect | OnConnect returns bool - Live returns true, Historical returns false |
| Historical handshake timing | OnRead returns bool indicating readiness; Pipeline tracks and sends request when ready |
| HttpClient::Write unsupported | HistoricalProtocol::SendRequest writes through TlsSocket |
| Historical auth missing | ChainType wrapper stores api_key; BuildHttpRequest uses Basic auth header |
| Sink takes shared_ptr | Changed to reference (Sink&) |
| Sink invalidation missing | Added Invalidate() in TeardownPipeline |
| Sink thread safety | Reactor-thread contract eliminates need for atomics; valid_ nulled on Invalidate |
| Boolean suspend | Changed to count-based atomic<int> |
| Single-use lifecycle | Pipeline is single-use; `connected_` flag enforces single Connect() |
| Callback lifetime risk | TeardownPipeline calls ClearCallbacks() before tcp_.reset() |
| Protocol errors terminal | HandlePipelineError also calls TeardownPipeline |
| Request validation | `request_set_` flag + runtime check in Start() ensures SetRequest() called |
| Connect() invalid-state not terminal | Now calls HandlePipelineError which triggers TeardownPipeline |
| Public constructor with weak_from_this | Private constructor via PrivateTag; must use Create() factory |
| Missing reactor thread assertions | Added assert(IsInReactorThread()) to all public methods, callback setters, handlers, and destructor |
| Terminal state guards missing | Start/HandleConnect/HandleRead check teardown_pending_ before operating |
| Destructor skipped sink invalidation | ~Pipeline() invalidates sink before DoTeardown to prevent callbacks during teardown |
| HTTP params not URL-encoded | Added UrlEncode() helper, all string params encoded in BuildHttpRequest |

---

## Implementation Order

1. Rename LiveProtocolHandler → CramAuth
2. Create Sink class with PipelineBase
3. Create ProtocolDriver concept
4. Implement LiveProtocol driver
5. Implement HistoricalProtocol driver
6. Create Pipeline<P> template
7. Add type aliases
8. Update tests
9. Remove old LiveClient/HistoricalClient
