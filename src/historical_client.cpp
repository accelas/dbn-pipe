// src/historical_client.cpp
#include "historical_client.hpp"

#include <cstring>
#include <iostream>
#include <sstream>

namespace databento_async {

// Debug logging macro (disabled in production)
#define HIST_DEBUG(msg) do {} while (0)

namespace {

// Base64 encode for HTTP Basic authentication
std::string Base64Encode(std::string_view input) {
    static const char* kBase64Chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string result;
    result.reserve(((input.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i + 2 < input.size()) {
        uint32_t triple = (static_cast<uint8_t>(input[i]) << 16) |
                          (static_cast<uint8_t>(input[i + 1]) << 8) |
                          static_cast<uint8_t>(input[i + 2]);
        result += kBase64Chars[(triple >> 18) & 0x3F];
        result += kBase64Chars[(triple >> 12) & 0x3F];
        result += kBase64Chars[(triple >> 6) & 0x3F];
        result += kBase64Chars[triple & 0x3F];
        i += 3;
    }

    if (i + 1 == input.size()) {
        uint32_t val = static_cast<uint8_t>(input[i]) << 16;
        result += kBase64Chars[(val >> 18) & 0x3F];
        result += kBase64Chars[(val >> 12) & 0x3F];
        result += '=';
        result += '=';
    } else if (i + 2 == input.size()) {
        uint32_t val = (static_cast<uint8_t>(input[i]) << 16) |
                       (static_cast<uint8_t>(input[i + 1]) << 8);
        result += kBase64Chars[(val >> 18) & 0x3F];
        result += kBase64Chars[(val >> 12) & 0x3F];
        result += kBase64Chars[(val >> 6) & 0x3F];
        result += '=';
    }

    return result;
}

// URL-encode a string for use in query parameters
std::string UrlEncode(std::string_view str) {
    std::string result;
    result.reserve(str.size());

    for (unsigned char c : str) {
        // Unreserved characters (RFC 3986): A-Z a-z 0-9 - _ . ~
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            result += static_cast<char>(c);
        } else {
            // Percent-encode all other characters
            static const char hex[] = "0123456789ABCDEF";
            result += '%';
            result += hex[(c >> 4) & 0x0F];
            result += hex[c & 0x0F];
        }
    }

    return result;
}

}  // namespace

// Sink implementation

void HistoricalClient::Sink::OnData(RecordBatch&& batch) {
    if (!valid_) return;

    // Iterate through each record in the batch and call the handler
    for (size_t i = 0; i < batch.size(); ++i) {
        // Create a Record view from the raw data
        // Note: The Record constructor expects a non-const pointer to the header.
        // The RecordBatch owns the buffer, so we need to cast away const for the
        // databento::Record API. The record is only valid for this scope.
        auto* data = const_cast<std::byte*>(batch.GetRecordData(i));
        databento::Record rec{reinterpret_cast<databento::RecordHeader*>(data)};

        client_->HandleRecord(rec);
    }
}

void HistoricalClient::Sink::OnError(const Error& e) {
    if (!valid_) return;
    valid_ = false;  // Prevent further callbacks
    client_->HandlePipelineError(e);
}

void HistoricalClient::Sink::OnComplete() {
    if (!valid_) return;
    valid_ = false;  // Prevent further callbacks
    client_->HandlePipelineComplete();
}

// HistoricalClient implementation

HistoricalClient::HistoricalClient(Reactor& reactor, std::string api_key)
    : reactor_(reactor),
      api_key_(std::move(api_key)) {
    // Pipeline components are created in BuildPipeline() when Connect() is called
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
        if (error_handler_) {
            error_handler_(Error{ErrorCode::InvalidState,
                               "Request() can only be called when disconnected"});
        }
        return;
    }

    if (dataset.empty()) {
        if (error_handler_) {
            error_handler_(Error{ErrorCode::InvalidDataset, "Dataset cannot be empty"});
        }
        return;
    }
    if (symbols.empty()) {
        if (error_handler_) {
            error_handler_(Error{ErrorCode::InvalidSymbol, "Symbols cannot be empty"});
        }
        return;
    }
    if (schema.empty()) {
        if (error_handler_) {
            error_handler_(Error{ErrorCode::InvalidSchema, "Schema cannot be empty"});
        }
        return;
    }
    if (start >= end) {
        if (error_handler_) {
            error_handler_(Error{ErrorCode::InvalidTimeRange,
                               "Start timestamp must be less than end timestamp"});
        }
        return;
    }

