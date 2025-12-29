// src/live_client.cpp
#include "live_client.hpp"

namespace databento_async {

// Sink implementation

void LiveClient::Sink::OnRecord(const databento::Record& rec) {
    if (!valid_) return;
    if (client_->record_handler_) {
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

void LiveClient::Sink::OnDone() {
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
    Close();
}

void LiveClient::BuildPipeline() {
    // Create sink (this must be created as shared_ptr for pipeline ownership)
    sink_ = std::make_shared<Sink>(this);

    // Create parser component with sink as downstream
    parser_ = ParserType::Create(reactor_, sink_);

    // Create protocol handler with parser as downstream
    protocol_ = ProtocolType::Create(reactor_, parser_, api_key_);

    // Set upstream for backpressure
    parser_->SetUpstream(protocol_.get());

    // Create TCP socket
    tcp_ = std::make_unique<TcpSocket>(reactor_);

    // Wire TCP socket callbacks to protocol handler
    tcp_->OnConnect([this]() {
        HandleConnect();
    });

    tcp_->OnRead([this](std::span<const std::byte> data) {
        HandleRead(data);
    });

    tcp_->OnError([this](std::error_code ec) {
        HandleSocketError(ec);
    });

    // Wire protocol write callback to TCP socket
    protocol_->SetWriteCallback([this](std::pmr::vector<std::byte> data) {
        if (tcp_) {
            tcp_->Write(std::span<const std::byte>(data.data(), data.size()));
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

    if (parser_) {
        parser_->Close();
        parser_.reset();
    }

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
    TeardownPipeline();
    state_ = State::Disconnected;
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
        if (protocol_->GetState() == LiveProtocolState::Streaming) {
            state_ = State::Streaming;
        }
    }
}

void LiveClient::Stop() {
    Close();
}

void LiveClient::Pause() {
    if (state_ != State::Streaming) return;
    if (parser_) {
        parser_->Suspend();
    }
}

void LiveClient::Resume() {
    if (state_ != State::Streaming) return;
    if (parser_) {
        parser_->Resume();
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

void LiveClient::HandleRead(std::span<const std::byte> data) {
    if (!protocol_) return;

    // Create pmr::vector from span
    std::pmr::vector<std::byte> pmr_data(data.begin(), data.end());
    protocol_->Read(std::move(pmr_data));

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
        case LiveProtocolState::WaitingGreeting:
            state_ = State::WaitingGreeting;
            break;
        case LiveProtocolState::WaitingChallenge:
            state_ = State::WaitingChallenge;
            session_id_ = protocol_->GetGreeting().session_id;
            break;
        case LiveProtocolState::Authenticating:
            state_ = State::Authenticating;
            break;
        case LiveProtocolState::Ready:
            state_ = State::Ready;
            break;
        case LiveProtocolState::Streaming:
            state_ = State::Streaming;
            break;
    }
}

}  // namespace databento_async
