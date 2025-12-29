// src/pipeline.hpp
#pragma once

#include <concepts>
#include <functional>
#include <memory_resource>
#include <optional>

#include <databento/record.hpp>

#include "error.hpp"
#include "reactor.hpp"

namespace databento_async {

// TerminalDownstream interface - minimal interface for error/done signals
template<typename D>
concept TerminalDownstream = requires(D& d, const Error& e) {
    { d.OnError(e) } -> std::same_as<void>;
    { d.OnDone() } -> std::same_as<void>;
};

// Downstream interface - receives data flowing toward application
template<typename D>
concept Downstream = TerminalDownstream<D> && requires(D& d, std::pmr::vector<std::byte> data) {
    { d.Read(std::move(data)) } -> std::same_as<void>;
};

// RecordDownstream interface - receives parsed records
// NOTE: Records are only valid for the duration of the OnRecord() call.
// Implementations must copy any data they need to retain.
template<typename D>
concept RecordDownstream = TerminalDownstream<D> && requires(D& d, const databento::Record& rec) {
    { d.OnRecord(rec) } -> std::same_as<void>;
};

// Forward declaration for RecordBatch
struct RecordBatch;

// RecordSink interface - receives batched records for backpressure pipeline
// Used by simplified components that delegate lifecycle management to the sink
template<typename S>
concept RecordSink = requires(S& s, RecordBatch&& batch, const Error& e) {
    { s.OnData(std::move(batch)) } -> std::same_as<void>;
    { s.OnError(e) } -> std::same_as<void>;
    { s.OnComplete() } -> std::same_as<void>;
};

// Upstream interface - control flowing toward socket
template<typename U>
concept Upstream = requires(U& u, std::pmr::vector<std::byte> data) {
    { u.Write(std::move(data)) } -> std::same_as<void>;
    { u.Suspend() } -> std::same_as<void>;
    { u.Resume() } -> std::same_as<void>;
    { u.Close() } -> std::same_as<void>;
};

// CRTP base - provides reentrancy-safe close with C++23 enhancements
template<typename Derived>
class PipelineComponent {
public:
    explicit PipelineComponent(Reactor& reactor) : reactor_(reactor) {}

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
        reactor_.Defer([self]() {
            self->DoClose();
        });
    }

    Reactor& reactor_;

private:
    int processing_count_ = 0;
    bool close_pending_ = false;
    bool close_scheduled_ = false;
    bool closed_ = false;
    bool finalized_ = false;
};

}  // namespace databento_async
