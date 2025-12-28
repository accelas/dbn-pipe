// src/historical_client.cpp
#include "historical_client.hpp"

#include <cstring>
#include <sstream>

namespace databento_async {

namespace {

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

// ApplicationSink implementation
void ApplicationSink::Read(std::pmr::vector<std::byte> data) {
    if (!valid_ || !client_) return;
    client_->HandleDecompressedData(std::move(data));
}

void ApplicationSink::OnError(const Error& e) {
    if (!valid_ || !client_) return;
    client_->HandlePipelineError(e);
}

void ApplicationSink::OnDone() {
    if (!valid_ || !client_) return;
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

void HistoricalClient::Connect(const sockaddr_storage& addr) {
    if (state_ != State::Disconnected) {
        DeliverError(Error{ErrorCode::InvalidState,
                           "Connect() can only be called when disconnected"});
        return;
    }

    // Validate that Request() was called
    if (dataset_.empty()) {
        DeliverError(Error{ErrorCode::InvalidState,
                           "Request() must be called before Connect()"});
        return;
    }

    // Build the pipeline before connecting
    BuildPipeline();

    state_ = State::Connecting;

    // Initiate TCP connection
    tcp_->Connect(addr);
}

void HistoricalClient::Start() {
    // For historical API, data starts flowing after HTTP response is received
    // This method resumes the parser if it was paused
    if (IsPaused()) {
        Resume();
    }
}

void HistoricalClient::Stop() {
    // Mark as stopped to prevent callbacks from firing
    stopped_ = true;

    // Invalidate sink before cleanup to prevent dangling pointer access
    if (sink_) {
        sink_->Invalidate();
    }

    // Close pipeline components in order
    if (tcp_) {
        tcp_->Close();
    }

    // Reset pipeline components
    tcp_.reset();
    tls_.reset();
    http_.reset();
    decompressor_.reset();
    sink_.reset();

    state_ = State::Disconnected;
}

void HistoricalClient::BuildPipeline() {
    // Build pipeline from bottom up (data flows: tcp -> tls -> http -> zstd -> sink)

    // 1. Create the application sink (final downstream)
    sink_ = std::make_shared<ApplicationSink>(this);

    // 2. Create ZstdDecompressor with sink as downstream
    decompressor_ = ZstdDecompressor<ApplicationSink>::Create(reactor_, sink_);

    // 3. Create HttpClient with decompressor as downstream
    http_ = HttpClient<ZstdDecompressor<ApplicationSink>>::Create(reactor_, decompressor_);
    decompressor_->SetUpstream(http_.get());

    // 4. Create TlsSocket with http as downstream
    tls_ = TlsSocket<HttpClient<ZstdDecompressor<ApplicationSink>>>::Create(reactor_, http_);
    http_->SetUpstream(tls_.get());
    tls_->SetHostname("hist.databento.com");

    // 5. Create TcpSocket
    tcp_ = std::make_unique<TcpSocket>(reactor_);

    // 6. Wire TcpSocket callbacks
    tcp_->OnConnect([this]() {
        HandleTcpConnect();
    });

    tcp_->OnRead([this](std::span<const std::byte> data) {
        HandleTcpRead(data);
    });

    tcp_->OnError([this](std::error_code ec) {
        HandleTcpError(ec);
    });

    // 7. Wire TlsSocket to write encrypted data back through TcpSocket
    tls_->SetUpstreamWriteCallback([this](std::pmr::vector<std::byte> encrypted) {
        if (tcp_ && tcp_->IsConnected()) {
            tcp_->Write(std::span<const std::byte>(encrypted.data(), encrypted.size()));
        }
    });
}

void HistoricalClient::HandleTcpConnect() {
    if (stopped_) return;
    state_ = State::TlsHandshaking;
    tls_->StartHandshake();
}

void HistoricalClient::HandleTcpRead(std::span<const std::byte> data) {
    if (stopped_) return;

    // Convert span to pmr::vector for TlsSocket
    std::pmr::vector<std::byte> buffer(&pool_);
    buffer.assign(data.begin(), data.end());

    // Feed encrypted data to TLS layer
    tls_->Read(std::move(buffer));

    // After TLS processes data, check if handshake just completed
    if (state_ == State::TlsHandshaking && tls_->IsHandshakeComplete()) {
        state_ = State::SendingRequest;
        SendHttpRequest();
    }
}

void HistoricalClient::HandleTcpError(std::error_code ec) {
    if (stopped_) return;
    state_ = State::Error;
    DeliverError(Error{ErrorCode::ConnectionFailed, ec.message(), ec.value()});
}

void HistoricalClient::HandleDecompressedData(std::pmr::vector<std::byte> data) {
    if (stopped_) return;

    // Update state if this is the first data received
    if (state_ == State::SendingRequest || state_ == State::ReceivingResponse) {
        state_ = State::Streaming;
    }

    // Forward decompressed DBN data to the parser
    DeliverBytes(std::span<const std::byte>(data.data(), data.size()));
}

void HistoricalClient::HandlePipelineError(const Error& e) {
    if (stopped_) return;
    state_ = State::Error;
    DeliverError(e);
}

void HistoricalClient::HandlePipelineComplete() {
    if (stopped_) return;
    state_ = State::Complete;
    // The DataSource doesn't have an OnComplete callback, but we could add one
    // For now, we just update the state
}

void HistoricalClient::SendHttpRequest() {
    std::string request = BuildHttpRequest();

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
    request << "Authorization: Basic " << api_key_ << "\r\n";
    request << "Accept: application/octet-stream\r\n";
    request << "Accept-Encoding: identity\r\n";  // We handle decompression ourselves
    request << "Connection: close\r\n";
    request << "\r\n";

    return request.str();
}

}  // namespace databento_async
