// src/zstd_decompressor.hpp
#pragma once

#include <zstd.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <vector>

#include "buffer_chain.hpp"
#include "error.hpp"
#include "pipeline.hpp"
#include "reactor.hpp"

namespace databento_async {

// ZstdDecompressor handles streaming zstd decompression.
// Sits between HttpClient (upstream) and application (downstream) in the pipeline.
// PipelineComponent provides Suspendable interface with suspend count semantics.
//
// Template parameter D must satisfy the Downstream concept.
template <Downstream D>
class ZstdDecompressor : public PipelineComponent<ZstdDecompressor<D>>,
                         public std::enable_shared_from_this<ZstdDecompressor<D>> {
public:
    // Factory method for shared_from_this safety
    static std::shared_ptr<ZstdDecompressor> Create(Reactor& reactor,
                                                     std::shared_ptr<D> downstream) {
        struct MakeSharedEnabler : public ZstdDecompressor {
            MakeSharedEnabler(Reactor& r, std::shared_ptr<D> ds)
                : ZstdDecompressor(r, std::move(ds)) {}
        };
        return std::make_shared<MakeSharedEnabler>(reactor, std::move(downstream));
    }

    ~ZstdDecompressor() { Cleanup(); }

    // Downstream interface: receive compressed data from HttpClient
    void OnData(BufferChain& data);

    // Forward errors from upstream
    void OnError(const Error& e);

    // Handle EOF from upstream
    void OnDone();

    // Write is not supported for decompressor (data flows downstream only)
    void Write(BufferChain /*data*/) {
        throw std::logic_error(
            "ZstdDecompressor::Write() is not supported - ZstdDecompressor is a "
            "receiver-only component for decompression.");
    }

    // Set upstream for control flow (backpressure)
    void SetUpstream(Suspendable* up) { upstream_ = up; }

    // Required by PipelineComponent
    void DisableWatchers() {
        // No direct epoll watchers; decompression operates on buffered data
    }

    void DoClose();

    // Suspendable hooks (called by PipelineComponent base)
    void OnSuspend() {
        // Propagate backpressure upstream
        if (upstream_) upstream_->Suspend();
    }

    void OnResume() {
        // Re-deliver any unconsumed output from previous OnData call
        if (this->ForwardData(*downstream_, output_chain_)) return;
        // Process any buffered data
        ProcessPendingData();
        if (this->IsSuspended()) return;
        // Only propagate resume upstream if we're still not suspended
        if (upstream_) {
            upstream_->Resume();
        }
    }

    void FlushAndComplete() {
        // Process any remaining input
        ProcessPendingData();

        // If suspended during drain, defer completion
        if (this->IsSuspended()) {
            this->DeferOnDone();
            return;
        }

        // Check for incomplete zstd frame
        if (last_decompress_result_ != 0) {
            this->EmitError(*downstream_,
                Error{ErrorCode::DecompressionError, "Incomplete zstd frame"});
            this->RequestClose();
            return;
        }

        // Flush output and complete - only close if completion happened
        if (this->CompleteWithFlush(*downstream_, output_chain_)) {
            this->RequestClose();
        }
    }

private:
    // Private constructor - use Create() factory method
    ZstdDecompressor(Reactor& reactor, std::shared_ptr<D> downstream);

    // Process buffered input data
    void ProcessPendingData();

    // Decompress chain and forward to downstream
    // Consumes data from chain as it processes
    // Returns false on error
    bool DecompressChain(BufferChain& chain);

    // Decompress contiguous data and forward to downstream
    // Returns number of bytes consumed from input, or SIZE_MAX on error
    size_t DecompressAndForward(const std::byte* input, size_t input_size);

    // Cleanup zstd resources
    void Cleanup();

    // Copy remaining bytes from a partially consumed chain to fresh segments
    // Returns false if overflow detected
    bool CopyToPendingInput(BufferChain& source) {
        while (!source.Empty()) {
            // Check for overflow before copying (use subtraction to avoid overflow)
            if (source.Size() > kMaxPendingInput - pending_input_.Size()) {
                this->EmitError(*downstream_,
                    Error{ErrorCode::DecompressionError,
                          "Decompressor input buffer overflow"});
                this->RequestClose();
                return false;
            }
            size_t chunk = std::min(source.ContiguousSize(), Segment::kSize);
            auto seg = segment_pool_.Acquire();
            source.CopyTo(0, chunk, seg->data.data());
            seg->size = chunk;
            pending_input_.Append(std::move(seg));
            source.Consume(chunk);
        }
        return true;
    }

    // Downstream component
    std::shared_ptr<D> downstream_;

    // Upstream for backpressure control
    Suspendable* upstream_ = nullptr;

    // ZSTD streaming decompression context
    ZSTD_DStream* dstream_ = nullptr;

    // Segment pool for output
    SegmentPool segment_pool_{8};

    // Output chain for zero-copy delivery (persistent - retains unconsumed data)
    BufferChain output_chain_;

    // Pending input chain when suspended
    BufferChain pending_input_;

    // Buffer size constants
    static constexpr size_t kMaxPendingInput = 16 * 1024 * 1024;   // 16MB
    static constexpr size_t kMaxBufferedOutput = 16 * 1024 * 1024; // 16MB

    // Track the last return value from ZSTD_decompressStream
    // 0 means frame is complete, non-zero means more data expected
    size_t last_decompress_result_ = 0;
};

// Implementation - must be in header due to template

template <Downstream D>
ZstdDecompressor<D>::ZstdDecompressor(Reactor& reactor, std::shared_ptr<D> downstream)
    : PipelineComponent<ZstdDecompressor<D>>(reactor), downstream_(std::move(downstream)) {

    // Set up segment recycling for output chain
    output_chain_.SetRecycleCallback(segment_pool_.MakeRecycler());

    // Create ZSTD decompression stream
    dstream_ = ZSTD_createDStream();
    if (!dstream_) {
        throw std::runtime_error("Failed to create ZSTD_DStream");
    }

    // Initialize the decompression stream
    size_t init_result = ZSTD_initDStream(dstream_);
    if (ZSTD_isError(init_result)) {
        ZSTD_freeDStream(dstream_);
        dstream_ = nullptr;
        throw std::runtime_error(
            std::string("Failed to initialize ZSTD_DStream: ") +
            ZSTD_getErrorName(init_result));
    }
}

template <Downstream D>
void ZstdDecompressor<D>::Cleanup() {
    if (dstream_) {
        ZSTD_freeDStream(dstream_);
        dstream_ = nullptr;
    }
}

template <Downstream D>
void ZstdDecompressor<D>::DoClose() {
    Cleanup();
    output_chain_.Clear();
    pending_input_.Clear();
    downstream_.reset();
}

template <Downstream D>
void ZstdDecompressor<D>::OnData(BufferChain& data) {
    auto guard = this->TryGuard();
    if (!guard) return;

    // If suspended, buffer the data
    if (this->IsSuspended()) {
        // Use subtraction to avoid size_t overflow
        if (data.Size() > kMaxPendingInput - pending_input_.Size()) {
            this->EmitError(*downstream_,
                Error{ErrorCode::DecompressionError,
                      "Decompressor input buffer overflow"});
            this->RequestClose();
            return;
        }
        // Compact if partially consumed before splicing
        if (pending_input_.IsPartiallyConsumed()) {
            pending_input_.Compact();
        }
        if (data.IsPartiallyConsumed()) {
            data.Compact();
        }
        pending_input_.Splice(std::move(data));
        return;
    }

    // Process pending input first if any
    if (!pending_input_.Empty()) {
        // Check for overflow before splicing (use subtraction to avoid overflow)
        if (data.Size() > kMaxPendingInput - pending_input_.Size()) {
            this->EmitError(*downstream_,
                Error{ErrorCode::DecompressionError,
                      "Decompressor input buffer overflow"});
            this->RequestClose();
            return;
        }
        // Compact if partially consumed before splicing
        if (pending_input_.IsPartiallyConsumed()) {
            pending_input_.Compact();
        }
        if (data.IsPartiallyConsumed()) {
            data.Compact();
        }
        pending_input_.Splice(std::move(data));
        if (!DecompressChain(pending_input_)) {
            return;  // Error already emitted
        }
    } else {
        if (!DecompressChain(data)) {
            return;  // Error already emitted
        }
        // If suspended mid-processing, copy unconsumed input to pending
        if (this->IsSuspended() && !data.Empty()) {
            if (!CopyToPendingInput(data)) {
                return;  // Overflow error already emitted
            }
        }
    }
}

template <Downstream D>
void ZstdDecompressor<D>::ProcessPendingData() {
    if (this->IsClosed() || pending_input_.Empty()) return;

    if (!DecompressChain(pending_input_)) {
        return;  // Error already emitted
    }
}

template <Downstream D>
bool ZstdDecompressor<D>::DecompressChain(BufferChain& chain) {
    // Process each contiguous chunk from the chain
    while (!chain.Empty()) {
        if (this->IsSuspended()) {
            return true;  // Chain retains unconsumed data
        }

        size_t chunk_size = chain.ContiguousSize();
        const std::byte* chunk_ptr = chain.DataAt(0);

        size_t consumed = DecompressAndForward(chunk_ptr, chunk_size);
        if (consumed == SIZE_MAX) {
            return false;  // Error occurred
        }

        // Consume what was processed (may be partial if suspended mid-chunk)
        if (consumed > 0) {
            chain.Consume(consumed);
        }

        // If we got suspended during decompression, stop processing
        if (this->IsSuspended()) {
            return true;
        }
    }
    return true;
}

template <Downstream D>
size_t ZstdDecompressor<D>::DecompressAndForward(const std::byte* input, size_t input_size) {
    if (!dstream_) {
        this->EmitError(*downstream_,
            Error{ErrorCode::DecompressionError, "Decompressor not initialized"});
        this->RequestClose();
        return SIZE_MAX;  // Error
    }

    ZSTD_inBuffer in_buf = {input, input_size, 0};

    while (in_buf.pos < in_buf.size) {
        // Check if we got suspended during processing
        if (this->IsSuspended()) {
            // Return how much was consumed before suspension
            return in_buf.pos;
        }

        // Decompress into a segment
        auto out_seg = segment_pool_.Acquire();
        ZSTD_outBuffer out_buf = {out_seg->data.data(), Segment::kSize, 0};

        size_t result = ZSTD_decompressStream(dstream_, &out_buf, &in_buf);
        if (ZSTD_isError(result)) {
            this->EmitError(*downstream_,
                Error{ErrorCode::DecompressionError,
                      std::string("ZSTD decompression failed: ") + ZSTD_getErrorName(result)});
            this->RequestClose();
            return SIZE_MAX;  // Error
        }

        // Track result for incomplete frame detection in OnDone()
        last_decompress_result_ = result;

        // Forward decompressed data to downstream using persistent chain
        if (out_buf.pos > 0) {
            // Check overflow before appending (use subtraction pattern)
            if (out_buf.pos > kMaxBufferedOutput - output_chain_.Size()) {
                this->EmitError(*downstream_,
                    Error{ErrorCode::BufferOverflow, "Decompressed output buffer overflow"});
                this->RequestClose();
                return SIZE_MAX;
            }
            out_seg->size = out_buf.pos;
            output_chain_.Append(std::move(out_seg));

            // Forward to downstream - it will consume what it can
            this->ForwardData(*downstream_, output_chain_);
        }
    }

    return in_buf.pos;  // All input consumed
}

template <Downstream D>
void ZstdDecompressor<D>::OnError(const Error& e) {
    this->PropagateError(*downstream_, e);
}

template <Downstream D>
void ZstdDecompressor<D>::OnDone() {
    auto guard = this->TryGuard();
    if (!guard) return;

    // If suspended, defer OnDone until Resume()
    if (this->IsSuspended()) {
        this->DeferOnDone();
        return;
    }

    // Process any remaining buffered data
    if (!pending_input_.Empty()) {
        if (!DecompressChain(pending_input_)) {
            return;  // Error already emitted
        }
        // If suspended during drain, defer completion
        if (this->IsSuspended()) {
            this->DeferOnDone();
            return;
        }
    }

    // Check for incomplete zstd frame
    if (last_decompress_result_ != 0) {
        this->EmitError(*downstream_,
            Error{ErrorCode::DecompressionError, "Incomplete zstd frame"});
        this->RequestClose();
        return;
    }

    // Flush output and complete - only close if completion happened
    if (this->CompleteWithFlush(*downstream_, output_chain_)) {
        this->RequestClose();
    }
}

}  // namespace databento_async
