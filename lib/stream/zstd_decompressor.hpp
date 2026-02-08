// SPDX-License-Identifier: MIT

// lib/stream/zstd_decompressor.hpp
#pragma once

#include <zstd.h>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

#include "dbn_pipe/stream/buffer_chain.hpp"
#include "dbn_pipe/stream/component.hpp"
#include "dbn_pipe/stream/error.hpp"
#include "dbn_pipe/stream/event_loop.hpp"
#include "dbn_pipe/stream/segment_allocator.hpp"

namespace dbn_pipe {

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

    /// Inject an external allocator (e.g. shared with other pipeline stages).
    void SetAllocator(SegmentAllocator* alloc) { allocator_ = alloc; }

    /// Return the active allocator (injected or default).
    SegmentAllocator& GetAllocator() { return allocator_ ? *allocator_ : default_allocator_; }

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
        auto result = ProcessPendingData();
        if (result == DecompressResult::Error) return;
        if (result == DecompressResult::Suspended) {
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

    enum class DecompressResult { Complete, Suspended, Error };
    DecompressResult ProcessPendingData();
    DecompressResult DecompressChain(BufferChain& chain);
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
            auto seg = GetAllocator().Allocate();
            source.CopyTo(0, chunk, seg->data.data());
            seg->size = chunk;
            pending_input_.Append(std::move(seg));
            source.Consume(chunk);
        }
        return true;
    }

    std::shared_ptr<D> downstream_;

    ZSTD_DStream* dstream_ = nullptr;

    // Allocator for zero-copy output segments
    SegmentAllocator* allocator_ = nullptr;
    SegmentAllocator default_allocator_;
    BufferChain output_chain_;      // Chain passed to downstream

    // Pending input when suspended
    BufferChain pending_input_;

    // Buffer limits - must accommodate large DBN metadata blobs
    // OPRA option chain definitions can exceed 200MB decompressed
    // These limits prevent DoS but allow realistic data sizes
    static constexpr size_t kMaxPendingInput = 256 * 1024 * 1024;   // 256MB
    static constexpr size_t kMaxBufferedOutput = 256 * 1024 * 1024; // 256MB

    size_t last_decompress_result_ = 0;
};

// Implementation

template <Downstream D>
ZstdDecompressor<D>::ZstdDecompressor(IEventLoop& loop, std::shared_ptr<D> downstream)
    : PipelineComponent<ZstdDecompressor<D>>(loop), downstream_(std::move(downstream)) {

    // Set up segment recycling
    output_chain_.SetRecycleCallback(GetAllocator().MakeRecycler());

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

    if (!pending_input_.Empty()) {
        // Merge new data with pending input from a previous suspension
        if (pending_input_.WouldOverflow(data.Size(), kMaxPendingInput)) {
            this->EmitError(*downstream_,
                Error{ErrorCode::DecompressionError,
                      "Decompressor input buffer overflow"});
            this->RequestClose();
            return;
        }
        pending_input_.CompactAndSplice(data);
        DecompressChain(pending_input_);
    } else {
        auto result = DecompressChain(data);
        if (result == DecompressResult::Error) return;
        // Save any unconsumed input from partial decompression
        // (can't Splice because data may have consumed_offset_ > 0)
        if (!data.Empty()) {
            CopyToPendingInput(data);
        }
    }
}

template <Downstream D>
auto ZstdDecompressor<D>::ProcessPendingData() -> DecompressResult {
    if (this->IsClosed() || pending_input_.Empty()) return DecompressResult::Complete;
    return DecompressChain(pending_input_);
}

template <Downstream D>
auto ZstdDecompressor<D>::DecompressChain(BufferChain& chain) -> DecompressResult {
    if (!dstream_) {
        this->EmitError(*downstream_,
            Error{ErrorCode::DecompressionError, "Decompressor not initialized"});
        this->RequestClose();
        return DecompressResult::Error;
    }

    if (this->IsSuspended()) return DecompressResult::Suspended;

    while (!chain.Empty()) {
        size_t chunk_size = chain.ContiguousSize();
        const std::byte* chunk_ptr = chain.DataAt(0);
        ZSTD_inBuffer in_buf = {chunk_ptr, chunk_size, 0};

        while (in_buf.pos < in_buf.size) {
            auto seg = GetAllocator().Allocate();
            ZSTD_outBuffer out_buf = {seg->data.data(), Segment::kSize, 0};

            size_t result = ZSTD_decompressStream(dstream_, &out_buf, &in_buf);
            if (ZSTD_isError(result)) {
                this->EmitError(*downstream_,
                    Error{ErrorCode::DecompressionError,
                          std::string("ZSTD decompression failed: ") + ZSTD_getErrorName(result)});
                this->RequestClose();
                return DecompressResult::Error;
            }

            last_decompress_result_ = result;

            if (out_buf.pos > 0) {
                if (out_buf.pos > kMaxBufferedOutput - output_chain_.Size()) {
                    this->EmitError(*downstream_,
                        Error{ErrorCode::BufferOverflow, "Decompressed output buffer overflow"});
                    this->RequestClose();
                    return DecompressResult::Error;
                }
                seg->size = out_buf.pos;
                output_chain_.Append(std::move(seg));

                if (this->ForwardData(*downstream_, output_chain_)) {
                    chain.Consume(in_buf.pos);
                    return DecompressResult::Suspended;
                }
            }
        }
        chain.Consume(chunk_size);
    }
    return DecompressResult::Complete;
}

template <Downstream D>
void ZstdDecompressor<D>::OnError(const Error& e) {
    this->PropagateError(*downstream_, e);
}

template <Downstream D>
void ZstdDecompressor<D>::OnDone() {
    auto guard = this->TryGuard();
    if (!guard) return;

    // OnDone can arrive while suspended (e.g. upstream connection close)
    if (this->IsSuspended()) {
        this->DeferOnDone();
        return;
    }

    if (!pending_input_.Empty()) {
        auto result = DecompressChain(pending_input_);
        if (result == DecompressResult::Error) return;
        if (result == DecompressResult::Suspended) {
            this->DeferOnDone();
            return;
        }
    }

    if (last_decompress_result_ != 0) {
        this->EmitError(*downstream_,
            Error{ErrorCode::DecompressionError, "Incomplete zstd frame"});
        this->RequestClose();
        return;
    }

    if (this->CompleteWithFlush(*downstream_, output_chain_)) {
        this->RequestClose();
    }
}

}  // namespace dbn_pipe
