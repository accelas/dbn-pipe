// SPDX-License-Identifier: MIT

// lib/stream/component.hpp
#pragma once

#include <atomic>
#include <cassert>
#include <concepts>
#include <functional>
#include <memory_resource>
#include <optional>
#include <string>

#include "lib/stream/error.hpp"
#include "lib/stream/event_loop.hpp"
#include "lib/stream/suspendable.hpp"

namespace dbn_pipe {

// TerminalDownstream interface - minimal interface for error/done signals
template<typename D>
concept TerminalDownstream = requires(D& d, const Error& e) {
    { d.OnError(e) } -> std::same_as<void>;
    { d.OnDone() } -> std::same_as<void>;
};

// Forward declaration for RecordBatch
class RecordBatch;

// Forward declaration for BufferChain
class BufferChain;

// RecordSink interface - receives batched records for backpressure pipeline
// Used by simplified components that delegate lifecycle management to the sink
template<typename S>
concept RecordSink = requires(S& s, RecordBatch&& batch, const Error& e) {
    { s.OnData(std::move(batch)) } -> std::same_as<void>;
    { s.OnError(e) } -> std::same_as<void>;
    { s.OnComplete() } -> std::same_as<void>;
};

// Downstream interface - receives data via BufferChain for zero-copy access
// All pipeline components use this unified interface
template<typename D>
concept Downstream = requires(D& d, BufferChain& chain, const Error& e) {
    { d.OnData(chain) } -> std::same_as<void>;
    { d.OnError(e) } -> std::same_as<void>;
    { d.OnDone() } -> std::same_as<void>;
};

// Upstream interface - control flowing toward socket
template<typename U>
concept Upstream = requires(U& u, BufferChain chain) {
    { u.Write(std::move(chain)) } -> std::same_as<void>;
    { u.Suspend() } -> std::same_as<void>;
    { u.Resume() } -> std::same_as<void>;
    { u.Close() } -> std::same_as<void>;
};

// CRTP base - provides reentrancy-safe close with backpressure support.
//
// Combines lifecycle management with Suspendable interface:
// - Reentrancy-safe close via processing guards
// - Suspend count for nested backpressure
// - Deferred OnDone when suspended
// - Automatic upstream backpressure propagation
//
// Derived classes must implement:
// - DoClose() - cleanup on close
// - DisableWatchers() - disable I/O watchers
// - ProcessPending() - forward buffered data and process pending input
// - FlushAndComplete() - flush pending data and emit OnDone (for deferred OnDone)
template<typename Derived>
class PipelineComponent : public Suspendable {
public:
    explicit PipelineComponent(IEventLoop& loop) : loop_(loop) {}

    // RAII guard for reentrancy-safe processing
    // Move-safe via active flag to prevent double-decrement
    class ProcessingGuard {
    public:
        explicit ProcessingGuard(PipelineComponent& c) : comp_(&c), active_(true) {
            ++comp_->processing_count_;
        }
        ~ProcessingGuard() {
            if (active_) {
                if (--comp_->processing_count_ == 0 && comp_->close_pending_) {
                    comp_->ScheduleClose();
                }
            }
        }
        ProcessingGuard(const ProcessingGuard&) = delete;
        ProcessingGuard& operator=(const ProcessingGuard&) = delete;
        ProcessingGuard(ProcessingGuard&& other) noexcept
            : comp_(other.comp_), active_(other.active_) {
            other.active_ = false;
        }
        ProcessingGuard& operator=(ProcessingGuard&&) = delete;
    private:
        PipelineComponent* comp_;
        bool active_;
    };

    // C++23 TryGuard pattern - combines closed check with guard creation
    [[nodiscard]] std::optional<ProcessingGuard> TryGuard() {
        if (closed_) return std::nullopt;
        return ProcessingGuard{*this};
    }

    // C++23 deducing this for CRTP dispatch
    void RequestClose(this auto&& self) {
        if (self.closed_) return;
        self.closed_ = true;
        self.DisableWatchers();

        if (self.processing_count_ > 0) {
            self.close_pending_ = true;
            return;
        }
        self.ScheduleClose();
    }

    bool IsClosed() const { return closed_; }

    // Per-message terminal guard
    bool IsFinalized() const { return finalized_; }
    void SetFinalized() { finalized_ = true; }
    void ResetFinalized() { finalized_ = false; }

    // Terminal emission with concept constraint
    template<TerminalDownstream D>
    void EmitError(D& downstream, const Error& e) {
        if (finalized_) return;
        finalized_ = true;
        ProcessingGuard guard(*this);
        downstream.OnError(e);
    }

    template<TerminalDownstream D>
    void EmitDone(D& downstream) {
        if (finalized_) return;
        finalized_ = true;
        ProcessingGuard guard(*this);
        downstream.OnDone();
    }

    // =========================================================================
    // Suspendable interface implementation
    // =========================================================================

    // Increment suspend count. On 0→1 transition, propagates suspend upstream.
    void Suspend() override {
        assert(loop_.IsInEventLoopThread() && "Suspend must be called from event loop thread");
        int prev = suspend_count_.fetch_add(1, std::memory_order_acq_rel);
        if (prev == 0) {
            // 0→1 transition: propagate backpressure upstream
            if (upstream_) upstream_->Suspend();
        }
    }

