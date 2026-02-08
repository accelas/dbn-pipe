// SPDX-License-Identifier: MIT

// lib/stream/component.hpp
#pragma once

#include <atomic>
#include <cassert>
#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>

#include "dbn_pipe/stream/error.hpp"
#include "dbn_pipe/stream/segment_allocator.hpp"
#include "dbn_pipe/stream/suspendable.hpp"

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

// Conditional storage for downstream shared_ptr.
// When D != void, stores a shared_ptr<D> with accessors.
// When D == void, empty (zero overhead).
template<typename D>
struct DownstreamStorage {
    std::shared_ptr<D> downstream_;
    void SetDownstream(std::shared_ptr<D> ds) { downstream_ = std::move(ds); }
    D& GetDownstream() { assert(downstream_); return *downstream_; }
    const D& GetDownstream() const { assert(downstream_); return *downstream_; }
    std::shared_ptr<D>& GetDownstreamPtr() { return downstream_; }
    const std::shared_ptr<D>& GetDownstreamPtr() const { return downstream_; }
    void ResetDownstream() { downstream_.reset(); }
};

template<>
struct DownstreamStorage<void> {};

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
template<typename Derived, typename D = void>
class PipelineComponent : public Suspendable, protected DownstreamStorage<D> {
public:
    using DeferFn = std::function<void(std::function<void()>)>;

    // Default constructor - use SetDefer() to wire deferred close behavior
    PipelineComponent() = default;

    void SetDefer(DeferFn fn) { defer_ = std::move(fn); }

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

    // =========================================================================
    // Downstream helpers (use stored downstream from DownstreamStorage<D>)
    // =========================================================================

    // Terminal emission - one-shot error delivery with finalization guard
    void EmitError(const Error& e) requires (!std::is_void_v<D>) {
        if (finalized_) return;
        finalized_ = true;
        ProcessingGuard guard(*this);
        this->GetDownstream().OnError(e);
    }

    // Terminal emission - one-shot done delivery with finalization guard
    void EmitDone() requires (!std::is_void_v<D>) {
        if (finalized_) return;
        finalized_ = true;
        ProcessingGuard guard(*this);
        this->GetDownstream().OnDone();
    }

    // Forward data to downstream.
    // Returns true if downstream suspended us (caller should return early).
    template<typename Chain>
    bool ForwardData(Chain& chain) requires (!std::is_void_v<D>) {
        if (chain.Empty()) return false;
        this->GetDownstream().OnData(chain);
        return IsSuspended();
    }

    // Propagate upstream error to downstream.
    // Handles guard check, emits error, and requests close.
    void PropagateError(const Error& e) requires (!std::is_void_v<D>) {
        auto guard = TryGuard();
        if (!guard) return;
        EmitError(e);
        static_cast<Derived*>(this)->RequestClose();
    }

    // Flush pending data before completing. Returns true if should defer completion.
    template<typename Chain>
    bool FlushPendingData(Chain& chain) requires (!std::is_void_v<D>) {
        if (chain.Empty()) return false;
        this->GetDownstream().OnData(chain);
        if (IsSuspended()) {
            DeferOnDone();
            return true;
        }
        if (!chain.Empty()) {
            EmitError(Error{ErrorCode::ParseError,
                      "Incomplete data (" + std::to_string(chain.Size()) + " bytes remaining)"});
            static_cast<Derived*>(this)->RequestClose();
            return true;
        }
        return false;
    }

    // Complete with Done after flushing pending data.
    // Returns true if completion happened, false if deferred.
    template<typename Chain>
    bool CompleteWithFlush(Chain& chain) requires (!std::is_void_v<D>) {
        if (FlushPendingData(chain)) return false;
        EmitDone();
        return true;
    }

    // =========================================================================
    // Suspendable interface implementation
    // =========================================================================

    // Increment suspend count. On 0→1 transition, propagates suspend upstream.
    void Suspend() override {
        int prev = suspend_count_.fetch_add(1, std::memory_order_acq_rel);
        if (prev == 0) {
            // 0→1 transition: propagate backpressure upstream
            if (upstream_) upstream_->Suspend();
        }
    }

    // Decrement suspend count. On 1→0 transition, processes pending data
    // and completes any deferred OnDone.
    void Resume() override {
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

    // Set an external allocator (e.g., shared across pipeline stages).
    // If not set, a default SegmentAllocator is used.
    void SetAllocator(SegmentAllocator* alloc) { allocator_ = alloc; }

    // Get the active allocator (external if set, otherwise default).
    SegmentAllocator& GetAllocator() { return allocator_ ? *allocator_ : default_allocator_; }

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

    // Standard buffer limit (16MB) - components can use smaller limits if needed
    static constexpr size_t kDefaultBufferLimit = 16 * 1024 * 1024;

protected:
    void ScheduleClose() {
        if (close_scheduled_) return;
        close_scheduled_ = true;
        close_pending_ = false;

        if (defer_) {
            if constexpr (requires { static_cast<Derived*>(this)->weak_from_this(); }) {
                auto self = static_cast<Derived*>(this)->weak_from_this().lock();
                if (!self) return;
                defer_([self]() { self->DoClose(); });
            } else {
                // No shared ownership - capture raw pointer, defer must execute before destruction
                auto* raw = static_cast<Derived*>(this);
                defer_([raw]() { raw->DoClose(); });
            }
        } else {
            static_cast<Derived*>(this)->DoClose();
        }
    }

    Suspendable* upstream_ = nullptr;  // Upstream for backpressure propagation
    SegmentAllocator* allocator_ = nullptr;
    SegmentAllocator default_allocator_;

private:
    DeferFn defer_;
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
