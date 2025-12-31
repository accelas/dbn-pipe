// src/zstd_decompressor.hpp
#pragma once

#include <zstd.h>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

#include "buffer_chain.hpp"
#include "error.hpp"
#include "event_loop.hpp"
#include "pipeline_component.hpp"

namespace databento_async {

// ZstdDecompressor - Zero-copy streaming zstd decompression.
//
// Decompresses data directly into BufferChain segments for zero-copy
// forwarding to downstream components like DbnParserComponent.
//
// Template parameter D must satisfy the Downstream concept.
template <Downstream D>
class ZstdDecompressor : public PipelineComponent<ZstdDecompressor<D>>,
                         public std::enable_shared_from_this<ZstdDecompressor<D>> {
public:
    // Factory method for shared_from_this safety
    static std::shared_ptr<ZstdDecompressor> Create(IEventLoop& loop,
                                                    std::shared_ptr<D> downstream) {
        struct MakeSharedEnabler : public ZstdDecompressor {
            MakeSharedEnabler(IEventLoop& l, std::shared_ptr<D> ds)
                : ZstdDecompressor(l, std::move(ds)) {}
        };
        return std::make_shared<MakeSharedEnabler>(loop, std::move(downstream));
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
            "ZstdDecompressor::Write() is not supported");
    }

    // Required by PipelineComponent
    void DisableWatchers() {}

    void DoClose();

    void ProcessPending() {
        // Re-deliver any unconsumed output from previous OnData call
        if (this->ForwardData(*downstream_, output_chain_)) return;
        // Process any pending input
        ProcessPendingData();
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
    ZstdDecompressor(IEventLoop& loop, std::shared_ptr<D> downstream);

    void ProcessPendingData();
    bool DecompressChain(BufferChain& chain);
    // Returns number of bytes consumed from input, or SIZE_MAX on error
    size_t DecompressAndForward(const std::byte* input, size_t input_size);
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

    std::shared_ptr<D> downstream_;

    ZSTD_DStream* dstream_ = nullptr;

    // Segment pool and chain for zero-copy output
    SegmentPool segment_pool_{16};  // Pool for output segments
    BufferChain output_chain_;      // Chain passed to downstream

    // Pending input when suspended
    BufferChain pending_input_;

    static constexpr size_t kMaxPendingInput = 16 * 1024 * 1024;   // 16MB
    static constexpr size_t kMaxBufferedOutput = 16 * 1024 * 1024; // 16MB

    size_t last_decompress_result_ = 0;
};

// Implementation

template <Downstream D>
ZstdDecompressor<D>::ZstdDecompressor(IEventLoop& loop, std::shared_ptr<D> downstream)
    : PipelineComponent<ZstdDecompressor<D>>(loop), downstream_(std::move(downstream)) {

    // Set up segment recycling
    output_chain_.SetRecycleCallback(segment_pool_.MakeRecycler());

    dstream_ = ZSTD_createDStream();
    if (!dstream_) {
        throw std::runtime_error("Failed to create ZSTD_DStream");
    }

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

    if (this->IsSuspended()) {
        // Use subtraction to avoid size_t overflow
        if (data.Size() > kMaxPendingInput - pending_input_.Size()) {
            this->EmitError(*downstream_,
                Error{ErrorCode::DecompressionError,
                      "Decompressor input buffer overflow"});
            this->RequestClose();
            return;
        }
        // Compact both chains if partially consumed before splicing
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
        // Compact both chains if partially consumed before splicing
        if (pending_input_.IsPartiallyConsumed()) {
            pending_input_.Compact();
        }
        if (data.IsPartiallyConsumed()) {
            data.Compact();
        }
        pending_input_.Splice(std::move(data));
        if (!DecompressChain(pending_input_)) {
            return;
        }
    } else {
        if (!DecompressChain(data)) {
            return;
        }
        // If suspended mid-processing, copy unconsumed input to fresh segments
        // (can't Splice because data may have consumed_offset_ > 0)
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
        return;  // Error occurred, already handled
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
        if (this->IsSuspended()) {
            // Return how much was consumed before suspension
            return in_buf.pos;
        }

        // Acquire a segment for output
        auto seg = segment_pool_.Acquire();

        // Use segment's buffer directly for ZSTD output
        ZSTD_outBuffer out_buf = {seg->data.data(), Segment::kSize, 0};

        size_t result = ZSTD_decompressStream(dstream_, &out_buf, &in_buf);
        if (ZSTD_isError(result)) {
            this->EmitError(*downstream_,
                Error{ErrorCode::DecompressionError,
                      std::string("ZSTD decompression failed: ") + ZSTD_getErrorName(result)});
            this->RequestClose();
            return SIZE_MAX;  // Error
        }

        last_decompress_result_ = result;

        // If we got output, append segment to chain and forward to downstream
        if (out_buf.pos > 0) {
            // Check overflow before appending (use subtraction pattern)
            if (out_buf.pos > kMaxBufferedOutput - output_chain_.Size()) {
                this->EmitError(*downstream_,
                    Error{ErrorCode::BufferOverflow, "Decompressed output buffer overflow"});
                this->RequestClose();
                return SIZE_MAX;
            }
            seg->size = out_buf.pos;
            output_chain_.Append(std::move(seg));

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

    // Process any remaining input
    if (!pending_input_.Empty() && !DecompressChain(pending_input_)) {
        return;
    }

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

}  // namespace databento_async