    // Decrement suspend count. On 1→0 transition, processes pending data
    // and completes any deferred OnDone.
    void Resume() override {
        assert(loop_.IsInEventLoopThread() && "Resume must be called from event loop thread");
        int prev = suspend_count_.fetch_sub(1, std::memory_order_acq_rel);
        assert(prev > 0 && "Resume called more times than Suspend");
        if (prev == 1) {
            // 1→0 transition: process pending data and resume upstream
            auto guard = TryGuard();
            if (guard) {
                static_cast<Derived*>(this)->ProcessPending();
                if (!IsSuspended() && upstream_) {
                    upstream_->Resume();
                }
            }

            // Complete deferred OnDone if pending AND still not suspended
            // (ProcessPending might have pushed data causing downstream to re-suspend)
            if (done_pending_ && !IsSuspended()) {
                done_pending_ = false;
                static_cast<Derived*>(this)->FlushAndComplete();
            }
        }
    }

    // Set upstream for backpressure propagation
    void SetUpstream(Suspendable* up) { upstream_ = up; }

    // Terminate connection via RequestClose
    void Close() override {
        static_cast<Derived*>(this)->RequestClose();
    }

    // Query suspend state (thread-safe)
    bool IsSuspended() const override {
        return suspend_count_.load(std::memory_order_acquire) > 0;
    }

    // Mark OnDone as pending (called by derived when OnDone received while suspended)
    void DeferOnDone() {
        done_pending_ = true;
    }

    // Check if OnDone is deferred
    bool IsOnDonePending() const {
        return done_pending_;
    }

    // =========================================================================
    // Helper methods for common patterns
    // =========================================================================

    // Standard buffer limit (16MB) - components can use smaller limits if needed
    static constexpr size_t kDefaultBufferLimit = 16 * 1024 * 1024;

    // Forward data to downstream, handling common patterns.
    // Returns true if downstream suspended us (caller should return early).
    // After return, check chain.Empty() - non-empty means unconsumed data.
    // Chain type is templated to avoid requiring full BufferChain definition here.
    template<typename D, typename Chain>
    bool ForwardData(D& downstream, Chain& chain) {
        if (chain.Empty()) return false;
        downstream.OnData(chain);
        return IsSuspended();
    }

    // Check buffer size and emit overflow error if exceeded.
    // Returns true if overflow detected (caller should return).
    // NOTE: This helper is deprecated - use subtraction pattern before append instead.
    template<TerminalDownstream D>
    bool CheckOverflow(D& downstream, size_t current, size_t limit,
                       const char* buffer_name = "Buffer") {
        if (current <= limit) return false;
        EmitError(downstream, Error{ErrorCode::BufferOverflow,
                  std::string(buffer_name) + " overflow"});
        static_cast<Derived*>(this)->RequestClose();
        return true;
    }

    // Propagate upstream error to downstream (common OnError pattern).
    // Handles guard check, emits error, and requests close.
    template<typename D>
    void PropagateError(D& downstream, const Error& e) {
        auto guard = TryGuard();
        if (!guard) return;
        EmitError(downstream, e);
        static_cast<Derived*>(this)->RequestClose();
    }

    // Flush pending data before completing. Returns true if should defer completion.
    // Use in DoClose/OnDone when you have pending data to deliver.
    // Chain type is templated to avoid requiring full BufferChain definition here.
    template<typename D, typename Chain>
    bool FlushPendingData(D& downstream, Chain& chain) {
        if (chain.Empty()) return false;
        downstream.OnData(chain);
        if (IsSuspended()) {
            DeferOnDone();
            return true;  // Caller should return without clearing/emitting Done
        }
        // Check for unconsumed data (protocol violation by downstream)
        if (!chain.Empty()) {
            EmitError(downstream, Error{ErrorCode::ParseError,
                      "Incomplete data (" + std::to_string(chain.Size()) + " bytes remaining)"});
            static_cast<Derived*>(this)->RequestClose();
            return true;
        }
        return false;
    }

    // Complete with Done after flushing pending data.
    // Combines flush + emit in one call for OnDone handlers.
    // Returns true if completion happened, false if deferred (caller should NOT close).
    // Chain type is templated to avoid requiring full BufferChain definition here.
    template<typename D, typename Chain>
    bool CompleteWithFlush(D& downstream, Chain& chain) {
        if (FlushPendingData(downstream, chain)) return false;  // Deferred
        EmitDone(downstream);
        return true;  // Completed
    }

protected:
    void ScheduleClose() {
        if (close_scheduled_) return;
        close_scheduled_ = true;
        close_pending_ = false;

        // Use weak_from_this to safely check if the object is still alive
        auto weak_self = static_cast<Derived*>(this)->weak_from_this();
        auto self = weak_self.lock();
        if (!self) {
            // Object is already being destroyed, skip deferred close
            return;
        }
        loop_.Defer([self]() {
            self->DoClose();
        });
    }

    IEventLoop& loop_;
    Suspendable* upstream_ = nullptr;  // Upstream for backpressure propagation

private:
    int processing_count_ = 0;
    bool close_pending_ = false;
    bool close_scheduled_ = false;
    bool closed_ = false;
    bool finalized_ = false;

    // Backpressure state
    std::atomic<int> suspend_count_{0};  // Suspend count (>0 means suspended)
    bool done_pending_ = false;          // OnDone received while suspended
};

}  // namespace dbn_pipe
