// src/dbn_parser_component.hpp
#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <string>

#include <databento/record.hpp>

#include "buffer_chain.hpp"
#include "error.hpp"
#include "pipeline_component.hpp"
#include "record_batch.hpp"

namespace dbn_pipe {

// Maximum valid DBN record size (64KB)
// Records larger than this are considered invalid
constexpr size_t kMaxRecordSize = 64 * 1024;

// DbnParserComponent - Zero-copy parser that outputs RecordBatch.
//
// This component transforms raw bytes from BufferChain into batched records
// for the backpressure pipeline. It uses zero-copy references where possible,
// only copying when records span segment boundaries.
//
// Key features:
// - Outputs RecordBatch with RecordRef entries (zero-copy when possible)
// - Uses BufferChain for input (chain manages unconsumed data)
// - Aligned scratch buffers for boundary-crossing records
// - Overflow-safe bounds checking
// - One-shot error handling via atomic guard
// - DBN metadata header parsing (skips DBN file header if present)
//
// Template parameter S must satisfy RecordSink concept.
template <RecordSink S>
class DbnParserComponent {
public:
    explicit DbnParserComponent(S& sink) : sink_(sink) {}

    // Primary interface - parse bytes from caller-managed chain into records.
    // Leaves incomplete records in the chain for next call.
    void OnData(BufferChain& chain);

    // TerminalDownstream interface
    void OnDone() noexcept { OnComplete(); }

    // Forward error to sink (one-shot)
    void OnError(const Error& e) noexcept;

    // Forward completion to sink (one-shot, no chain check).
    // Use when caller has already verified chain is empty.
    void OnComplete() noexcept;

    // Forward completion to sink (one-shot).
    // Checks that chain is empty (no incomplete records).
    void OnComplete(BufferChain& chain) noexcept;

private:
    // Report a terminal error and set error state
    void ReportTerminalError(const std::string& msg);

    // Skip DBN metadata header if present.
    // Returns true if ready to parse records, false if waiting for more data.
    bool SkipMetadataIfNeeded(BufferChain& chain);

    S& sink_;                              // Reference to sink
    std::atomic<bool> error_state_{false}; // One-shot error guard
    bool metadata_parsed_ = false;         // Whether DBN header has been skipped

    // DBN file header structure (first 8 bytes of DBN stream)
    struct DbnHeader {
        char magic[3];           // "DBN"
        std::uint8_t version;    // DBN version
        std::uint32_t frame_size; // Size of metadata AFTER this header
    } __attribute__((packed));

    static_assert(sizeof(DbnHeader) == 8, "DbnHeader must be 8 bytes");

