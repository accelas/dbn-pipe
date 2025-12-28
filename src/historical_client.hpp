// src/historical_client.hpp
#pragma once

#include <netinet/in.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "data_source.hpp"
#include "reactor.hpp"

namespace databento_async {

// HistoricalClient fetches historical data from the Databento Historical API.
// Uses HTTPS (TLS) + HTTP + zstd decompression pipeline.
//
// Lifecycle:
// 1. Construct with reactor and API key
// 2. Call Request() to set dataset, symbols, schema, start/end timestamps
// 3. Call Connect() to initiate TLS connection
// 4. Call Start() to begin streaming data
// 5. Receive records via OnRecord() callback
// 6. Call Stop() to close connection
class HistoricalClient : public DataSource {
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

    HistoricalClient(Reactor& reactor, std::string api_key);
    ~HistoricalClient() override;

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

    // DataSource interface
    void Start() override;
    void Stop() override;

    // State accessor
    State GetState() const { return state_; }

private:
    // TODO: Task 7 - Pipeline wiring
    // void SetupPipeline();
    // void HandleTcpConnect();
    // void HandleTlsHandshake();
    // void HandleHttpResponse();
    // void HandleDecompressedData(std::span<const std::byte> data);
    // void HandleError(const Error& e);
    // void HandleComplete();
    // void SendHttpRequest();

    Reactor& reactor_;
    std::string api_key_;
    State state_ = State::Disconnected;

    // Request parameters
    std::string dataset_;
    std::string symbols_;
    std::string schema_;
    std::uint64_t start_ = 0;
    std::uint64_t end_ = 0;

    // TODO: Task 7 - Pipeline components
    // TcpSocket socket_;
    // std::shared_ptr<TlsSocket<...>> tls_;
    // std::shared_ptr<HttpClient<...>> http_;
    // std::shared_ptr<ZstdDecompressor<...>> decompressor_;
};

}  // namespace databento_async
