// SPDX-License-Identifier: MIT

// lib/stream/sink.hpp
#pragma once

#include <atomic>
#include <concepts>
#include <expected>
#include <functional>

#include "lib/stream/error.hpp"
#include "src/record_batch.hpp"

namespace dbn_pipe {

// Base sink concept - lifecycle methods required for all sinks
// Named BasicSink to avoid conflict with legacy Sink<Record> template class
template<typename S>
concept BasicSink = requires(S& s, const Error& e) {
    { s.OnError(e) } -> std::same_as<void>;
    { s.OnComplete() } -> std::same_as<void>;
    { s.Invalidate() } -> std::same_as<void>;
};

// Streaming sink - receives batches of records
template<typename S>
concept StreamingSink = BasicSink<S> && requires(S& s, RecordBatch&& batch) {
    { s.OnData(std::move(batch)) } -> std::same_as<void>;
};

// StreamRecordSink - concrete streaming sink implementation
// Named to avoid conflict with the RecordSink concept in component.hpp
class StreamRecordSink {
public:
    StreamRecordSink(
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

static_assert(StreamingSink<StreamRecordSink>, "StreamRecordSink must satisfy StreamingSink");

// Single-result sink - receives one result (success or error via expected)
template<typename S>
concept SingleResultSink = BasicSink<S> && requires(S& s) {
    typename S::ResultType;
    { s.OnResult(std::declval<typename S::ResultType>()) } -> std::same_as<void>;
};

// ResultSink - concrete single-result sink implementation
template<typename Result>
class ResultSink {
public:
    using ResultType = Result;

    explicit ResultSink(std::function<void(std::expected<Result, Error>)> on_result)
        : on_result_(std::move(on_result)) {}

    void OnResult(Result&& result) {
        if (!valid_.load(std::memory_order_acquire) || delivered_) return;
        delivered_ = true;
        on_result_(std::move(result));
    }

    void OnError(const Error& e) {
        if (!valid_.load(std::memory_order_acquire) || delivered_) return;
        delivered_ = true;
        on_result_(std::unexpected(e));
    }

    void OnComplete() {
        // No-op for single-result - result already delivered
    }

    void Invalidate() { valid_.store(false, std::memory_order_release); }

private:
    std::function<void(std::expected<Result, Error>)> on_result_;
    std::atomic<bool> valid_{true};
    bool delivered_ = false;
};

}  // namespace dbn_pipe
