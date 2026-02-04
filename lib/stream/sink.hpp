// SPDX-License-Identifier: MIT

// lib/stream/sink.hpp
#pragma once

#include <atomic>
#include <concepts>
#include <expected>
#include <functional>

#include "dbn_pipe/stream/error.hpp"
#include "dbn_pipe/record_batch.hpp"

namespace dbn_pipe {

/// Concept for the minimal sink lifecycle: error, completion, and invalidation.
///
/// Every sink must support OnError, OnComplete, and Invalidate.
template<typename S>
concept BasicSink = requires(S& s, const Error& e) {
    { s.OnError(e) } -> std::same_as<void>;
    { s.OnComplete() } -> std::same_as<void>;
    { s.Invalidate() } -> std::same_as<void>;
};

/// Concept for a streaming sink that receives batches of records.
///
/// Refines BasicSink by adding an OnData callback for RecordBatch delivery.
template<typename S>
concept StreamingSink = BasicSink<S> && requires(S& s, RecordBatch&& batch) {
    { s.OnData(std::move(batch)) } -> std::same_as<void>;
};

/// Concrete streaming sink that dispatches records through user-provided callbacks.
///
/// All callbacks are guarded by an atomic validity flag: once Invalidate()
/// is called, subsequent OnData / OnError / OnComplete calls are silently
/// dropped, preventing use-after-free when the downstream consumer is torn
/// down before the producer.
class StreamRecordSink {
public:
    /// Construct with callbacks for data, error, and completion events.
    /// @param on_data      Invoked for each incoming RecordBatch.
    /// @param on_error     Invoked when the stream encounters an error.
    /// @param on_complete  Invoked when the stream ends normally.
    StreamRecordSink(
        std::function<void(RecordBatch&&)> on_data,
        std::function<void(const Error&)> on_error,
        std::function<void()> on_complete
    ) : on_data_(std::move(on_data)),
        on_error_(std::move(on_error)),
        on_complete_(std::move(on_complete)) {}

    /// Deliver a batch of records to the downstream consumer.
    void OnData(RecordBatch&& batch) {
        if (valid_.load(std::memory_order_acquire)) on_data_(std::move(batch));
    }

    /// Report an error to the downstream consumer.
    void OnError(const Error& e) {
        if (valid_.load(std::memory_order_acquire)) on_error_(e);
    }

    /// Signal normal stream completion.
    void OnComplete() {
        if (valid_.load(std::memory_order_acquire)) on_complete_();
    }

    /// Atomically disable all future callback dispatches.
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
