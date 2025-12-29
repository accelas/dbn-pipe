# Unified Pipeline Design

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Unify LiveClient and HistoricalClient with a common Pipeline template, reducing code duplication while preserving protocol-specific behavior.

**Architecture:** Pipeline<Protocol> template with explicit ProtocolDriver hooks that match actual connection flows.

**Tech Stack:** C++23, concepts, CRTP

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
    using ChainType = TlsType;  // Entry point is TLS layer

    static std::shared_ptr<ChainType> BuildChain(
        Reactor& reactor, Sink& sink, const std::string& api_key
    ) {
        auto parser = std::make_shared<ParserType>(sink);
        auto zstd = ZstdType::Create(reactor, parser);
        auto http = HttpType::Create(reactor, zstd);
        auto tls = TlsType::Create(reactor, http);
        tls->SetHostname("hist.databento.com");
        return tls;
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
        // Build and send HTTP request through TLS
        std::string http_request = BuildHttpRequest(req);
        std::pmr::vector<std::byte> buffer;
        buffer.resize(http_request.size());
        std::memcpy(buffer.data(), http_request.data(), http_request.size());
        chain->Write(std::move(buffer));
    }

    static void Teardown(std::shared_ptr<ChainType>& chain) {
        if (chain) chain->Close();
    }

private:
    static std::string BuildHttpRequest(const Request& req);
};
```

---

## 3. Pipeline Template

```cpp
template <ProtocolDriver P>
class Pipeline : public Suspendable {
public:
    using Request = typename P::Request;

    Pipeline(Reactor& reactor, std::string api_key)
        : reactor_(reactor), api_key_(std::move(api_key)) {}

    ~Pipeline() {
        TeardownPipeline();
    }

    void SetRequest(Request params) {
        request_ = std::move(params);
    }

    void Connect(const sockaddr_storage& addr) {
        BuildPipeline();
        state_ = State::Connecting;
        tcp_->Connect(addr);
    }

    void Start() {
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
        TeardownPipeline();
    }

    bool IsSuspended() const override {
        return suspend_count_.load(std::memory_order_acquire) > 0;
    }

    // Callbacks
    template <typename H> void OnRecord(H&& h) { record_handler_ = std::forward<H>(h); }
    template <typename H> void OnError(H&& h) { error_handler_ = std::forward<H>(h); }
    template <typename H> void OnComplete(H&& h) { complete_handler_ = std::forward<H>(h); }

    // State accessor
    State GetState() const { return state_; }

private:
    void BuildPipeline() {
        // Reset state for potential reconnect
        start_requested_ = false;
        request_sent_ = false;
        ready_to_send_ = false;
        suspend_count_.store(0, std::memory_order_release);

        // Create sink (bridges to callbacks)
        sink_ = std::make_shared<Sink>(this);

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
        // Invalidate sink first to stop callbacks
        if (sink_) sink_->Invalidate();

        // Teardown chain before TCP to ensure no pending writes
        P::Teardown(chain_);
        chain_.reset();

        // Close TCP socket last - clears callbacks to avoid use-after-free
        if (tcp_) {
            tcp_->ClearCallbacks();  // Clear OnConnect/OnRead/OnError handlers
            tcp_->Close();
        }
        tcp_.reset();
        sink_.reset();
    }

    void HandleConnect() {
        ready_to_send_ = P::OnConnect(chain_);

        // If ready immediately (Live) and Start() was called, send request now
        if (ready_to_send_ && start_requested_ && !request_sent_) {
            P::SendRequest(chain_, request_);
            request_sent_ = true;
            state_ = State::Streaming;
        }
    }

    void HandleRead(std::span<const std::byte> data) {
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
        state_ = State::Error;
        if (error_handler_) {
            error_handler_(Error{ErrorCode::ConnectionFailed, ec.message(), ec.value()});
        }
    }

    // Called by Sink
    void HandleRecord(const databento::Record& rec) {
        if (record_handler_) record_handler_(rec);
    }

    void HandlePipelineError(const Error& e) {
        state_ = State::Error;
        if (error_handler_) error_handler_(e);
    }

    void HandlePipelineComplete() {
        state_ = State::Complete;
        if (complete_handler_) complete_handler_();
    }

    friend class Sink;

    Reactor& reactor_;
    std::string api_key_;
    Request request_;
    State state_ = State::Disconnected;

    bool start_requested_ = false;
    bool request_sent_ = false;
    bool ready_to_send_ = false;

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

class Sink {
public:
    explicit Sink(PipelineBase* pipeline) : pipeline_(pipeline) {}

    void Invalidate() { valid_ = false; }

    // RecordSink interface
    void OnData(RecordBatch&& batch) {
        if (!valid_ || !pipeline_) return;
        for (const auto& rec : batch.records) {
            pipeline_->HandleRecord(rec);
        }
    }

    void OnError(const Error& e) {
        if (!valid_ || !pipeline_) return;
        pipeline_->HandlePipelineError(e);
    }

    void OnComplete() {
        if (!valid_ || !pipeline_) return;
        pipeline_->HandlePipelineComplete();
    }

private:
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
| Sink takes shared_ptr | Changed to reference (Sink&) |
| Sink invalidation missing | Added Invalidate() in TeardownPipeline |
| Boolean suspend | Changed to count-based atomic<int> |
| Reconnect state not reset | BuildPipeline resets all state flags |
| Callback lifetime risk | TeardownPipeline calls ClearCallbacks() before tcp_.reset() |
| Request validation | Can add static P::Validate(request) hook if needed |

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
