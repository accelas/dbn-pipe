// lib/stream/suspendable.hpp
#pragma once

namespace dbn_pipe {

// Suspendable - interface for components that support backpressure.
//
// Suspend count semantics:
// - Suspend() increments count, Resume() decrements count
// - IsSuspended() returns true when count > 0
// - Actual pause/resume only happens on 0->1 and 1->0 transitions
// - This allows nested suspend calls from multiple sources
//
// Thread safety:
// - Suspend(), Resume(), Close() MUST be called from event loop thread only.
// - IsSuspended() is thread-safe and can be called from any thread.
//
// OnDone coordination:
// - If OnDone() is received while suspended, it is deferred
// - Resume() checks for deferred OnDone and completes it when count->0
class Suspendable {
public:
    virtual ~Suspendable() = default;

    // Increment suspend count. When count goes 0->1, pause reading.
    virtual void Suspend() = 0;

    // Decrement suspend count. When count goes 1->0, resume reading
    // and complete any deferred OnDone.
    virtual void Resume() = 0;

    // Terminate the connection. After Close(), no more callbacks.
    virtual void Close() = 0;

    // Query whether suspended (count > 0). Thread-safe.
    virtual bool IsSuspended() const = 0;
};

}  // namespace dbn_pipe