    dataset_ = dataset;
    symbols_ = symbols;
    schema_ = schema;
    start_ = start;
    end_ = end;
}

void HistoricalClient::Connect(const sockaddr_storage& addr) {
    HIST_DEBUG("Connect() called, state=" << static_cast<int>(state_));

    if (state_ != State::Disconnected) {
        if (error_handler_) {
            error_handler_(Error{ErrorCode::InvalidState,
                               "Connect() can only be called when disconnected"});
        }
        return;
    }

    // Validate that Request() was called
    if (dataset_.empty()) {
        if (error_handler_) {
            error_handler_(Error{ErrorCode::InvalidState,
                               "Request() must be called before Connect()"});
        }
        return;
    }

    // Build the pipeline before connecting
    BuildPipeline();

    state_ = State::Connecting;
    HIST_DEBUG("Initiating TCP connection...");

    // Initiate TCP connection
    tcp_->Connect(addr);
}

void HistoricalClient::Start() {
    // For historical API, data starts flowing after HTTP response is received
    // This is a no-op in the current implementation - data flows automatically
    // after the HTTP response headers are received.
}

void HistoricalClient::Stop() {
    TeardownPipeline();
    state_ = State::Disconnected;
    suspended_.store(false, std::memory_order_release);
}

void HistoricalClient::Suspend() {
    assert(reactor_.IsInReactorThread() && "Suspend must be called from reactor thread");
    // Idempotent: only pause if not already suspended
    // atomic exchange returns the previous value
    if (!suspended_.exchange(true, std::memory_order_acq_rel)) {
        if (tcp_) {
            tcp_->PauseRead();
        }
    }
}

void HistoricalClient::Resume() {
    assert(reactor_.IsInReactorThread() && "Resume must be called from reactor thread");
    // Idempotent: only resume if currently suspended
    // atomic exchange returns the previous value
    if (suspended_.exchange(false, std::memory_order_acq_rel)) {
        if (tcp_) {
            tcp_->ResumeRead();
        }
    }
}

void HistoricalClient::Close() {
    assert(reactor_.IsInReactorThread() && "Close must be called from reactor thread");
    TeardownPipeline();
    state_ = State::Disconnected;
    suspended_.store(false, std::memory_order_release);
}

void HistoricalClient::BuildPipeline() {
    // Build pipeline from bottom up (data flows: tcp -> tls -> http -> zstd -> parser -> sink)

    // 1. Create the application sink (final downstream)
    sink_ = std::make_shared<Sink>(this);

    // 2. Create DbnParserComponent with sink as downstream
    // The new DbnParserComponent takes a reference, not shared_ptr
    parser_ = std::make_shared<ParserType>(*sink_);

    // 3. Create ZstdDecompressor with parser as downstream
    zstd_ = ZstdType::Create(reactor_, parser_);

    // 4. Create HttpClient with zstd as downstream
    http_ = HttpType::Create(reactor_, zstd_);
    zstd_->SetUpstream(http_.get());

    // 5. Create TlsSocket with http as downstream
    tls_ = TlsType::Create(reactor_, http_);
    http_->SetUpstream(tls_.get());
    tls_->SetHostname("hist.databento.com");

    // 6. Create TcpSocket
    tcp_ = std::make_unique<TcpSocket>(reactor_);

    // 7. Wire TcpSocket callbacks
    tcp_->OnConnect([this]() {
        HandleTcpConnect();
    });

    tcp_->OnRead([this](std::span<const std::byte> data) {
        HandleTcpRead(data);
    });

    tcp_->OnError([this](std::error_code ec) {
        HandleTcpError(ec);
    });

    // 8. Wire TlsSocket to write encrypted data back through TcpSocket
    tls_->SetUpstreamWriteCallback([this](std::pmr::vector<std::byte> encrypted) {
        if (tcp_ && tcp_->IsConnected()) {
            tcp_->Write(std::span<const std::byte>(encrypted.data(), encrypted.size()));
        }
    });
}

void HistoricalClient::TeardownPipeline() {
    // Invalidate sink to prevent callbacks during teardown
    if (sink_) {
        sink_->Invalidate();
    }

    // Close components before reset to handle callback reentrancy
    // Note: parser_ doesn't have Close() - just reset it
    if (zstd_) zstd_->Close();
    if (http_) http_->Close();
    if (tls_) tls_->Close();
    if (tcp_) tcp_->Close();

    // Now safe to reset
    tcp_.reset();
    tls_.reset();
    http_.reset();
    zstd_.reset();
    parser_.reset();
    sink_.reset();
}

void HistoricalClient::HandleTcpConnect() {
    HIST_DEBUG("TCP connected! Starting TLS handshake...");
    state_ = State::TlsHandshaking;
    tls_->StartHandshake();
}

void HistoricalClient::HandleTcpRead(std::span<const std::byte> data) {
    HIST_DEBUG("TCP read: " << data.size() << " bytes, state=" << static_cast<int>(state_));

    // Convert span to pmr::vector for TlsSocket
    std::pmr::vector<std::byte> buffer(&pool_);
    buffer.assign(data.begin(), data.end());

    // Feed encrypted data to TLS layer
    tls_->Read(std::move(buffer));

    // After TLS processes data, check if handshake just completed
    if (state_ == State::TlsHandshaking && tls_->IsHandshakeComplete()) {
        HIST_DEBUG("TLS handshake complete! Sending HTTP request...");
        state_ = State::SendingRequest;
        SendHttpRequest();
    }
}

void HistoricalClient::HandleTcpError(std::error_code ec) {
    HIST_DEBUG("TCP error: " << ec.message() << " (" << ec.value() << ")");
    state_ = State::Error;
    if (error_handler_) {
        error_handler_(Error{ErrorCode::ConnectionFailed, ec.message(), ec.value()});
    }
    // No teardown here - let destructor or Stop() handle it
    // Synchronous teardown is NOT safe since we're inside TcpSocket callback
}

void HistoricalClient::HandleRecord(const databento::Record& rec) {
    HIST_DEBUG("Received record! rtype=" << static_cast<int>(rec.Header().rtype));
    // Update state if this is the first record received
    if (state_ == State::SendingRequest || state_ == State::ReceivingResponse) {
        state_ = State::Streaming;
    }

    // Forward parsed record to the application callback
    if (record_handler_) {
        record_handler_(rec);
    }
}

void HistoricalClient::HandlePipelineError(const Error& e) {
    HIST_DEBUG("Pipeline error: " << e.message);
    state_ = State::Error;
    if (error_handler_) {
        error_handler_(e);
    }
}

void HistoricalClient::HandlePipelineComplete() {
    HIST_DEBUG("Pipeline complete!");
    state_ = State::Complete;
    if (complete_handler_) {
        complete_handler_();
    }
}

void HistoricalClient::SendHttpRequest() {
    std::string request = BuildHttpRequest();
    HIST_DEBUG("Sending HTTP request:\n" << request.substr(0, 200) << "...");

    // Convert to pmr::vector<byte> for TlsSocket::Write
    std::pmr::vector<std::byte> buffer(&pool_);
    buffer.resize(request.size());
    std::memcpy(buffer.data(), request.data(), request.size());

    state_ = State::ReceivingResponse;
    tls_->Write(std::move(buffer));
}

std::string HistoricalClient::BuildHttpRequest() const {
    std::ostringstream request;

    // Build the path with query parameters (URL-encoded)
    request << "GET /v0/timeseries.get_range?";
    request << "dataset=" << UrlEncode(dataset_);
    request << "&symbols=" << UrlEncode(symbols_);
    request << "&schema=" << UrlEncode(schema_);
    request << "&start=" << start_;
    request << "&end=" << end_;
    request << "&encoding=dbn";       // DBN binary format
    request << "&compression=zstd";   // zstd compression
    request << " HTTP/1.1\r\n";

    // Headers
    request << "Host: hist.databento.com\r\n";
    // HTTP Basic auth: base64(api_key:)
    request << "Authorization: Basic " << Base64Encode(api_key_ + ":") << "\r\n";
    request << "Accept: application/octet-stream\r\n";
    request << "Accept-Encoding: identity\r\n";  // We handle decompression ourselves
    request << "Connection: close\r\n";
    request << "\r\n";

    return request.str();
}

}  // namespace databento_async
