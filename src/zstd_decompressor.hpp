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
        // ZstdDecompressor doesn't need to do anything special on suspend
    }

    void OnResume() {
        // Process any buffered data
        ProcessPendingData();
    }

    void FlushAndComplete() {
        // Flush pending data then signal done
        ProcessPendingData();
        downstream_->OnDone();
        this->RequestClose();
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
    // Returns false on error
    bool DecompressAndForward(const std::byte* input, size_t input_size);

    // Cleanup zstd resources
    void Cleanup();

    // Downstream component
    std::shared_ptr<D> downstream_;

    // Upstream for backpressure control
    Suspendable* upstream_ = nullptr;

    // ZSTD streaming decompression context
    ZSTD_DStream* dstream_ = nullptr;

    // Segment pool for output
    SegmentPool segment_pool_{8};

    // Pending input chain when suspended
    BufferChain pending_input_;

    // Buffer size constants
    static constexpr size_t kMaxPendingInput = 16 * 1024 * 1024;  // 16MB

    // Track the last return value from ZSTD_decompressStream
    // 0 means frame is complete, non-zero means more data expected
    size_t last_decompress_result_ = 0;
};

// Implementation - must be in header due to template

template <Downstream D>
ZstdDecompressor<D>::ZstdDecompressor(Reactor& reactor, std::shared_ptr<D> downstream)
    : PipelineComponent<ZstdDecompressor<D>>(reactor), downstream_(std::move(downstream)) {
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
    downstream_.reset();
}

template <Downstream D>
void ZstdDecompressor<D>::OnData(BufferChain& data) {
    auto guard = this->TryGuard();
    if (!guard) return;

    // If suspended, buffer the data
    if (this->IsSuspended()) {
        if (pending_input_.Size() + data.Size() > kMaxPendingInput) {
            this->EmitError(*downstream_,
                Error{ErrorCode::DecompressionError,
                      "Decompressor input buffer overflow"});
            this->RequestClose();
            return;
        }
        pending_input_.Splice(std::move(data));
        return;
    }

    // Process pending input first if any
    if (!pending_input_.Empty()) {
        pending_input_.Splice(std::move(data));
        if (!DecompressChain(pending_input_)) {
            return;  // Error already emitted
        }
    } else {
        if (!DecompressChain(data)) {
            return;  // Error already emitted
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
        size_t chunk_size = chain.ContiguousSize();
        const std::byte* chunk_ptr = chain.DataAt(0);

        if (!DecompressAndForward(chunk_ptr, chunk_size)) {
            return false;
        }

        // If we got suspended, don't consume more
        if (this->IsSuspended()) {
            return true;
        }

        chain.Consume(chunk_size);
    }
    return true;
}

template <Downstream D>
bool ZstdDecompressor<D>::DecompressAndForward(const std::byte* input, size_t input_size) {
    if (!dstream_) {
        this->EmitError(*downstream_,
            Error{ErrorCode::DecompressionError, "Decompressor not initialized"});
        this->RequestClose();
        return false;
    }

    ZSTD_inBuffer in_buf = {input, input_size, 0};

    while (in_buf.pos < in_buf.size) {
        // Check if we got suspended during processing
        if (this->IsSuspended()) {
            // Buffer remaining input into pending_input_
            if (in_buf.pos < in_buf.size) {
                const auto* remaining = static_cast<const std::byte*>(in_buf.src) + in_buf.pos;
                size_t remaining_size = in_buf.size - in_buf.pos;
                auto seg = segment_pool_.Acquire();
                std::memcpy(seg->data.data(), remaining, remaining_size);
                seg->size = remaining_size;
                pending_input_.Append(std::move(seg));
            }
            return true;
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
            return false;
        }

        // Track result for incomplete frame detection in OnDone()
        last_decompress_result_ = result;

        // Forward decompressed data to downstream
        if (out_buf.pos > 0) {
            out_seg->size = out_buf.pos;
            BufferChain chain;
            chain.SetRecycleCallback(segment_pool_.MakeRecycler());
            chain.Append(std::move(out_seg));
            downstream_->OnData(chain);
        }
    }

    return true;
}

template <Downstream D>
void ZstdDecompressor<D>::OnError(const Error& e) {
    this->EmitError(*downstream_, e);
    this->RequestClose();
}

template <Downstream D>
void ZstdDecompressor<D>::OnDone() {
    auto guard = this->TryGuard();
    if (!guard) return;

    // Process any remaining buffered data
    if (!pending_input_.Empty()) {
        if (!DecompressChain(pending_input_)) {
            return;  // Error already emitted
        }
    }

    // Check for incomplete zstd frame:
    // If last_decompress_result_ is non-zero, the stream expects more data
    // to complete the current frame. A return value of 0 indicates the frame
    // was fully decoded.
    if (last_decompress_result_ != 0) {
        this->EmitError(*downstream_,
            Error{ErrorCode::DecompressionError, "Incomplete zstd frame"});
    } else {
        this->EmitDone(*downstream_);
    }
    this->RequestClose();
}

}  // namespace databento_async
