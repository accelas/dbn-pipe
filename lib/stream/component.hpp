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

/// Concept for types that can receive terminal signals (error/done).
///
/// Satisfied by any type with `OnError(const Error&)` and `OnDone()` methods.
template<typename D>
concept TerminalDownstream = requires(D& d, const Error& e) {
    { d.OnError(e) } -> std::same_as<void>;
    { d.OnDone() } -> std::same_as<void>;
};

class RecordBatch;
class BufferChain;

/// Concept for sinks that receive batched records in the backpressure pipeline.
///
/// Used by terminal components (e.g., DbnParserComponent) that deliver parsed
/// records to user code.
template<typename S>
concept RecordSink = requires(S& s, RecordBatch&& batch, const Error& e) {
    { s.OnData(std::move(batch)) } -> std::same_as<void>;
    { s.OnError(e) } -> std::same_as<void>;
    { s.OnComplete() } -> std::same_as<void>;
};

/// Concept for downstream pipeline stages that receive data via BufferChain.
///
/// All middleware pipeline components (TlsTransport, HttpClient, etc.) satisfy
/// this concept. Data flows downstream via `OnData(BufferChain&)`.
template<typename D>
concept Downstream = requires(D& d, BufferChain& chain, const Error& e) {
    { d.OnData(chain) } -> std::same_as<void>;
    { d.OnError(e) } -> std::same_as<void>;
    { d.OnDone() } -> std::same_as<void>;
};

/// Concept for upstream pipeline stages (control flows toward the socket).
///
/// Provides `Write`, `Suspend`, `Resume`, and `Close` for sending data and
/// propagating backpressure toward the network layer.
template<typename U>
concept Upstream = requires(U& u, BufferChain chain) {
    { u.Write(std::move(chain)) } -> std::same_as<void>;
    { u.Suspend() } -> std::same_as<void>;
    { u.Resume() } -> std::same_as<void>;
    { u.Close() } -> std::same_as<void>;
};

/// Conditional storage for a downstream shared_ptr.
///
/// When `D != void`, stores a `shared_ptr<D>` with accessors for setting,
/// getting, and resetting the downstream reference. When `D == void` (terminal
/// nodes), the specialization is empty with zero overhead.
///
/// @tparam D  Downstream type, or `void` for terminal nodes.
template<typename D>
struct DownstreamStorage {
    std::shared_ptr<D> downstream_;  ///< Owning reference to the downstream stage.

    /// Store the downstream stage.
    void SetDownstream(std::shared_ptr<D> ds) { downstream_ = std::move(ds); }

    /// @return Reference to the downstream stage.
    /// @pre SetDownstream() must have been called.
    D& GetDownstream() { assert(downstream_); return *downstream_; }

    /// @return Const reference to the downstream stage.
    /// @pre SetDownstream() must have been called.
    const D& GetDownstream() const { assert(downstream_); return *downstream_; }

    /// @return The shared_ptr holding the downstream stage.
    std::shared_ptr<D>& GetDownstreamPtr() { return downstream_; }

    /// @return Const reference to the shared_ptr holding the downstream stage.
    const std::shared_ptr<D>& GetDownstreamPtr() const { return downstream_; }

    /// Release the downstream reference (sets to nullptr).
    void ResetDownstream() { downstream_.reset(); }
};

/// @cond
template<>
struct DownstreamStorage<void> {};
/// @endcond

/// CRTP base for pipeline components with reentrancy-safe lifecycle management.
///
/// Combines lifecycle management with the Suspendable backpressure interface:
/// - Reentrancy-safe close via ProcessingGuard
/// - Suspend count for nested backpressure (Suspend/Resume propagation)
/// - Deferred OnDone when suspended
/// - Optional downstream storage via DownstreamStorage<D>
///
/// When `D != void`, provides downstream helper methods (EmitError, EmitDone,
/// ForwardData, PropagateError, FlushPendingData, CompleteWithFlush) that
/// operate on the stored downstream reference.
///
/// When `D == void` (terminal nodes like DbnParserComponent), the downstream
/// helpers are disabled at compile time via `requires` clauses.
///
/// Derived classes must implement:
/// - `DoClose()` --- cleanup on close
/// - `DisableWatchers()` --- disable I/O watchers
/// - `ProcessPending()` --- forward buffered data and process pending input
/// - `FlushAndComplete()` --- flush pending data and emit OnDone
///
/// @tparam Derived  The CRTP derived class.
/// @tparam D        Downstream type, or `void` for terminal nodes.
template<typename Derived, typename D = void>
class PipelineComponent : public Suspendable, protected DownstreamStorage<D> {
public:
    /// Callback type for deferring close to the event loop.
    using DeferFn = std::function<void(std::function<void()>)>;

    /// Default constructor. Call SetDefer() to wire deferred close behavior.
    PipelineComponent() = default;

    /// Set the defer callback for scheduling close on the event loop.
    /// Typically wired by the protocol during chain construction:
    /// @code
    ///   comp->SetDefer([&loop](auto fn) { loop.Defer(std::move(fn)); });
    /// @endcode
    void SetDefer(DeferFn fn) { defer_ = std::move(fn); }

