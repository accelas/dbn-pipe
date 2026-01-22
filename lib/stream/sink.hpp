// lib/stream/sink.hpp
#pragma once

#include <concepts>

#include "lib/stream/error.hpp"

namespace dbn_pipe {

// Base sink concept - lifecycle methods required for all sinks
template<typename S>
concept Sink = requires(S& s, const Error& e) {
    { s.OnError(e) } -> std::same_as<void>;
    { s.OnComplete() } -> std::same_as<void>;
    { s.Invalidate() } -> std::same_as<void>;
};

// Forward declaration
class RecordBatch;

// Streaming sink - receives batches of records
template<typename S>
concept StreamingSink = Sink<S> && requires(S& s, RecordBatch&& batch) {
    { s.OnData(std::move(batch)) } -> std::same_as<void>;
};

}  // namespace dbn_pipe
