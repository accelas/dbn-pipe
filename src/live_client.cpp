// src/live_client.cpp
#include "live_client.hpp"

#include <sstream>

#include "cram_auth.hpp"

namespace databento_async {

LiveClient::LiveClient(Reactor* reactor, std::string api_key)
    : reactor_(reactor),
      socket_(reactor),
      api_key_(std::move(api_key)) {

    socket_.OnConnect([this](std::error_code ec) {
        HandleConnect(ec);
    });

    socket_.OnRead([this](std::span<const std::byte> data, std::error_code ec) {
        HandleRead(data, ec);
    });
}

LiveClient::~LiveClient() {
    Close();
}

void LiveClient::Connect(std::string_view host, int port) {
    if (state_ != State::Disconnected) {
        return;
    }

    state_ = State::Connecting;
    socket_.Connect(host, port);
}

void LiveClient::Close() {
    socket_.Close();
    state_ = State::Disconnected;
    line_buffer_.clear();
    session_id_.clear();
}

void LiveClient::Subscribe(std::string_view dataset,
                           std::string_view symbols,
                           std::string_view schema) {
    dataset_ = dataset;
    symbols_ = symbols;
    schema_ = schema;

    // If already in Ready state, send subscription immediately
    if (state_ == State::Ready) {
        SendSubscription();
    }
}

void LiveClient::Start() {
    if (state_ != State::Ready) {
        return;
    }
    SendStart();
    state_ = State::Streaming;
}

void LiveClient::Stop() {
    Close();
}

void LiveClient::HandleConnect(std::error_code ec) {
    if (ec) {
        DeliverError(Error{ErrorCode::ConnectionFailed,
                           "Connect failed: " + ec.message()});
        state_ = State::Disconnected;
        return;
    }

    state_ = State::WaitingGreeting;
}

void LiveClient::HandleRead(std::span<const std::byte> data, std::error_code ec) {
    if (ec) {
        DeliverError(Error{ErrorCode::ConnectionFailed,
                           "Read failed: " + ec.message()});
        return;
    }

    if (state_ == State::Streaming) {
        // Binary DBN data - pass directly to DataSource
        DeliverBytes(data);
        return;
    }

    // Text protocol - accumulate lines
    for (auto byte : data) {
        char c = static_cast<char>(byte);
        line_buffer_ += c;

        if (c == '\n') {
            ProcessLine(line_buffer_);
            line_buffer_.clear();
        }
    }
}

void LiveClient::ProcessLine(std::string_view line) {
    // Remove trailing newline for easier processing
    if (!line.empty() && line.back() == '\n') {
        line = line.substr(0, line.size() - 1);
    }

    switch (state_) {
        case State::WaitingGreeting:
            HandleGreeting(line);
            break;
        case State::WaitingChallenge:
            HandleChallenge(line);
            break;
        case State::Authenticating:
            HandleAuthResponse(line);
            break;
        default:
            // Ignore unexpected lines in other states
            break;
    }
}

void LiveClient::HandleGreeting(std::string_view line) {
    // Greeting can be "session_id\n" or "session_id|version\n"
    // For our purposes we just extract the session_id part
    auto pipe_pos = line.find('|');
    if (pipe_pos != std::string_view::npos) {
        session_id_ = line.substr(0, pipe_pos);
    } else {
        session_id_ = line;
    }

    state_ = State::WaitingChallenge;
}

void LiveClient::HandleChallenge(std::string_view line) {
    auto challenge = CramAuth::ParseChallenge(std::string(line) + "\n");
    if (!challenge) {
        DeliverError(Error{ErrorCode::InvalidChallenge,
                           "Invalid challenge: " + std::string(line)});
        return;
    }

    SendAuth(*challenge);
    state_ = State::Authenticating;
}

void LiveClient::HandleAuthResponse(std::string_view line) {
    // Parse "success=1|session_id=5|" or "success=0|error=message|"
    bool found_success = false;
    bool is_success = false;
    std::string error_msg;

    // Parse key=value pairs separated by |
    std::string_view remaining = line;
    while (!remaining.empty()) {
        auto pipe_pos = remaining.find('|');
        std::string_view kv;
        if (pipe_pos != std::string_view::npos) {
            kv = remaining.substr(0, pipe_pos);
            remaining = remaining.substr(pipe_pos + 1);
        } else {
            kv = remaining;
            remaining = {};
        }

        if (kv.empty()) continue;

        auto eq_pos = kv.find('=');
        if (eq_pos == std::string_view::npos) continue;

        auto key = kv.substr(0, eq_pos);
        auto value = kv.substr(eq_pos + 1);

        if (key == "success") {
            found_success = true;
            is_success = (value == "1");
        } else if (key == "error") {
            error_msg = value;
        } else if (key == "session_id") {
            // Update session_id from auth response if provided
            session_id_ = value;
        }
    }

    if (!found_success || !is_success) {
        std::string msg = "Authentication failed";
        if (!error_msg.empty()) {
            msg += ": " + error_msg;
        }
        DeliverError(Error{ErrorCode::AuthFailed, msg});
        state_ = State::Disconnected;
        return;
    }

    state_ = State::Ready;

    // If subscription params were set before auth completed, send now
    if (!dataset_.empty()) {
        SendSubscription();
    }
}

void LiveClient::SendAuth(std::string_view challenge) {
    // Compute CRAM response: SHA256(challenge + "|" + api_key)
    std::string response = CramAuth::ComputeResponse(challenge, api_key_);

    // Extract bucket_id (last 5 chars of API key)
    constexpr std::size_t kBucketIdLength = 5;
    std::string bucket_id;
    if (api_key_.size() >= kBucketIdLength) {
        bucket_id = api_key_.substr(api_key_.size() - kBucketIdLength);
    }

    // Build auth message matching databento protocol:
    // "auth=<hash>-<bucket>|dataset=<dataset>|encoding=dbn|ts_out=0\n"
    std::ostringstream auth_msg;
    auth_msg << "auth=" << response;
    if (!bucket_id.empty()) {
        auth_msg << "-" << bucket_id;
    }
    auth_msg << "|dataset=" << dataset_
             << "|encoding=dbn"
             << "|ts_out=0"
             << "\n";

    std::string msg = auth_msg.str();
    socket_.Write(std::as_bytes(std::span{msg.data(), msg.size()}));
}

void LiveClient::SendSubscription() {
    ++sub_counter_;

    // Build subscription message:
    // "schema=<schema>|stype_in=raw_symbol|id=<id>|symbols=<symbols>|snapshot=0|is_last=1\n"
    std::ostringstream sub_msg;
    sub_msg << "schema=" << schema_
            << "|stype_in=raw_symbol"
            << "|id=" << sub_counter_
            << "|symbols=" << symbols_
            << "|snapshot=0"
            << "|is_last=1"
            << "\n";

    std::string msg = sub_msg.str();
    socket_.Write(std::as_bytes(std::span{msg.data(), msg.size()}));
}

void LiveClient::SendStart() {
    constexpr std::string_view msg = "start_session\n";
    socket_.Write(std::as_bytes(std::span{msg.data(), msg.size()}));
}

}  // namespace databento_async
