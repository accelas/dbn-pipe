// src/zstd_decompressor.hpp
#pragma once

#include <zstd.h>

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <vector>

#include "error.hpp"
#include "pipeline.hpp"
#include "reactor.hpp"
#include "tls_socket.hpp"

namespace databento_async {

// ZstdDecompressor handles streaming zstd decompression.
// Sits between HttpClient (upstream) and application (downstream) in the pipeline.
//
// Template parameter D must satisfy the Downstream concept.
template <Downstream D>
class ZstdDecompressor : public PipelineComponent<ZstdDecompressor<D>>,
                         public Suspendable,
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

    // Upstream interface: receive compressed data from HttpClient
    void Read(std::pmr::vector<std::byte> data);

    // Forward errors from upstream
    void OnError(const Error& e);

    // Handle EOF from upstream
    void OnDone();

    // Suspendable interface
    void Suspend() override {
        suspended_ = true;
    }

    void Resume() override {
        suspended_ = false;
        // Process any buffered data
        ProcessPendingData();
    }

    void Close() override {
        this->RequestClose();
    }

    bool IsSuspended() const override {
        return suspended_;
    }

    // Write is not supported for decompressor (data flows downstream only)
    void Write(std::pmr::vector<std::byte> /*data*/) {
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

private:
    // Private constructor - use Create() factory method
    ZstdDecompressor(Reactor& reactor, std::shared_ptr<D> downstream);

    // Process buffered input data
    void ProcessPendingData();

    // Decompress data and forward to downstream
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

    // Backpressure state
    bool suspended_ = false;

    // PMR pool for output buffers
    std::pmr::unsynchronized_pool_resource pool_;
    std::pmr::polymorphic_allocator<std::byte> alloc_{&pool_};

    // Pending input data when suspended
    std::pmr::vector<std::byte> pending_input_{&pool_};

    // Buffer size constants
    // ZSTD_DStreamOutSize() returns 128KB (131072) but is not constexpr
    static constexpr size_t kOutputBufferSize = 128 * 1024;
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
void ZstdDecompressor<D>::Read(std::pmr::vector<std::byte> data) {
    auto guard = this->TryGuard();
    if (!guard) return;

    // If suspended, buffer the data
    if (suspended_) {
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

    // If we have pending data, prepend it
    if (!pending_input_.empty()) {
        pending_input_.insert(pending_input_.end(), data.begin(), data.end());
        auto combined = std::move(pending_input_);
        pending_input_ = std::pmr::vector<std::byte>{&pool_};
        if (!DecompressAndForward(combined.data(), combined.size())) {
            return;  // Error already emitted
        }
    } else {
        if (!DecompressAndForward(data.data(), data.size())) {
            return;  // Error already emitted
        }
    }
}

template <Downstream D>
void ZstdDecompressor<D>::ProcessPendingData() {
    if (this->IsClosed() || pending_input_.empty()) return;

    auto data = std::move(pending_input_);
    pending_input_ = std::pmr::vector<std::byte>{&pool_};

    if (!DecompressAndForward(data.data(), data.size())) {
        return;  // Error already emitted
    }
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
        if (suspended_) {
            // Buffer remaining input
            if (in_buf.pos < in_buf.size) {
                const auto* remaining = static_cast<const std::byte*>(in_buf.src) + in_buf.pos;
                size_t remaining_size = in_buf.size - in_buf.pos;
                pending_input_.insert(pending_input_.end(), remaining, remaining + remaining_size);
            }
            return true;
        }

        // Allocate output buffer
        std::pmr::vector<std::byte> output(kOutputBufferSize, alloc_);
        ZSTD_outBuffer out_buf = {output.data(), output.size(), 0};

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
            output.resize(out_buf.pos);
            downstream_->Read(std::move(output));
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
    if (!pending_input_.empty()) {
        if (!DecompressAndForward(pending_input_.data(), pending_input_.size())) {
            return;  // Error already emitted
        }
        pending_input_.clear();
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
