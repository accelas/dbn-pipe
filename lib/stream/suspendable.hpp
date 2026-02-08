// SPDX-License-Identifier: MIT

// lib/stream/suspendable.hpp
#pragma once

namespace dbn_pipe {

/// Interface for pipeline components that support backpressure.
///
/// Uses suspend-count semantics:
/// - Suspend() increments the count; Resume() decrements it.
/// - IsSuspended() returns true when the count is greater than zero.
/// - Actual pause/resume only happens on 0-to-1 and 1-to-0 transitions,
///   allowing nested suspend calls from multiple downstream sources.
///
/// Thread safety:
/// - Suspend(), Resume(), Close() must be called from the event-loop thread.
/// - IsSuspended() is thread-safe (atomic load).
///
/// OnDone coordination:
/// - If OnDone() arrives while suspended, it is deferred until Resume()
///   brings the count back to zero.
class Suspendable {
public:
    virtual ~Suspendable() = default;

    /// Increment the suspend count. When the count goes from 0 to 1,
    /// the component pauses reading from its data source.
    virtual void Suspend() = 0;

    /// Decrement the suspend count. When the count goes from 1 to 0,
    /// the component resumes reading and completes any deferred OnDone.
    virtual void Resume() = 0;

    /// Terminate the connection. No further callbacks after Close().
    virtual void Close() = 0;

    /// @return true if the component is currently suspended (count > 0).
    virtual bool IsSuspended() const = 0;
};

}  // namespace dbn_pipe
