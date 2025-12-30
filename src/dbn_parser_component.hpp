// src/dbn_parser_component.hpp
#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <new>
#include <string>

#include <databento/record.hpp>

#include "buffer_chain.hpp"
#include "error.hpp"
#include "pipeline.hpp"
#include "record_batch.hpp"

namespace databento_async {

// Maximum valid DBN record size (64KB)
// Records larger than this are considered invalid
constexpr size_t kMaxRecordSize = 64 * 1024;

// DbnParserComponent - Zero-copy parser that outputs RecordBatch.
//
// This component transforms raw bytes from BufferChain into batched records
// for the backpressure pipeline. It uses zero-copy references where possible,
// only copying when records span segment boundaries.
//
// Two input paths:
// - Direct: OnData(BufferChain&) - caller manages the chain (live path)
// - Legacy: Read(pmr::vector<byte>) - internal chain management (historical path)
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
    explicit DbnParserComponent(S& sink) : sink_(sink) {
        // Set up segment recycling for legacy Read() path
        parse_chain_.SetRecycleCallback(segment_pool_.MakeRecycler());
    }

    // Primary interface - parse bytes from caller-managed chain into records.
    // Leaves incomplete records in the chain for next call.
    void OnData(BufferChain& chain);

    // Legacy adapter interface for Downstream concept compatibility.
    // Appends data to internal chain and calls OnData.
    // Used by CramAuth, ZstdDecompressor, and other upstream components.
    void Read(std::pmr::vector<std::byte> data);

    // TerminalDownstream interface for CramAuth compatibility
    void OnDone() noexcept { OnComplete(); }

    // Forward error to sink (one-shot)
    void OnError(const Error& e) noexcept;

    // Forward completion to sink (one-shot).
    // Checks internal parse_chain_ for incomplete data when using legacy path.
    void OnComplete() noexcept;

    // Forward completion to sink (one-shot).
    // Checks that chain is empty (no incomplete records).
    // Use this overload when caller manages the chain directly.
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

    // Internal chain for legacy Read() path
    SegmentPool segment_pool_{8};          // Pool for segment allocation
    BufferChain parse_chain_;              // Internal chain for byte->chain conversion

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
        // Need to check if header is contiguous first
        databento::RecordHeader header_copy;
        size_t record_size;

        if (chain.IsContiguous(0, sizeof(databento::RecordHeader))) {
            // Fast path: header in single segment, direct access
            const auto* header = reinterpret_cast<const databento::RecordHeader*>(
                chain.DataAt(0));
            record_size = header->Size();
        } else {
            // Slow path: header spans segments, need to copy
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

        if (chain.IsContiguous(0, record_size)) {
            // Fast path: record entirely in one segment
            // Direct pointer, zero copy
            ref.data = chain.DataAt(0);
            ref.keepalive = chain.GetSegmentAt(0);  // Keep segment alive
        } else {
            // Slow path: record spans segments, allocate aligned scratch buffer
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
void DbnParserComponent<S>::Read(std::pmr::vector<std::byte> data) {
    // Check error state - if already in error, ignore all data
    if (error_state_.load(std::memory_order_acquire)) {
        return;
    }

    if (data.empty()) {
        return;
    }

    // Append incoming bytes to internal chain using segments from pool
    size_t offset = 0;
    while (offset < data.size()) {
        // Get or create a segment with space
        auto seg = segment_pool_.Acquire();

        // Copy as much as will fit
        size_t to_copy = std::min(data.size() - offset, seg->Remaining());
        std::memcpy(seg->data.data() + seg->size, data.data() + offset, to_copy);
        seg->size += to_copy;
        offset += to_copy;

        // Add segment to chain
        parse_chain_.Append(std::move(seg));
    }

    // Parse using common logic
    OnData(parse_chain_);
}

template <RecordSink S>
void DbnParserComponent<S>::OnComplete() noexcept {
    // Check error state - if already in error, ignore completion
    if (error_state_.load(std::memory_order_acquire)) {
        return;
    }

    // Check for incomplete record in internal chain (legacy path)
    if (!parse_chain_.Empty()) {
        ReportTerminalError("Incomplete record at end of stream (" +
                          std::to_string(parse_chain_.Size()) + " bytes remaining)");
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

}  // namespace databento_async
