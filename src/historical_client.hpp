// src/historical_client.hpp
#pragma once

#include <netinet/in.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

#include <databento/record.hpp>

#include "dbn_parser_component.hpp"
#include "error.hpp"
#include "http_client.hpp"
#include "reactor.hpp"
#include "record_batch.hpp"
#include "tcp_socket.hpp"
#include "tls_socket.hpp"
#include "zstd_decompressor.hpp"

namespace databento_async {

// HistoricalClient fetches historical data from the Databento Historical API.
// Uses HTTPS (TLS) + HTTP + zstd decompression + DBN parsing pipeline.
//
// Architecture: TcpSocket -> TlsSocket -> HttpClient -> ZstdDecompressor -> DbnParserComponent -> Sink
//
// Lifecycle:
// 1. Construct with reactor and API key
// 2. Call Request() to set dataset, symbols, schema, start/end timestamps
// 3. Call Connect() to initiate TLS connection
// 4. Call Start() to begin streaming data
// 5. Receive records via OnRecord() callback
// 6. Call Stop() to close connection
class HistoricalClient {
public:
    enum class State {
        Disconnected,       // Initial state, not connected
        Connecting,         // TCP connection in progress
        TlsHandshaking,     // TLS handshake in progress
        SendingRequest,     // Sending HTTP request
        ReceivingResponse,  // Receiving HTTP response headers
        Streaming,          // Receiving and parsing DBN data
        Complete,           // All data received successfully
        Error               // An error occurred
    };

    // Sink class - RecordSink that bridges to HistoricalClient callbacks
    // Implements the RecordSink concept (OnData, OnError, OnComplete) for batch-based
    // record delivery from DbnParserComponent.
    class Sink {
    public:
        explicit Sink(HistoricalClient* client) : client_(client) {}

        // Invalidate sink (called when client is being destroyed)
        void Invalidate() { valid_ = false; }

        // RecordSink interface
        void OnData(RecordBatch&& batch);
        void OnError(const Error& e);
        void OnComplete();

    private:
        HistoricalClient* client_;
        bool valid_ = true;
    };

    // Pipeline type aliases
    using ParserType = DbnParserComponent<Sink>;
    using ZstdType = ZstdDecompressor<ParserType>;
    using HttpType = HttpClient<ZstdType>;
    using TlsType = TlsSocket<HttpType>;

    HistoricalClient(Reactor& reactor, std::string api_key);
    ~HistoricalClient();

    // Non-copyable, non-movable
    HistoricalClient(const HistoricalClient&) = delete;
    HistoricalClient& operator=(const HistoricalClient&) = delete;
    HistoricalClient(HistoricalClient&&) = delete;
    HistoricalClient& operator=(HistoricalClient&&) = delete;

    // Set request parameters (call before Connect)
    // start and end are Unix timestamps in nanoseconds
    void Request(std::string_view dataset,
                 std::string_view symbols,
                 std::string_view schema,
                 std::uint64_t start,
                 std::uint64_t end);

    // Connection (caller responsible for DNS resolution)
    void Connect(const sockaddr_storage& addr);

    // Control
    void Start();
    void Stop();

    // Backpressure control
    void Pause();
    void Resume();

    // State accessor
    State GetState() const { return state_; }

    // Callbacks
    template <typename Handler>
    void OnRecord(Handler&& h) {
        record_handler_ = std::forward<Handler>(h);
    }

    template <typename Handler>
    void OnError(Handler&& h) {
        error_handler_ = std::forward<Handler>(h);
    }

    template <typename Handler>
    void OnComplete(Handler&& h) {
        complete_handler_ = std::forward<Handler>(h);
    }

private:
    friend class Sink;

    // Pipeline setup
    void BuildPipeline();

    // Clean up pipeline components
    void TeardownPipeline();

    // TCP socket callbacks
    void HandleTcpConnect();
    void HandleTcpRead(std::span<const std::byte> data);
    void HandleTcpError(std::error_code ec);

    // Pipeline event handlers (called from Sink)
    void HandleRecord(const databento::Record& rec);
    void HandlePipelineError(const Error& e);
    void HandlePipelineComplete();

    // HTTP request generation
    void SendHttpRequest();
    std::string BuildHttpRequest() const;

    Reactor& reactor_;
    std::string api_key_;
    State state_ = State::Disconnected;

    // Request parameters
    std::string dataset_;
    std::string symbols_;
    std::string schema_;
    std::uint64_t start_ = 0;
    std::uint64_t end_ = 0;

    // Pipeline components (data flow: tcp -> tls -> http -> zstd -> parser -> sink)
    std::unique_ptr<TcpSocket> tcp_;
    std::shared_ptr<TlsType> tls_;
    std::shared_ptr<HttpType> http_;
    std::shared_ptr<ZstdType> zstd_;
    std::shared_ptr<ParserType> parser_;
    std::shared_ptr<Sink> sink_;

    // PMR pool for TLS write buffers
    std::pmr::unsynchronized_pool_resource pool_;

    // Callbacks
    std::function<void(const databento::Record&)> record_handler_;
    std::function<void(const Error&)> error_handler_;
    std::function<void()> complete_handler_;
};

}  // namespace databento_async
