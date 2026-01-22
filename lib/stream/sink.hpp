// lib/stream/sink.hpp
#pragma once

#include <atomic>
#include <concepts>
#include <functional>

#include "lib/stream/error.hpp"
#include "src/record_batch.hpp"

namespace dbn_pipe {

// Base sink concept - lifecycle methods required for all sinks
template<typename S>
concept Sink = requires(S& s, const Error& e) {
    { s.OnError(e) } -> std::same_as<void>;
    { s.OnComplete() } -> std::same_as<void>;
    { s.Invalidate() } -> std::same_as<void>;
};

// Streaming sink - receives batches of records
template<typename S>
concept StreamingSink = Sink<S> && requires(S& s, RecordBatch&& batch) {
    { s.OnData(std::move(batch)) } -> std::same_as<void>;
};

// RecordSink - concrete streaming sink implementation
class RecordSink {
public:
    RecordSink(
        std::function<void(RecordBatch&&)> on_data,
        std::function<void(const Error&)> on_error,
        std::function<void()> on_complete
    ) : on_data_(std::move(on_data)),
        on_error_(std::move(on_error)),
        on_complete_(std::move(on_complete)) {}

    void OnData(RecordBatch&& batch) {
        if (valid_.load(std::memory_order_acquire)) on_data_(std::move(batch));
    }

    void OnError(const Error& e) {
        if (valid_.load(std::memory_order_acquire)) on_error_(e);
    }

    void OnComplete() {
        if (valid_.load(std::memory_order_acquire)) on_complete_();
    }

    void Invalidate() { valid_.store(false, std::memory_order_release); }

private:
    std::function<void(RecordBatch&&)> on_data_;
    std::function<void(const Error&)> on_error_;
    std::function<void()> on_complete_;
    std::atomic<bool> valid_{true};
};

static_assert(StreamingSink<RecordSink>, "RecordSink must satisfy StreamingSink");

}  // namespace dbn_pipe
