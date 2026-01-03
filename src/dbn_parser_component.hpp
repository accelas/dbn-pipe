// src/dbn_parser_component.hpp
#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <string>

#include <databento/record.hpp>

#include "lib/stream/buffer_chain.hpp"
#include "lib/stream/error.hpp"
#include "lib/stream/component.hpp"
#include "record_batch.hpp"

namespace dbn_pipe {

// Maximum valid DBN record size (64KB)
// Records larger than this are considered invalid
constexpr size_t kMaxRecordSize = 64 * 1024;

// InstrumentDefMsg v3 size for buffer allocation
constexpr size_t kInstrumentDefMsgV3Size = sizeof(databento::InstrumentDefMsg);

// StatMsg sizes for version conversion
constexpr size_t kStatMsgV1Size = 64;
constexpr size_t kStatMsgV3Size = sizeof(databento::StatMsg);
static_assert(kStatMsgV3Size == 80, "StatMsg v3 must be 80 bytes");

// ErrorMsg sizes for version conversion
constexpr size_t kErrorMsgV1Size = 80;
constexpr size_t kErrorMsgV3Size = sizeof(databento::ErrorMsg);
static_assert(kErrorMsgV3Size == 320, "ErrorMsg v3 must be 320 bytes");

// SystemMsg sizes for version conversion
constexpr size_t kSystemMsgV1Size = 80;
constexpr size_t kSystemMsgV3Size = sizeof(databento::SystemMsg);
static_assert(kSystemMsgV3Size == 320, "SystemMsg v3 must be 320 bytes");

// SymbolMappingMsg sizes for version conversion
constexpr size_t kSymbolMappingMsgV1Size = 80;
constexpr size_t kSymbolMappingMsgV3Size = sizeof(databento::SymbolMappingMsg);
static_assert(kSymbolMappingMsgV3Size == 176, "SymbolMappingMsg v3 must be 176 bytes");

// Symbol string lengths by version
constexpr size_t kSymbolCstrLenV1 = 22;
constexpr size_t kSymbolCstrLenV3 = 71;

// DBN rtype constants
constexpr std::uint8_t kRTypeInstrumentDef = 0x13;
constexpr std::uint8_t kRTypeStatistics = 0x11;
constexpr std::uint8_t kRTypeError = 0x15;
constexpr std::uint8_t kRTypeSymbolMapping = 0x16;
constexpr std::uint8_t kRTypeSystem = 0x17;

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

    // Get the v3 size for a given rtype that needs conversion.
    // Returns 0 if no conversion needed for this rtype.
    size_t GetV3SizeForConversion(std::uint8_t rtype) const;

    // Convert a record from v1/v2 to v3 format.
    // Dispatches to the appropriate type-specific converter.
    // Returns true if conversion succeeded.
    bool ConvertRecordToV3(std::uint8_t rtype, const std::byte* src,
                           std::byte* dst, size_t dst_size);

    // Type-specific converters (delegate to detail namespace helpers)
    bool ConvertInstrumentDef(const std::byte* src, std::byte* dst, size_t dst_size);
    bool ConvertStat(const std::byte* src, std::byte* dst, size_t dst_size);
    bool ConvertError(const std::byte* src, std::byte* dst, size_t dst_size);
    bool ConvertSystem(const std::byte* src, std::byte* dst, size_t dst_size);
    bool ConvertSymbolMapping(const std::byte* src, std::byte* dst, size_t dst_size);

    S& sink_;                              // Reference to sink
    std::atomic<bool> error_state_{false}; // One-shot error guard
    bool metadata_parsed_ = false;         // Whether DBN header has been skipped
    std::uint8_t dbn_version_ = 3;         // DBN version from metadata (default v3)

    // DBN file header structure (first 8 bytes of DBN stream)
    struct DbnHeader {
        char magic[3];           // "DBN"
        std::uint8_t version;    // DBN version
        std::uint32_t frame_size; // Size of metadata AFTER this header
    } __attribute__((packed));

    static_assert(sizeof(DbnHeader) == 8, "DbnHeader must be 8 bytes");

    // Maximum metadata size to prevent DoS
    // Large option chains (e.g., SPY.OPT) can have 10k+ contracts = 2-3MB metadata
    static constexpr size_t kMaxMetadataSize = 8 * 1024 * 1024;  // 8MB
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

        // Check if data is contiguous and aligned (enables zero-copy)
        const std::byte* src_ptr = nullptr;
        std::shared_ptr<void> src_keepalive;

        if (chain.IsContiguous(0, record_size)) {
            const std::byte* ptr = chain.DataAt(0);
            if ((reinterpret_cast<uintptr_t>(ptr) % 8) == 0) {
                // Data is contiguous and aligned
                src_ptr = ptr;
                src_keepalive = chain.GetSegmentAt(0);
            }
        }

