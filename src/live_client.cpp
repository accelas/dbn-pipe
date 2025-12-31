// src/live_client.cpp
#include "live_client.hpp"

namespace databento_async {

// Sink implementation

void LiveClient::Sink::OnData(RecordBatch&& batch) {
    if (!valid_) return;
    if (!client_->record_handler_) return;

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

        client_->record_handler_(rec);
    }
}

void LiveClient::Sink::OnError(const Error& e) {
    if (!valid_) return;
    valid_ = false;  // Prevent further callbacks
    client_->state_ = State::Error;
    if (client_->error_handler_) {
        client_->error_handler_(e);
    }
    // No deferred teardown - let destructor or Stop() handle it
}

void LiveClient::Sink::OnComplete() {
    if (!valid_) return;
    valid_ = false;  // Prevent further callbacks
    client_->state_ = State::Complete;
    if (client_->complete_handler_) {
        client_->complete_handler_();
    }
    // No deferred teardown - let destructor or Stop() handle it
}

// LiveClient implementation

LiveClient::LiveClient(Reactor& reactor, std::string api_key)
    : reactor_(reactor),
      api_key_(std::move(api_key)) {
}

LiveClient::~LiveClient() {
    // Destructor teardown - does not assert reactor thread since
    // destruction may happen from any thread. Use TeardownPipeline directly.
    TeardownPipeline();
}

void LiveClient::BuildPipeline() {
    // Create sink (this must be created as shared_ptr for pipeline ownership)
    sink_ = std::make_shared<Sink>(this);

    // Create parser component with sink as downstream
    // The new DbnParserComponent takes a reference, not shared_ptr
    parser_ = std::make_shared<ParserType>(*sink_);

    // Create protocol handler with parser as downstream
    protocol_ = ProtocolType::Create(reactor_, parser_, api_key_);

    // Create TCP socket
    tcp_ = std::make_unique<TcpSocket>(reactor_);

    // Wire TCP socket callbacks to protocol handler
    tcp_->OnConnect([this]() {
        HandleConnect();
    });

    tcp_->OnRead([this](BufferChain chain) {
        HandleRead(std::move(chain));
    });

    tcp_->OnError([this](std::error_code ec) {
        HandleSocketError(ec);
    });

    // Wire protocol write callback to TCP socket
    protocol_->SetWriteCallback([this](BufferChain data) {
        if (tcp_) {
            tcp_->Write(std::move(data));
        }
    });

    // If subscription was requested before pipeline was built, set it now
    if (!dataset_.empty()) {
        protocol_->Subscribe(dataset_, symbols_, schema_);
    }
}

void LiveClient::TeardownPipeline() {
    // Invalidate sink to prevent callbacks during teardown
    if (sink_) {
        sink_->Invalidate();
    }

    // Close TCP socket first
    if (tcp_) {
        tcp_->Close();
        tcp_.reset();
    }

    // Close pipeline components
    if (protocol_) {
        protocol_->Close();
        protocol_.reset();
    }

    // Parser component doesn't have Close() - just reset it
    parser_.reset();

    sink_.reset();
    session_id_.clear();
}

void LiveClient::Connect(const sockaddr_storage& addr) {
    if (state_ != State::Disconnected) {
        // Report error for invalid state transition
        if (error_handler_) {
            error_handler_(Error{ErrorCode::InvalidState,
                                 "Cannot connect: client is not disconnected"});
        }
        return;
    }

    // Build pipeline before connecting
    BuildPipeline();

    state_ = State::Connecting;
    tcp_->Connect(addr);
}

void LiveClient::Close() {
    assert(reactor_.IsInReactorThread() && "Close must be called from reactor thread");
    TeardownPipeline();
    state_ = State::Disconnected;
    suspended_.store(false, std::memory_order_release);
}

void LiveClient::Subscribe(std::string_view dataset,
                           std::string_view symbols,
                           std::string_view schema) {
    dataset_ = dataset;
    symbols_ = symbols;
    schema_ = schema;

    // Forward to protocol handler if it exists
    if (protocol_) {
        protocol_->Subscribe(std::string(dataset), std::string(symbols),
                             std::string(schema));
    }
}

void LiveClient::Start() {
    if (state_ != State::Ready) {
        return;
    }

    if (protocol_) {
        protocol_->StartStreaming();

        // Only update state if protocol actually transitioned
        if (protocol_->GetState() == CramAuthState::Streaming) {
            state_ = State::Streaming;
        }
    }
}

void LiveClient::Stop() {
    // Stop can be called from any thread - it just does teardown
    // For reactor-thread-safe close with proper state transition, use Close()
    TeardownPipeline();
    state_ = State::Disconnected;
    suspended_.store(false, std::memory_order_release);
}

void LiveClient::Suspend() {
    assert(reactor_.IsInReactorThread() && "Suspend must be called from reactor thread");
    // Idempotent: only pause if not already suspended
    // atomic exchange returns the previous value
    if (!suspended_.exchange(true, std::memory_order_acq_rel)) {
        if (tcp_) {
            tcp_->Suspend();
        }
    }
}

void LiveClient::Resume() {
    assert(reactor_.IsInReactorThread() && "Resume must be called from reactor thread");
    // Idempotent: only resume if currently suspended
    // atomic exchange returns the previous value
    if (suspended_.exchange(false, std::memory_order_acq_rel)) {
        if (tcp_) {
            tcp_->Resume();
        }
    }
}

void LiveClient::HandleConnect() {
    state_ = State::WaitingGreeting;
    // Protocol handler will receive data and handle authentication
}

void LiveClient::HandleSocketError(std::error_code ec) {
    state_ = State::Error;
    if (error_handler_) {
        error_handler_(Error{ErrorCode::ConnectionFailed, ec.message()});
    }
    // No teardown here - let destructor or Stop() handle it
    // Synchronous teardown is NOT safe since we're inside TcpSocket callback
}

void LiveClient::HandleRead(BufferChain data) {
    if (!protocol_) return;

    protocol_->OnData(data);

    // Update state based on protocol state
    UpdateStateFromProtocol();
}

void LiveClient::UpdateStateFromProtocol() {
    if (!protocol_) return;

    // Don't overwrite terminal states (Disconnected, Complete, Error)
    if (state_ == State::Disconnected ||
        state_ == State::Complete ||
        state_ == State::Error) {
        return;
    }

    switch (protocol_->GetState()) {
        case CramAuthState::WaitingGreeting:
            state_ = State::WaitingGreeting;
            break;
        case CramAuthState::WaitingChallenge:
            state_ = State::WaitingChallenge;
            session_id_ = protocol_->GetGreeting().session_id;
            break;
        case CramAuthState::Authenticating:
            state_ = State::Authenticating;
            break;
        case CramAuthState::Ready:
            state_ = State::Ready;
            break;
        case CramAuthState::Streaming:
            state_ = State::Streaming;
            break;
    }
}

}  // namespace databento_async
