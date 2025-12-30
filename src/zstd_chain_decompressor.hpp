// src/zstd_chain_decompressor.hpp
#pragma once

#include <zstd.h>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

#include "buffer_chain.hpp"
#include "error.hpp"
#include "pipeline.hpp"
#include "reactor.hpp"

namespace databento_async {

// ZstdChainDecompressor - Zero-copy streaming zstd decompression.
//
// Unlike ZstdDecompressor which outputs pmr::vector<byte>, this class writes
// decompressed data directly to BufferChain segments, eliminating the copy
// when forwarding to DbnParserComponent.
//
// Template parameter D must satisfy the ChainSink concept.
template <ChainSink D>
class ZstdChainDecompressor : public PipelineComponent<ZstdChainDecompressor<D>>,
                               public std::enable_shared_from_this<ZstdChainDecompressor<D>> {
public:
    // Factory method for shared_from_this safety
    static std::shared_ptr<ZstdChainDecompressor> Create(Reactor& reactor,
                                                          std::shared_ptr<D> downstream) {
        struct MakeSharedEnabler : public ZstdChainDecompressor {
            MakeSharedEnabler(Reactor& r, std::shared_ptr<D> ds)
                : ZstdChainDecompressor(r, std::move(ds)) {}
        };
        return std::make_shared<MakeSharedEnabler>(reactor, std::move(downstream));
    }

    ~ZstdChainDecompressor() { Cleanup(); }

    // Upstream interface: receive compressed data from HttpClient
    void Read(std::pmr::vector<std::byte> data);

    // Forward errors from upstream
    void OnError(const Error& e);

    // Handle EOF from upstream
    void OnDone();

    // Write is not supported for decompressor (data flows downstream only)
    void Write(std::pmr::vector<std::byte> /*data*/) {
        throw std::logic_error(
            "ZstdChainDecompressor::Write() is not supported");
    }

    // Set upstream for control flow (backpressure)
    void SetUpstream(Suspendable* up) { upstream_ = up; }

    // Required by PipelineComponent
    void DisableWatchers() {}

    void DoClose();

    // Suspendable hooks
    void OnSuspend() {}

    void OnResume() {
        ProcessPendingData();
    }

    void FlushAndComplete() {
        ProcessPendingData();
        downstream_->OnComplete();
        this->RequestClose();
    }

private:
    ZstdChainDecompressor(Reactor& reactor, std::shared_ptr<D> downstream);

    void ProcessPendingData();
    bool DecompressAndForward(const std::byte* input, size_t input_size);
    void Cleanup();

    std::shared_ptr<D> downstream_;
    Suspendable* upstream_ = nullptr;

    ZSTD_DStream* dstream_ = nullptr;

    // Segment pool and chain for zero-copy output
    SegmentPool segment_pool_{16};  // Pool for output segments
    BufferChain output_chain_;      // Chain passed to downstream

    // Pending input when suspended
    std::vector<std::byte> pending_input_;

    static constexpr size_t kMaxPendingInput = 16 * 1024 * 1024;  // 16MB

    size_t last_decompress_result_ = 0;
};

// Implementation

template <ChainSink D>
ZstdChainDecompressor<D>::ZstdChainDecompressor(Reactor& reactor, std::shared_ptr<D> downstream)
    : PipelineComponent<ZstdChainDecompressor<D>>(reactor), downstream_(std::move(downstream)) {

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

template <ChainSink D>
void ZstdChainDecompressor<D>::Cleanup() {
    if (dstream_) {
        ZSTD_freeDStream(dstream_);
        dstream_ = nullptr;
    }
}

template <ChainSink D>
void ZstdChainDecompressor<D>::DoClose() {
    Cleanup();
    downstream_.reset();
}

template <ChainSink D>
void ZstdChainDecompressor<D>::Read(std::pmr::vector<std::byte> data) {
    auto guard = this->TryGuard();
    if (!guard) return;

    if (this->IsSuspended()) {
        if (pending_input_.size() + data.size() > kMaxPendingInput) {
            this->EmitError(*downstream_,
                Error{ErrorCode::DecompressionError,
                      "Decompressor input buffer overflow"});
            this->RequestClose();
            return;
        }
        pending_input_.insert(pending_input_.end(), data.begin(), data.end());
        return;
    }

    if (!pending_input_.empty()) {
        pending_input_.insert(pending_input_.end(), data.begin(), data.end());
        auto combined = std::move(pending_input_);
        pending_input_.clear();
        if (!DecompressAndForward(combined.data(), combined.size())) {
            return;
        }
    } else {
        if (!DecompressAndForward(data.data(), data.size())) {
            return;
        }
    }
}

template <ChainSink D>
void ZstdChainDecompressor<D>::ProcessPendingData() {
    if (this->IsClosed() || pending_input_.empty()) return;

    auto data = std::move(pending_input_);
    pending_input_.clear();

    DecompressAndForward(data.data(), data.size());
}

template <ChainSink D>
bool ZstdChainDecompressor<D>::DecompressAndForward(const std::byte* input, size_t input_size) {
    if (!dstream_) {
        this->EmitError(*downstream_,
            Error{ErrorCode::DecompressionError, "Decompressor not initialized"});
        this->RequestClose();
        return false;
    }

    ZSTD_inBuffer in_buf = {input, input_size, 0};

    while (in_buf.pos < in_buf.size) {
        if (this->IsSuspended()) {
            // Buffer remaining input
            if (in_buf.pos < in_buf.size) {
                const auto* remaining = static_cast<const std::byte*>(in_buf.src) + in_buf.pos;
                size_t remaining_size = in_buf.size - in_buf.pos;
                pending_input_.insert(pending_input_.end(), remaining, remaining + remaining_size);
            }
            return true;
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
            return false;
        }

        last_decompress_result_ = result;

        // If we got output, append segment to chain and forward to downstream
        if (out_buf.pos > 0) {
            seg->size = out_buf.pos;
            output_chain_.Append(std::move(seg));

            // Forward to downstream - it will consume what it can
            downstream_->OnData(output_chain_);
        }
    }

    return true;
}

template <ChainSink D>
void ZstdChainDecompressor<D>::OnError(const Error& e) {
    this->EmitError(*downstream_, e);
    this->RequestClose();
}

template <ChainSink D>
void ZstdChainDecompressor<D>::OnDone() {
    auto guard = this->TryGuard();
    if (!guard) return;

    if (!pending_input_.empty()) {
        if (!DecompressAndForward(pending_input_.data(), pending_input_.size())) {
            return;
        }
        pending_input_.clear();
    }

    if (last_decompress_result_ != 0) {
        this->EmitError(*downstream_,
            Error{ErrorCode::DecompressionError, "Incomplete zstd frame"});
    } else {
        // Check for incomplete records in output chain
        if (!output_chain_.Empty()) {
            this->EmitError(*downstream_,
                Error{ErrorCode::DecompressionError,
                      "Incomplete data at end of stream (" +
                      std::to_string(output_chain_.Size()) + " bytes remaining)"});
        } else {
            this->EmitDone(*downstream_);
        }
    }
    this->RequestClose();
}

}  // namespace databento_async