        // Check if version conversion is needed
        size_t v3_size = 0;
        if (dbn_version_ != 3) {
            if (dbn_version_ > 3) {
                ReportTerminalError("Unsupported DBN version " +
                    std::to_string(dbn_version_) + " (max supported: 3)");
                return;
            }
            // Read rtype to check if this record type needs conversion
            std::uint8_t rtype;
            if (src_ptr) {
                rtype = reinterpret_cast<const databento::RecordHeader*>(src_ptr)->rtype;
            } else {
                // Read rtype from chain (offset 1 in RecordHeader)
                chain.CopyTo(1, 1, reinterpret_cast<std::byte*>(&rtype));
            }
            v3_size = GetV3SizeForConversion(rtype);
        }

        // Determine the path based on what operations are needed
        if (v3_size > 0) {
            // Conversion needed - allocate v3 buffer and convert
            auto v3_buffer = std::shared_ptr<std::byte[]>(
                new (std::align_val_t{8}) std::byte[v3_size],
                [](std::byte* p) { operator delete[](p, std::align_val_t{8}); }
            );

            std::uint8_t rtype;
            if (src_ptr) {
                // Source is contiguous - convert directly
                rtype = reinterpret_cast<const databento::RecordHeader*>(src_ptr)->rtype;
                ConvertRecordToV3(rtype, src_ptr, v3_buffer.get(), v3_size);
            } else {
                // Source spans segments - copy to thread-local temp, then convert
                alignas(8) static thread_local std::byte temp[kMaxRecordSize];
                chain.CopyTo(0, record_size, temp);
                rtype = reinterpret_cast<const databento::RecordHeader*>(temp)->rtype;
                ConvertRecordToV3(rtype, temp, v3_buffer.get(), v3_size);
            }

            ref.data = v3_buffer.get();
            ref.size = v3_size;
            ref.keepalive = v3_buffer;
        } else if (src_ptr) {
            // No conversion, data is contiguous and aligned - zero copy
            ref.data = src_ptr;
            ref.keepalive = src_keepalive;
        } else {
            // No conversion, but need alignment copy
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

    // Store the DBN version for record conversion
    dbn_version_ = header.version;

    // Skip the entire metadata (header + content)
    chain.Consume(total_metadata_size);
    metadata_parsed_ = true;
    return true;
}

template <RecordSink S>
size_t DbnParserComponent<S>::GetV3SizeForConversion(std::uint8_t rtype) const {
    switch (rtype) {
        case kRTypeInstrumentDef: return kInstrumentDefMsgV3Size;
        case kRTypeStatistics:    return kStatMsgV3Size;
        case kRTypeError:         return kErrorMsgV3Size;
        case kRTypeSystem:        return kSystemMsgV3Size;
        case kRTypeSymbolMapping: return kSymbolMappingMsgV3Size;
        default:                  return 0;  // No conversion needed
    }
}

template <RecordSink S>
bool DbnParserComponent<S>::ConvertRecordToV3(
    std::uint8_t rtype, const std::byte* src, std::byte* dst, size_t dst_size) {

    // Zero-initialize destination
    std::memset(dst, 0, dst_size);

    switch (rtype) {
        case kRTypeInstrumentDef:
            return ConvertInstrumentDef(src, dst, dst_size);
        case kRTypeStatistics:
            return ConvertStat(src, dst, dst_size);
        case kRTypeError:
            return ConvertError(src, dst, dst_size);
        case kRTypeSystem:
            return ConvertSystem(src, dst, dst_size);
        case kRTypeSymbolMapping:
            return ConvertSymbolMapping(src, dst, dst_size);
        default:
            return false;
    }
}

// ============================================================================
// Type-Specific Conversion Helpers (private implementation details)
// ============================================================================

namespace detail {

// Helper to update record header with new v3 length
inline void UpdateHeaderLength(std::byte* dst, size_t v3_size) {
    auto* hdr = reinterpret_cast<databento::RecordHeader*>(dst);
    hdr->length = static_cast<std::uint8_t>(v3_size / 4);
}

// InstrumentDefMsg v1/v2 -> v3
inline bool ConvertInstrumentDef(const std::byte* src, std::byte* dst,
                                  size_t dst_size, std::uint8_t version) {
    if (dst_size < kInstrumentDefMsgV3Size) return false;

    if (version == 1) {
        // V1 layout differs from V3: strike_price is at end (offset 328) instead of 112
        std::memcpy(dst, src, 16);  // RecordHeader
        UpdateHeaderLength(dst, kInstrumentDefMsgV3Size);
        std::memcpy(dst + 16, src + 16, 96);    // ts_recv through price_ratio
        std::memcpy(dst + 112, src + 328, 8);   // strike_price
        std::memcpy(dst + 120, src + 112, 209); // inst_attrib_value through underlying
        std::memcpy(dst + 329, src + 321, 39);  // strike_price_currency through end
        return true;
    } else if (version == 2) {
        // V2 has same field order as V3, just smaller (400 vs 520 bytes)
        std::memcpy(dst, src, 16);  // RecordHeader
        UpdateHeaderLength(dst, kInstrumentDefMsgV3Size);
        std::memcpy(dst + 8, src + 8, 392);  // Remaining V2 content
        return true;
    }
    return false;
}

// StatMsg v1/v2 -> v3 (quantity: i32 -> i64)
inline bool ConvertStat(const std::byte* src, std::byte* dst, size_t dst_size) {
    if (dst_size < kStatMsgV3Size) return false;

    std::memcpy(dst, src, 16);  // RecordHeader
    UpdateHeaderLength(dst, kStatMsgV3Size);
    std::memcpy(dst + 16, src + 16, 24);  // ts_recv, ts_ref, price

    // Sign-extend quantity from i32 to i64
    std::int32_t quantity_i32;
    std::memcpy(&quantity_i32, src + 40, 4);
    std::int64_t quantity_i64 = quantity_i32;
    std::memcpy(dst + 40, &quantity_i64, 8);

    std::memcpy(dst + 48, src + 44, 14);  // sequence through stat_flags
    return true;
}

// ErrorMsg v1 -> v3 (err: 64 -> 302 chars, added code/is_last)
inline bool ConvertError(const std::byte* src, std::byte* dst, size_t dst_size) {
    if (dst_size < kErrorMsgV3Size) return false;

    std::memcpy(dst, src, 16);  // RecordHeader
    UpdateHeaderLength(dst, kErrorMsgV3Size);
    std::memcpy(dst + 16, src + 16, 64);  // err string (zero-padded to 302)
    dst[318] = std::byte{255};  // ErrorCode::Unset
    dst[319] = std::byte{255};  // is_last unknown
    return true;
}

// SystemMsg v1 -> v3 (msg: 64 -> 303 chars, added code)
inline bool ConvertSystem(const std::byte* src, std::byte* dst, size_t dst_size) {
    if (dst_size < kSystemMsgV3Size) return false;

    std::memcpy(dst, src, 16);  // RecordHeader
    UpdateHeaderLength(dst, kSystemMsgV3Size);
    std::memcpy(dst + 16, src + 16, 64);  // msg string (zero-padded to 303)
    dst[319] = std::byte{255};  // SystemCode::Unset
    return true;
}

// SymbolMappingMsg v1 -> v3 (symbols: 22 -> 71 chars, added stype enums)
inline bool ConvertSymbolMapping(const std::byte* src, std::byte* dst, size_t dst_size) {
    if (dst_size < kSymbolMappingMsgV3Size) return false;

    std::memcpy(dst, src, 16);  // RecordHeader
    UpdateHeaderLength(dst, kSymbolMappingMsgV3Size);
    dst[16] = std::byte{255};  // stype_in invalid
    std::memcpy(dst + 17, src + 16, 22);  // stype_in_symbol
    dst[88] = std::byte{255};  // stype_out invalid
    std::memcpy(dst + 89, src + 38, 22);  // stype_out_symbol
    std::memcpy(dst + 160, src + 64, 16); // start_ts, end_ts
    return true;
}

}  // namespace detail

// Wrapper methods that delegate to detail helpers
template <RecordSink S>
bool DbnParserComponent<S>::ConvertInstrumentDef(
    const std::byte* src, std::byte* dst, size_t dst_size) {
    return detail::ConvertInstrumentDef(src, dst, dst_size, dbn_version_);
}

template <RecordSink S>
bool DbnParserComponent<S>::ConvertStat(
    const std::byte* src, std::byte* dst, size_t dst_size) {
    return detail::ConvertStat(src, dst, dst_size);
}

template <RecordSink S>
bool DbnParserComponent<S>::ConvertError(
    const std::byte* src, std::byte* dst, size_t dst_size) {
    return detail::ConvertError(src, dst, dst_size);
}

template <RecordSink S>
bool DbnParserComponent<S>::ConvertSystem(
    const std::byte* src, std::byte* dst, size_t dst_size) {
    return detail::ConvertSystem(src, dst, dst_size);
}

template <RecordSink S>
bool DbnParserComponent<S>::ConvertSymbolMapping(
    const std::byte* src, std::byte* dst, size_t dst_size) {
    return detail::ConvertSymbolMapping(src, dst, dst_size);
}

}  // namespace dbn_pipe