    /// RAII guard for reentrancy-safe processing.
    ///
    /// Increments `processing_count_` on construction and decrements on
    /// destruction. If a close was requested during processing, ScheduleClose()
    /// fires when the last guard is destroyed. Move-safe via an active flag.
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

    /// Create a ProcessingGuard if the component is not closed.
    /// @return A guard on success, or `std::nullopt` if already closed.
    [[nodiscard]] std::optional<ProcessingGuard> TryGuard() {
        if (closed_) return std::nullopt;
        return ProcessingGuard{*this};
    }

    /// Request component shutdown (reentrancy-safe, deducing-this dispatch).
    ///
    /// Marks the component as closed and disables watchers. If a
    /// ProcessingGuard is active, the actual close is deferred until the
    /// guard is destroyed.
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

    /// @return true if RequestClose() has been called.
    bool IsClosed() const { return closed_; }

    /// @return true if a terminal signal (error or done) has been emitted.
    bool IsFinalized() const { return finalized_; }
    /// Mark the component as finalized (terminal signal emitted).
    void SetFinalized() { finalized_ = true; }
    /// Clear the finalized flag (for reuse scenarios).
    void ResetFinalized() { finalized_ = false; }

    // =========================================================================
    // Downstream helpers (available only when D != void)
    // =========================================================================

    /// Emit a terminal error to downstream (one-shot, guarded by finalized flag).
    /// @param e  The error to forward.
    void EmitError(const Error& e) requires (!std::is_void_v<D>) {
        if (finalized_) return;
        finalized_ = true;
        ProcessingGuard guard(*this);
        this->GetDownstream().OnError(e);
    }

    /// Emit a terminal done signal to downstream (one-shot, guarded by finalized flag).
    void EmitDone() requires (!std::is_void_v<D>) {
        if (finalized_) return;
        finalized_ = true;
        ProcessingGuard guard(*this);
        this->GetDownstream().OnDone();
    }

    /// Forward data to the stored downstream stage.
    /// @return true if downstream suspended this component (caller should stop).
    template<typename Chain>
    bool ForwardData(Chain& chain) requires (!std::is_void_v<D>) {
        if (chain.Empty()) return false;
        this->GetDownstream().OnData(chain);
        return IsSuspended();
    }

    /// Propagate an upstream error to downstream, then request close.
    /// Combines TryGuard + EmitError + RequestClose in one call.
    void PropagateError(const Error& e) requires (!std::is_void_v<D>) {
        auto guard = TryGuard();
        if (!guard) return;
        EmitError(e);
        static_cast<Derived*>(this)->RequestClose();
    }

    /// Flush pending data to downstream before completing.
    /// @return true if completion should be deferred (suspended or incomplete data).
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

    /// Flush pending data, then emit OnDone if nothing was deferred.
    /// @return true if completion happened immediately, false if deferred.
    template<typename Chain>
    bool CompleteWithFlush(Chain& chain) requires (!std::is_void_v<D>) {
        if (FlushPendingData(chain)) return false;
        EmitDone();
        return true;
    }

    // =========================================================================
    // Suspendable interface
    // =========================================================================

    /// Increment the suspend count. On 0-to-1 transition, propagates Suspend()
    /// to the upstream component for backpressure.
    void Suspend() override {
        int prev = suspend_count_.fetch_add(1, std::memory_order_acq_rel);
        if (prev == 0) {
            // 0→1 transition: propagate backpressure upstream
            if (upstream_) upstream_->Suspend();
        }
    }

    /// Decrement the suspend count. On 1-to-0 transition, calls
    /// `ProcessPending()` to drain buffered data, resumes upstream, and
    /// completes any deferred OnDone.
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

    /// Set the upstream Suspendable for backpressure propagation.
    void SetUpstream(Suspendable* up) { upstream_ = up; }

    /// Set an external allocator shared across pipeline stages.
    /// If not set, a per-component default SegmentAllocator is used.
    void SetAllocator(SegmentAllocator* alloc) { allocator_ = alloc; }

    /// @return The active allocator (external if set, otherwise default).
    SegmentAllocator& GetAllocator() { return allocator_ ? *allocator_ : default_allocator_; }

    /// @copydoc Suspendable::Close
    void Close() override {
        static_cast<Derived*>(this)->RequestClose();
    }

    /// @copydoc Suspendable::IsSuspended
    bool IsSuspended() const override {
        return suspend_count_.load(std::memory_order_acquire) > 0;
    }

    /// Mark OnDone as pending (called when OnDone arrives while suspended).
    void DeferOnDone() {
        done_pending_ = true;
    }

    /// @return true if an OnDone signal is deferred (waiting for Resume).
    bool IsOnDonePending() const {
        return done_pending_;
    }

    /// Standard buffer limit (16 MB). Components may use smaller limits.
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

    Suspendable* upstream_ = nullptr;  ///< Upstream for backpressure propagation.
    SegmentAllocator* allocator_ = nullptr;
    SegmentAllocator default_allocator_;

private:
    DeferFn defer_;
    int processing_count_ = 0;
    bool close_pending_ = false;
    bool close_scheduled_ = false;
    bool closed_ = false;
    bool finalized_ = false;

    std::atomic<int> suspend_count_{0};  ///< Suspend count (>0 means suspended).
    bool done_pending_ = false;          ///< OnDone received while suspended.
};

}  // namespace dbn_pipe
