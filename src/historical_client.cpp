// src/historical_client.cpp
#include "historical_client.hpp"

namespace databento_async {

HistoricalClient::HistoricalClient(Reactor& reactor, std::string api_key)
    : reactor_(reactor),
      api_key_(std::move(api_key)) {
    // TODO: Task 7 - Initialize pipeline components
}

HistoricalClient::~HistoricalClient() {
    Stop();
}

void HistoricalClient::Request(std::string_view dataset,
                               std::string_view symbols,
                               std::string_view schema,
                               std::uint64_t start,
                               std::uint64_t end) {
    if (state_ != State::Disconnected) {
        DeliverError(Error{ErrorCode::InvalidState,
                           "Request() can only be called when disconnected"});
        return;
    }

    if (dataset.empty()) {
        DeliverError(Error{ErrorCode::InvalidDataset, "Dataset cannot be empty"});
        return;
    }
    if (symbols.empty()) {
        DeliverError(Error{ErrorCode::InvalidSymbol, "Symbols cannot be empty"});
        return;
    }
    if (schema.empty()) {
        DeliverError(Error{ErrorCode::InvalidSchema, "Schema cannot be empty"});
        return;
    }
    if (start >= end) {
        DeliverError(Error{ErrorCode::InvalidTimeRange,
                           "Start timestamp must be less than end timestamp"});
        return;
    }

    dataset_ = dataset;
    symbols_ = symbols;
    schema_ = schema;
    start_ = start;
    end_ = end;
}

void HistoricalClient::Connect(const sockaddr_storage& /*addr*/) {
    if (state_ != State::Disconnected) {
        DeliverError(Error{ErrorCode::InvalidState,
                           "Connect() can only be called when disconnected"});
        return;
    }

    // TODO: Task 7 - Implement connection logic
    // 1. Create TCP socket
    // 2. Set up TLS, HTTP, and zstd pipeline
    // 3. Initiate TCP connection
    // 4. On connect, start TLS handshake
    // 5. On TLS complete, send HTTP request
    // 6. On HTTP response, start streaming

    state_ = State::Connecting;
}

void HistoricalClient::Start() {
    // TODO: Task 7 - Implement start streaming
    // For historical API, data starts flowing after HTTP response is received
    // This method may be a no-op if streaming starts automatically after Connect
}

void HistoricalClient::Stop() {
    // TODO: Task 7 - Implement clean shutdown
    // 1. Close pipeline components
    // 2. Reset state

    state_ = State::Disconnected;
}

}  // namespace databento_async
