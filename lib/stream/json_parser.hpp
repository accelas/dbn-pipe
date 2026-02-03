// SPDX-License-Identifier: MIT

// lib/stream/json_parser.hpp
#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rapidjson/error/en.h>
#include <rapidjson/reader.h>

#include "dbn_pipe/stream/buffer_chain.hpp"
#include "dbn_pipe/stream/error.hpp"

namespace dbn_pipe {

// Builder concept - types that can incrementally build a result from JSON events
template <typename B>
concept JsonBuilder = requires(B& b, std::string_view sv, int64_t i, uint64_t u,
                               double d, bool bl) {
    typename B::Result;
    { b.OnKey(sv) } -> std::same_as<void>;
    { b.OnString(sv) } -> std::same_as<void>;
    { b.OnInt(i) } -> std::same_as<void>;
    { b.OnUint(u) } -> std::same_as<void>;
    { b.OnDouble(d) } -> std::same_as<void>;
    { b.OnBool(bl) } -> std::same_as<void>;
    { b.OnNull() } -> std::same_as<void>;
    { b.OnStartObject() } -> std::same_as<void>;
    { b.OnEndObject() } -> std::same_as<void>;
    { b.OnStartArray() } -> std::same_as<void>;
    { b.OnEndArray() } -> std::same_as<void>;
    { b.Build() } -> std::same_as<std::expected<typename B::Result, std::string>>;
};

// SAX-style JSON parser that feeds events to a Builder
//
// Supports:
// - Streaming parse via OnData() for chunked input
// - Best-effort result on connection errors (tries Build() before failing)
// - Callback-based completion with std::expected<Result, Error>
//
// Thread safety: Not thread-safe. All methods must be called from the same thread.
template <JsonBuilder Builder>
class JsonParser : public std::enable_shared_from_this<JsonParser<Builder>> {
public:
    using Result = typename Builder::Result;
    using Callback = std::function<void(std::expected<Result, Error>)>;

    // Factory method
    static std::shared_ptr<JsonParser> Create(Builder& builder, Callback on_complete) {
        return std::shared_ptr<JsonParser>(new JsonParser(builder, std::move(on_complete)));
    }

    // Feed data to the parser (supports chunked input)
    void OnData(BufferChain& chain) {
        if (completed_) return;

        // Accumulate data into internal buffer for parsing
        // JSON parsing requires contiguous data for simplicity with rapidjson
        while (!chain.Empty()) {
            size_t chunk_size = chain.ContiguousSize();

            // Check buffer limit before appending
            if (buffer_.size() + chunk_size > kMaxBufferSize) {
                Complete(std::unexpected(Error{
                    ErrorCode::BufferOverflow,
                    "JSON response exceeds maximum buffer size (" +
                        std::to_string(kMaxBufferSize / (1024 * 1024)) + "MB)"}));
                return;
            }

            const auto* data = chain.DataAt(0);
            buffer_.insert(buffer_.end(),
                           reinterpret_cast<const char*>(data),
                           reinterpret_cast<const char*>(data) + chunk_size);
            chain.Consume(chunk_size);
        }
    }

    // Handle upstream error
    void OnError(const Error& error) {
        if (completed_) return;

        // For HTTP errors (server responded with error status), propagate immediately.
        // These are definitive failures - the server rejected the request.
        // Only attempt best-effort parsing for connection-level errors where
        // we might have received partial valid data before the connection dropped.
        if (IsHttpError(error.code)) {
            Complete(std::unexpected(error));
            return;
        }

        // Connection-level error - try to build with whatever data we have
        auto result = TryParse();
        if (result) {
            Complete(std::move(*result));
        } else {
            // Build failed, propagate original error
            Complete(std::unexpected(error));
        }
    }

    // Check if error code represents an HTTP-level error (server responded)
    static bool IsHttpError(ErrorCode code) {
        switch (code) {
            case ErrorCode::Unauthorized:
            case ErrorCode::NotFound:
            case ErrorCode::ValidationError:
            case ErrorCode::RateLimited:
            case ErrorCode::ServerError:
            case ErrorCode::HttpError:
                return true;
            default:
                return false;
        }
    }

    // Signal end of data stream
    void OnDone() {
        if (completed_) return;

        auto result = TryParse();
        if (result) {
            Complete(std::move(*result));
        } else {
            Complete(std::unexpected(Error{
                ErrorCode::ParseError,
                "JSON parse failed: " + result.error()}));
        }
    }

private:
    JsonParser(Builder& builder, Callback on_complete)
        : builder_(builder), on_complete_(std::move(on_complete)) {}

    // RapidJSON SAX handler that forwards to Builder
    struct SaxHandler : public rapidjson::BaseReaderHandler<rapidjson::UTF8<>, SaxHandler> {
        Builder& builder;

        explicit SaxHandler(Builder& b) : builder(b) {}

        bool Null() {
            builder.OnNull();
            return true;
        }
        bool Bool(bool b) {
            builder.OnBool(b);
            return true;
        }
        bool Int(int i) {
            builder.OnInt(static_cast<int64_t>(i));
            return true;
        }
        bool Uint(unsigned u) {
            builder.OnUint(static_cast<uint64_t>(u));
            return true;
        }
        bool Int64(int64_t i) {
            builder.OnInt(i);
            return true;
        }
        bool Uint64(uint64_t u) {
            builder.OnUint(u);
            return true;
        }
        bool Double(double d) {
            builder.OnDouble(d);
            return true;
        }
        bool String(const char* str, rapidjson::SizeType length, bool /*copy*/) {
            builder.OnString(std::string_view(str, length));
            return true;
        }
        bool Key(const char* str, rapidjson::SizeType length, bool /*copy*/) {
            builder.OnKey(std::string_view(str, length));
            return true;
        }
        bool StartObject() {
            builder.OnStartObject();
            return true;
        }
        bool EndObject(rapidjson::SizeType /*memberCount*/) {
            builder.OnEndObject();
            return true;
        }
        bool StartArray() {
            builder.OnStartArray();
            return true;
        }
        bool EndArray(rapidjson::SizeType /*elementCount*/) {
            builder.OnEndArray();
            return true;
        }
    };

    // Attempt to parse accumulated buffer and build result
    std::expected<Result, std::string> TryParse() {
        if (buffer_.empty()) {
            return builder_.Build();
        }

        // Ensure null-termination for rapidjson::StringStream
        buffer_.push_back('\0');

        // Parse the JSON data
        SaxHandler handler(builder_);
        rapidjson::Reader reader;
        rapidjson::StringStream stream(buffer_.data());

        auto result = reader.Parse(stream, handler);

        // Remove the null terminator we added
        buffer_.pop_back();

        if (result.IsError()) {
            return std::unexpected(std::string("Parse error at offset ") +
                                   std::to_string(result.Offset()) + ": " +
                                   rapidjson::GetParseError_En(result.Code()));
        }

        return builder_.Build();
    }

    void Complete(std::expected<Result, Error> result) {
        if (completed_) return;
        completed_ = true;
        if (on_complete_) {
            on_complete_(std::move(result));
        }
    }

    // Maximum buffer size for JSON responses (matches HttpClient limits)
    static constexpr size_t kMaxBufferSize = 16 * 1024 * 1024;  // 16MB

    Builder& builder_;
    Callback on_complete_;
    std::vector<char> buffer_;  // Accumulated JSON data
    bool completed_ = false;
};

}  // namespace dbn_pipe