    // Maximum metadata size to prevent DoS
    static constexpr size_t kMaxMetadataSize = 1024 * 1024;  // 1MB
};

// Implementation

template <RecordSink S>
void DbnParserComponent<S>::OnData(BufferChain& chain) {
    // Check error state - if already in error, ignore all data
    if (error_state_.load(std::memory_order_acquire)) {
        return;
    }

    // Skip DBN metadata header if present
    if (!SkipMetadataIfNeeded(chain)) {
        // Need more data for metadata - leave in chain for next call
        return;
    }

    // Parse all complete records
    RecordBatch batch;

    while (chain.Size() >= sizeof(databento::RecordHeader)) {
        // Read record size from header
        // Use zero-copy when header is contiguous and 8-byte aligned
        size_t record_size;
        const std::byte* header_ptr = nullptr;

        if (chain.IsContiguous(0, sizeof(databento::RecordHeader))) {
            header_ptr = chain.DataAt(0);
            if ((reinterpret_cast<uintptr_t>(header_ptr) % 8) == 0) {
                // Fast path: direct access to aligned header
                record_size = reinterpret_cast<const databento::RecordHeader*>(
                    header_ptr)->Size();
            } else {
                // Misaligned: copy header
                databento::RecordHeader header_copy;
                chain.CopyTo(0, sizeof(databento::RecordHeader),
                            reinterpret_cast<std::byte*>(&header_copy));
                record_size = header_copy.Size();
                header_ptr = nullptr;  // Signal that we copied
            }
        } else {
            // Header spans segments: copy
            databento::RecordHeader header_copy;
            chain.CopyTo(0, sizeof(databento::RecordHeader),
                        reinterpret_cast<std::byte*>(&header_copy));
            record_size = header_copy.Size();
        }

        // Validate record size - must be at least header size
        if (record_size < sizeof(databento::RecordHeader)) {
            ReportTerminalError("Invalid record size: smaller than header");
            return;
        }

        // Validate record size - must not exceed maximum
        if (record_size > kMaxRecordSize) {
            ReportTerminalError("Invalid record size: exceeds maximum (" +
                              std::to_string(record_size) + " > " +
                              std::to_string(kMaxRecordSize) + ")");
            return;
        }

        // Check if complete record is available
        if (chain.Size() < record_size) {
            break;  // Incomplete record, wait for more data
        }

        // Build RecordRef for this record
        RecordRef ref;
        ref.size = record_size;

        // Try zero-copy path: requires contiguous AND 8-byte aligned data
        bool use_zero_copy = false;
        if (chain.IsContiguous(0, record_size)) {
            const std::byte* ptr = chain.DataAt(0);
            // Check 8-byte alignment (required for RecordHeader access)
            if ((reinterpret_cast<uintptr_t>(ptr) % 8) == 0) {
                // Fast path: contiguous and aligned, zero copy
                ref.data = ptr;
                ref.keepalive = chain.GetSegmentAt(0);
                use_zero_copy = true;
            }
        }

        if (!use_zero_copy) {
            // Slow path: copy to aligned buffer
            // (record spans segments OR data is misaligned)
            auto scratch = std::shared_ptr<std::byte[]>(
                new (std::align_val_t{8}) std::byte[record_size],
                [](std::byte* p) { operator delete[](p, std::align_val_t{8}); }
            );
            chain.CopyTo(0, record_size, scratch.get());
            ref.data = scratch.get();
            ref.keepalive = scratch;
        }

        batch.Add(std::move(ref));
        chain.Consume(record_size);
    }

    // Deliver batch if not empty
    if (!batch.empty()) {
        sink_.OnData(std::move(batch));
    }
}

template <RecordSink S>
void DbnParserComponent<S>::OnError(const Error& e) noexcept {
    // One-shot guard - only forward first error
    if (error_state_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    sink_.OnError(e);
}

template <RecordSink S>
void DbnParserComponent<S>::OnComplete() noexcept {
    // Check error state - if already in error, ignore completion
    if (error_state_.load(std::memory_order_acquire)) {
        return;
    }
    sink_.OnComplete();
}

template <RecordSink S>
void DbnParserComponent<S>::OnComplete(BufferChain& chain) noexcept {
    // Check error state - if already in error, ignore completion
    if (error_state_.load(std::memory_order_acquire)) {
        return;
    }

    // Check for incomplete record at end of stream
    if (!chain.Empty()) {
        ReportTerminalError("Incomplete record at end of stream (" +
                          std::to_string(chain.Size()) + " bytes remaining)");
        return;
    }

    sink_.OnComplete();
}

template <RecordSink S>
void DbnParserComponent<S>::ReportTerminalError(const std::string& msg) {
    // One-shot guard - only report first error
    if (error_state_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    sink_.OnError(Error{ErrorCode::ParseError, msg});
}

template <RecordSink S>
bool DbnParserComponent<S>::SkipMetadataIfNeeded(BufferChain& chain) {
    if (metadata_parsed_) {
        return true;
    }

    // Need at least sizeof(DbnHeader) bytes to check for magic
    if (chain.Size() < sizeof(DbnHeader)) {
        return false;
    }

    // Read the header (may span segments, so always copy)
    DbnHeader header;
    chain.CopyTo(0, sizeof(header), reinterpret_cast<std::byte*>(&header));

    // Check for "DBN" magic prefix
    if (header.magic[0] != 'D' || header.magic[1] != 'B' || header.magic[2] != 'N') {
        // No DBN prefix - this might be raw records without metadata
        // (e.g., from live streaming). Treat as ready to parse records.
        metadata_parsed_ = true;
        return true;
    }

    // Validate frame_size to prevent excessive memory usage
    if (header.frame_size > kMaxMetadataSize) {
        ReportTerminalError("Invalid DBN metadata frame size: " +
                          std::to_string(header.frame_size));
        return false;
    }

    // Total metadata size = header + content
    size_t total_metadata_size = sizeof(DbnHeader) + header.frame_size;

    // Wait for complete metadata
    if (chain.Size() < total_metadata_size) {
        return false;
    }

    // Skip the entire metadata (header + content)
    chain.Consume(total_metadata_size);
    metadata_parsed_ = true;
    return true;
}

}  // namespace dbn_pipe
