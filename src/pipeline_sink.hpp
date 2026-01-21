// src/pipeline_sink.hpp
#pragma once

#include <atomic>
#include <cassert>

#include "lib/stream/error.hpp"
#include "lib/stream/event_loop.hpp"
#include "record_batch.hpp"

namespace dbn_pipe {

// Forward declaration for templated record type
// Pipeline implementations define their own record type
template <typename Record>
class PipelineBase {
public:
    virtual ~PipelineBase() = default;

    virtual void HandleRecord(const Record& rec) = 0;
    virtual void HandleRecordBatch(RecordBatch&& batch) = 0;
    virtual void HandlePipelineError(const Error& e) = 0;
    virtual void HandlePipelineComplete() = 0;
};

// Sink bridges protocol components to Pipeline callbacks.
//
// Thread-safety:
// - Callback methods (OnRecord, OnData, etc.) must be called from event loop thread
// - Invalidate() may be called from any thread (for destruction scenarios)
//
// The valid_ flag is atomic to support cross-thread Invalidate() during shutdown.
// After Invalidate(), callbacks become no-ops regardless of which thread calls them.
template <typename Record>
class Sink {
public:
    Sink(IEventLoop& loop, PipelineBase<Record>* pipeline)
        : loop_(loop), pipeline_(pipeline) {}

    // Invalidate clears valid_ and nulls pipeline_.
    // Called from TeardownPipeline before deferred cleanup.
    //
    // Thread-safety: May be called from any thread. During normal operation,
    // this is called from the event loop thread. During destruction (e.g.,
    // Pipeline destructor after reactor thread has exited), this may be
    // called from a different thread. The atomic valid_ flag ensures
    // visibility, and callbacks check valid_ before dereferencing pipeline_.
    void Invalidate() {
        valid_.store(false, std::memory_order_release);
        pipeline_ = nullptr;
    }

    // RecordSink interface - only called from event loop thread
    // Note: Handler may trigger teardown, so check valid_ on each iteration.
    void OnRecord(const Record& rec) {
        assert(loop_.IsInEventLoopThread());
        if (!valid_.load(std::memory_order_acquire) || !pipeline_) return;
        pipeline_->HandleRecord(rec);
    }

    void OnData(RecordBatch&& batch) {
        assert(loop_.IsInEventLoopThread());
        if (!valid_.load(std::memory_order_acquire) || !pipeline_) return;
        pipeline_->HandleRecordBatch(std::move(batch));
    }

    void OnError(const Error& e) {
        assert(loop_.IsInEventLoopThread());
        if (!valid_.load(std::memory_order_acquire) || !pipeline_) return;
        pipeline_->HandlePipelineError(e);
    }

    void OnComplete() {
        assert(loop_.IsInEventLoopThread());
        if (!valid_.load(std::memory_order_acquire) || !pipeline_) return;
        pipeline_->HandlePipelineComplete();
    }

private:
    IEventLoop& loop_;
    PipelineBase<Record>* pipeline_;
    std::atomic<bool> valid_{true};
};

}  // namespace dbn_pipe
