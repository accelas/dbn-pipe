// src/pipeline_sink.hpp
#pragma once

#include <cassert>

#include "error.hpp"
#include "event_loop.hpp"
#include "record_batch.hpp"

namespace databento_async {

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
// Thread-safety: ALL methods must be called from event loop thread.
// No atomics needed since single-threaded access is guaranteed.
template <typename Record>
class Sink {
public:
    Sink(IEventLoop& loop, PipelineBase<Record>* pipeline)
        : loop_(loop), pipeline_(pipeline) {}

    // Invalidate clears valid_ and nulls pipeline_.
    // Called from TeardownPipeline before deferred cleanup.
    void Invalidate() {
        assert(loop_.IsInEventLoopThread());
        valid_ = false;
        pipeline_ = nullptr;
    }

    // RecordSink interface - only called from event loop thread
    // Note: Handler may trigger teardown, so check valid_ on each iteration.
    void OnRecord(const Record& rec) {
        assert(loop_.IsInEventLoopThread());
        if (!valid_ || !pipeline_) return;
        pipeline_->HandleRecord(rec);
    }

    void OnData(RecordBatch&& batch) {
        assert(loop_.IsInEventLoopThread());
        if (!valid_ || !pipeline_) return;
        pipeline_->HandleRecordBatch(std::move(batch));
    }

    void OnError(const Error& e) {
        assert(loop_.IsInEventLoopThread());
        if (!valid_ || !pipeline_) return;
        pipeline_->HandlePipelineError(e);
    }

    void OnComplete() {
        assert(loop_.IsInEventLoopThread());
        if (!valid_ || !pipeline_) return;
        pipeline_->HandlePipelineComplete();
    }

private:
    IEventLoop& loop_;
    PipelineBase<Record>* pipeline_;
    bool valid_ = true;
};

}  // namespace databento_async
