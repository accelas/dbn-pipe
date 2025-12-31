// src/pipeline_sink.hpp
#pragma once

#include <cassert>

#include "error.hpp"
#include "reactor.hpp"
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
// Thread-safety: ALL methods must be called from reactor thread.
// No atomics needed since single-threaded access is guaranteed.
template <typename Record>
class Sink {
public:
    Sink(Reactor& reactor, PipelineBase<Record>* pipeline)
        : reactor_(reactor), pipeline_(pipeline) {}

    // Invalidate clears valid_ and nulls pipeline_.
    // Called from TeardownPipeline before deferred cleanup.
    void Invalidate() {
        assert(reactor_.IsInReactorThread());
        valid_ = false;
        pipeline_ = nullptr;
    }

    // RecordSink interface - only called from reactor thread
    // Note: Handler may trigger teardown, so check valid_ on each iteration.
    void OnRecord(const Record& rec) {
        assert(reactor_.IsInReactorThread());
        if (!valid_ || !pipeline_) return;
        pipeline_->HandleRecord(rec);
    }

    void OnRecordBatch(RecordBatch&& batch) {
        assert(reactor_.IsInReactorThread());
        if (!valid_ || !pipeline_) return;
        pipeline_->HandleRecordBatch(std::move(batch));
    }

    void OnError(const Error& e) {
        assert(reactor_.IsInReactorThread());
        if (!valid_ || !pipeline_) return;
        pipeline_->HandlePipelineError(e);
    }

    void OnComplete() {
        assert(reactor_.IsInReactorThread());
        if (!valid_ || !pipeline_) return;
        pipeline_->HandlePipelineComplete();
    }

private:
    Reactor& reactor_;
    PipelineBase<Record>* pipeline_;
    bool valid_ = true;
};

}  // namespace databento_async
