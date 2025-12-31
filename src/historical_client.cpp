// src/historical_client.cpp
#include "historical_client.hpp"

#include <iostream>
#include <span>
#include <spanstream>

namespace databento_async {

// Debug logging macro (disabled in production)
#define HIST_DEBUG(msg) do {} while (0)

namespace {

// Base64 encode for HTTP Basic authentication - writes directly to ostream
void Base64Encode(std::ostream& out, std::string_view input) {
    static const char* kBase64Chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    size_t i = 0;
    while (i + 2 < input.size()) {
        uint32_t triple = (static_cast<uint8_t>(input[i]) << 16) |
                          (static_cast<uint8_t>(input[i + 1]) << 8) |
                          static_cast<uint8_t>(input[i + 2]);
        out << kBase64Chars[(triple >> 18) & 0x3F];
        out << kBase64Chars[(triple >> 12) & 0x3F];
        out << kBase64Chars[(triple >> 6) & 0x3F];
        out << kBase64Chars[triple & 0x3F];
        i += 3;
    }

    if (i + 1 == input.size()) {
        uint32_t val = static_cast<uint8_t>(input[i]) << 16;
        out << kBase64Chars[(val >> 18) & 0x3F];
        out << kBase64Chars[(val >> 12) & 0x3F];
        out << '=';
        out << '=';
    } else if (i + 2 == input.size()) {
        uint32_t val = (static_cast<uint8_t>(input[i]) << 16) |
                       (static_cast<uint8_t>(input[i + 1]) << 8);
        out << kBase64Chars[(val >> 18) & 0x3F];
        out << kBase64Chars[(val >> 12) & 0x3F];
        out << kBase64Chars[(val >> 6) & 0x3F];
        out << '=';
    }
}

// URL-encode a string for use in query parameters - writes directly to ostream
void UrlEncode(std::ostream& out, std::string_view str) {
    for (unsigned char c : str) {
        // Unreserved characters (RFC 3986): A-Z a-z 0-9 - _ . ~
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out << static_cast<char>(c);
        } else {
            // Percent-encode all other characters
            static const char hex[] = "0123456789ABCDEF";
            out << '%';
            out << hex[(c >> 4) & 0x0F];
            out << hex[c & 0x0F];
        }
    }
}

}  // namespace

// Sink implementation

void HistoricalClient::Sink::OnData(RecordBatch&& batch) {
    if (!valid_) return;

    // Iterate through each record in the batch and call the handler
    for (const auto& ref : batch) {
        // Create a databento::Record view from the RecordRef data.
        //
        // const_cast justification:
        // - databento::Record's constructor requires a non-const RecordHeader*
        //   because it allows mutation for some use cases.
        // - Our RecordRef stores const data since we treat received records as
        //   read-only (zero-copy from network buffers).
        // - The cast is safe: we don't mutate the data, and databento::Record
        //   is a non-owning view that doesn't take ownership.
        // - The RecordRef's keepalive ensures the buffer remains valid for this scope.
        auto* data = const_cast<std::byte*>(ref.data);
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

    // 5. Create TlsTransport with http as downstream
    tls_ = TlsType::Create(reactor_, http_);
    http_->SetUpstream(tls_.get());
    tls_->SetHostname("hist.databento.com");

    // 6. Create TcpSocket
    tcp_ = std::make_unique<TcpSocket>(reactor_);

    // 7. Wire TcpSocket callbacks
    tcp_->OnConnect([this]() {
        HandleTcpConnect();
    });

    tcp_->OnRead([this](BufferChain chain) {
        HandleTcpRead(std::move(chain));
    });

    tcp_->OnError([this](std::error_code ec) {
        HandleTcpError(ec);
    });

    // 8. Wire TlsTransport to write encrypted data back through TcpSocket
    tls_->SetUpstreamWriteCallback([this](BufferChain encrypted) {
        if (tcp_ && tcp_->IsConnected()) {
            tcp_->Write(std::move(encrypted));
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

void HistoricalClient::HandleTcpRead(BufferChain data) {
    HIST_DEBUG("TCP read: " << data.Size() << " bytes, state=" << static_cast<int>(state_));

    // Feed encrypted data to TLS layer (zero-copy)
    tls_->Read(std::move(data));

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
    // Format HTTP request directly into Segment buffer (zero-copy)
    auto seg = std::make_shared<Segment>();
    std::ospanstream out(std::span<char>(
        reinterpret_cast<char*>(seg->data.data()), Segment::kSize));

    // Build the path with query parameters (URL-encoded)
    out << "GET /v0/timeseries.get_range?dataset=";
    UrlEncode(out, dataset_);
    out << "&symbols=";
    UrlEncode(out, symbols_);
    out << "&schema=";
    UrlEncode(out, schema_);
    out << "&start=" << start_;
    out << "&end=" << end_;
    out << "&encoding=dbn";       // DBN binary format
    out << "&compression=zstd";   // zstd compression
    out << " HTTP/1.1\r\n";

    // Headers
    out << "Host: hist.databento.com\r\n";
    // HTTP Basic auth: base64(api_key:)
    out << "Authorization: Basic ";
    Base64Encode(out, api_key_ + ":");
    out << "\r\n";
    out << "Accept: application/octet-stream\r\n";
    out << "Accept-Encoding: identity\r\n";  // We handle decompression ourselves
    out << "Connection: close\r\n";
    out << "\r\n";

    seg->size = out.span().size();
    HIST_DEBUG("Sending HTTP request: " << seg->size << " bytes");

    BufferChain chain;
    chain.Append(std::move(seg));

    state_ = State::ReceivingResponse;
    tls_->Write(std::move(chain));
}

}  // namespace databento_async
