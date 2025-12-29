// src/dbn_parser_component.hpp
#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include <databento/record.hpp>

#include "error.hpp"
#include "pipeline.hpp"
#include "record_batch.hpp"

namespace databento_async {

// Maximum valid DBN record size (64KB)
// Records larger than this are considered invalid
constexpr size_t kMaxRecordSize = 64 * 1024;

// DbnParserComponent - Simplified parser that outputs RecordBatch.
//
// This component transforms raw bytes into batched records for the backpressure
// pipeline. It delegates lifecycle management to the sink, keeping the parser
// stateless except for carryover data from partial records.
//
// Key features:
// - Outputs RecordBatch instead of individual records
// - Overflow-safe bounds checking
// - Carryover buffer for partial records across OnData calls
// - One-shot error handling via atomic guard
// - DBN metadata header parsing (skips DBN file header if present)
//
// Template parameter S must satisfy RecordSink concept.
template <RecordSink S>
class DbnParserComponent {
public:
    explicit DbnParserComponent(S& sink) : sink_(sink) {}

    // Input from upstream - parse bytes into records
    void OnData(std::vector<std::byte>&& buffer);

    // Forward error to sink (one-shot)
    void OnError(const Error& e) noexcept;

    // Forward completion to sink (one-shot)
    void OnComplete() noexcept;

private:
    // Report a terminal error and set error state
    void ReportTerminalError(const std::string& msg);

    // Skip DBN metadata header if present
    // Returns true if ready to parse records, false if waiting for more data
    bool SkipMetadataIfNeeded(const std::vector<std::byte>& data, size_t& offset);

    // Peek at record size from header at given offset
    // Returns 0 if insufficient data for header
    size_t PeekRecordSize(const std::vector<std::byte>& data, size_t offset) const;

    S& sink_;                              // Reference to sink
    std::vector<std::byte> carryover_;     // Partial record from previous buffer
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
void DbnParserComponent<S>::OnData(std::vector<std::byte>&& buffer) {
    // Check error state - if already in error, ignore all data
    if (error_state_.load(std::memory_order_acquire)) {
        return;
    }

    // Combine carryover with incoming buffer
    std::vector<std::byte> data;
    if (!carryover_.empty()) {
        data = std::move(carryover_);
        data.insert(data.end(), buffer.begin(), buffer.end());
    } else {
        data = std::move(buffer);
    }

    size_t offset = 0;

    // Skip DBN metadata header if present
    if (!SkipMetadataIfNeeded(data, offset)) {
        // Need more data for metadata - save everything as carryover
        carryover_ = std::move(data);
        return;
    }

    // Parse all complete records
    RecordBatch batch;
    const size_t data_size = data.size();

    while (offset < data_size) {
        // Check if we have enough for a header
        if (data_size - offset < sizeof(databento::RecordHeader)) {
            // Partial header - save as carryover
            break;
        }

        size_t record_size = PeekRecordSize(data, offset);

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

        // Overflow-safe bounds check: record_size > data_size - offset
        // Equivalent to: offset + record_size > data_size (but overflow-safe)
        if (record_size > data_size - offset) {
            // Partial record - save as carryover
            break;
        }

        // Record is complete - add to batch
        // On first record, copy entire buffer up to end of last complete record
        if (batch.empty()) {
            // Reserve space - we'll copy the actual data after we know how much
        }
        batch.offsets.push_back(batch.buffer.size());
        batch.buffer.insert(batch.buffer.end(),
                           data.begin() + offset,
                           data.begin() + offset + record_size);

        offset += record_size;
    }

    // Save any remaining partial data as carryover
    if (offset < data_size) {
        carryover_.assign(data.begin() + offset, data.end());
    } else {
        carryover_.clear();
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

    // Check for incomplete record at end of stream
    if (!carryover_.empty()) {
        ReportTerminalError("Incomplete record at end of stream (" +
                          std::to_string(carryover_.size()) + " bytes remaining)");
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
bool DbnParserComponent<S>::SkipMetadataIfNeeded(const std::vector<std::byte>& data,
                                                   size_t& offset) {
    if (metadata_parsed_) {
        return true;
    }

    // Need at least sizeof(DbnHeader) bytes to check for magic
    if (data.size() - offset < sizeof(DbnHeader)) {
        return false;
    }

    // Read the header
    DbnHeader header;
    std::memcpy(&header, data.data() + offset, sizeof(header));

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
    if (data.size() - offset < total_metadata_size) {
        return false;
    }

    // Skip the entire metadata (header + content)
    offset += total_metadata_size;
    metadata_parsed_ = true;
    return true;
}

template <RecordSink S>
size_t DbnParserComponent<S>::PeekRecordSize(const std::vector<std::byte>& data,
                                              size_t offset) const {
    if (data.size() - offset < sizeof(databento::RecordHeader)) {
        return 0;
    }
    // Use memcpy to avoid undefined behavior from unaligned access
    databento::RecordHeader hdr;
    std::memcpy(&hdr, data.data() + offset, sizeof(hdr));
    return hdr.Size();
}

}  // namespace databento_async
