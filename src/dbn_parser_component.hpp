// src/dbn_parser_component.hpp
#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <string>

#include <databento/record.hpp>
#include <databento/v1.hpp>
#include <databento/v2.hpp>

// kUndefPrice from databento constants
constexpr std::int64_t kUndefPrice = std::numeric_limits<std::int64_t>::max();

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
    // Large option chains can have massive metadata:
    // - Single underlying (SPY.OPT): ~4-5MB
    // - Multiple underlyings: 5 symbols = ~23MB observed
    // Set to 64MB to allow reasonable batches while still limiting DoS risk
    static constexpr size_t kMaxMetadataSize = 64 * 1024 * 1024;  // 64MB
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
// Uses databento-cpp's typed structs for correct field-by-field conversion.
// V1 (360 bytes) and V2 (456 bytes) have different layouts than V3 (520 bytes):
// - V1 has strike_price at end (offset 328), V3 at offset 104
// - V1 raw_symbol is 22 chars, V3 is 71 chars
// - V3 has additional leg_* fields
inline bool ConvertInstrumentDef(const std::byte* src, std::byte* dst,
                                  size_t dst_size, std::uint8_t version) {
    if (dst_size < kInstrumentDefMsgV3Size) return false;

    // Zero-initialize destination to handle new fields in V3
    std::memset(dst, 0, dst_size);

    if (version == 1) {
        // Inline conversion from v1 to v3 (based on databento-cpp v1.cpp)
        const auto& v1 = *reinterpret_cast<const databento::v1::InstrumentDefMsg*>(src);
        auto& v3 = *reinterpret_cast<databento::InstrumentDefMsg*>(dst);

        // RecordHeader with updated length
        v3.hd = databento::RecordHeader{
            sizeof(databento::InstrumentDefMsg) / databento::RecordHeader::kLengthMultiplier,
            databento::RType::InstrumentDef, v1.hd.publisher_id, v1.hd.instrument_id,
            v1.hd.ts_event};

        // Numeric fields
        v3.ts_recv = v1.ts_recv;
        v3.min_price_increment = v1.min_price_increment;
        v3.display_factor = v1.display_factor;
        v3.expiration = v1.expiration;
        v3.activation = v1.activation;
        v3.high_limit_price = v1.high_limit_price;
        v3.low_limit_price = v1.low_limit_price;
        v3.max_price_variation = v1.max_price_variation;
        v3.unit_of_measure_qty = v1.unit_of_measure_qty;
        v3.min_price_increment_amount = v1.min_price_increment_amount;
        v3.price_ratio = v1.price_ratio;
        v3.strike_price = v1.strike_price;
        v3.raw_instrument_id = v1.raw_instrument_id;
        v3.leg_price = kUndefPrice;  // New in V3
        v3.leg_delta = kUndefPrice;  // New in V3
        v3.inst_attrib_value = v1.inst_attrib_value;
        v3.underlying_id = v1.underlying_id;
        v3.market_depth_implied = v1.market_depth_implied;
        v3.market_depth = v1.market_depth;
        v3.market_segment_id = v1.market_segment_id;
        v3.max_trade_vol = v1.max_trade_vol;
        v3.min_lot_size = v1.min_lot_size;
        v3.min_lot_size_block = v1.min_lot_size_block;
        v3.min_lot_size_round_lot = v1.min_lot_size_round_lot;
        v3.min_trade_vol = v1.min_trade_vol;
        v3.contract_multiplier = v1.contract_multiplier;
        v3.decay_quantity = v1.decay_quantity;
        v3.original_contract_size = v1.original_contract_size;
        v3.appl_id = v1.appl_id;
        v3.maturity_year = v1.maturity_year;
        v3.decay_start_date = v1.decay_start_date;
        v3.channel_id = v1.channel_id;

        // Copy string fields (V1 has smaller strings, zero-padded in V3)
        std::copy(v1.currency.begin(), v1.currency.end(), v3.currency.begin());
        std::copy(v1.settl_currency.begin(), v1.settl_currency.end(), v3.settl_currency.begin());
        std::copy(v1.secsubtype.begin(), v1.secsubtype.end(), v3.secsubtype.begin());
        std::copy(v1.raw_symbol.begin(), v1.raw_symbol.end(), v3.raw_symbol.begin());
        std::copy(v1.group.begin(), v1.group.end(), v3.group.begin());
        std::copy(v1.exchange.begin(), v1.exchange.end(), v3.exchange.begin());
        std::copy(v1.asset.begin(), v1.asset.end(), v3.asset.begin());
        std::copy(v1.cfi.begin(), v1.cfi.end(), v3.cfi.begin());
        std::copy(v1.security_type.begin(), v1.security_type.end(), v3.security_type.begin());
        std::copy(v1.unit_of_measure.begin(), v1.unit_of_measure.end(), v3.unit_of_measure.begin());
        std::copy(v1.underlying.begin(), v1.underlying.end(), v3.underlying.begin());
        std::copy(v1.strike_price_currency.begin(), v1.strike_price_currency.end(),
                  v3.strike_price_currency.begin());

        // Trailing single-byte fields
        v3.instrument_class = v1.instrument_class;
        v3.match_algorithm = v1.match_algorithm;
        v3.main_fraction = v1.main_fraction;
        v3.price_display_format = v1.price_display_format;
        v3.sub_fraction = v1.sub_fraction;
        v3.underlying_product = v1.underlying_product;
        v3.security_update_action = v1.security_update_action;
        v3.maturity_month = v1.maturity_month;
        v3.maturity_day = v1.maturity_day;
        v3.maturity_week = v1.maturity_week;
        v3.user_defined_instrument = v1.user_defined_instrument;
        v3.contract_multiplier_unit = v1.contract_multiplier_unit;
        v3.flow_schedule_type = v1.flow_schedule_type;
        v3.tick_rule = v1.tick_rule;
        v3.leg_side = databento::Side::None;  // New in V3

        return true;
    } else if (version == 2) {
        // Inline conversion from v2 to v3 (based on databento-cpp v2.cpp)
        const auto& v2 = *reinterpret_cast<const databento::v2::InstrumentDefMsg*>(src);
        auto& v3 = *reinterpret_cast<databento::InstrumentDefMsg*>(dst);

        // RecordHeader with updated length
        v3.hd = databento::RecordHeader{
            sizeof(databento::InstrumentDefMsg) / databento::RecordHeader::kLengthMultiplier,
            databento::RType::InstrumentDef, v2.hd.publisher_id, v2.hd.instrument_id,
            v2.hd.ts_event};

        // Numeric fields
        v3.ts_recv = v2.ts_recv;
        v3.min_price_increment = v2.min_price_increment;
        v3.display_factor = v2.display_factor;
        v3.expiration = v2.expiration;
        v3.activation = v2.activation;
        v3.high_limit_price = v2.high_limit_price;
        v3.low_limit_price = v2.low_limit_price;
        v3.max_price_variation = v2.max_price_variation;
        v3.unit_of_measure_qty = v2.unit_of_measure_qty;
        v3.min_price_increment_amount = v2.min_price_increment_amount;
        v3.price_ratio = v2.price_ratio;
        v3.strike_price = v2.strike_price;
        v3.raw_instrument_id = v2.raw_instrument_id;
        v3.leg_price = kUndefPrice;  // New in V3
        v3.leg_delta = kUndefPrice;  // New in V3
        v3.inst_attrib_value = v2.inst_attrib_value;
        v3.underlying_id = v2.underlying_id;
        v3.market_depth_implied = v2.market_depth_implied;
        v3.market_depth = v2.market_depth;
        v3.market_segment_id = v2.market_segment_id;
        v3.max_trade_vol = v2.max_trade_vol;
        v3.min_lot_size = v2.min_lot_size;
        v3.min_lot_size_block = v2.min_lot_size_block;
        v3.min_lot_size_round_lot = v2.min_lot_size_round_lot;
        v3.min_trade_vol = v2.min_trade_vol;
        v3.contract_multiplier = v2.contract_multiplier;
        v3.decay_quantity = v2.decay_quantity;
        v3.original_contract_size = v2.original_contract_size;
        v3.appl_id = v2.appl_id;
        v3.maturity_year = v2.maturity_year;
        v3.decay_start_date = v2.decay_start_date;
        v3.channel_id = v2.channel_id;

        // Copy string fields (V2 has same size as V1)
        std::copy(v2.currency.begin(), v2.currency.end(), v3.currency.begin());
        std::copy(v2.settl_currency.begin(), v2.settl_currency.end(), v3.settl_currency.begin());
        std::copy(v2.secsubtype.begin(), v2.secsubtype.end(), v3.secsubtype.begin());
        std::copy(v2.raw_symbol.begin(), v2.raw_symbol.end(), v3.raw_symbol.begin());
        std::copy(v2.group.begin(), v2.group.end(), v3.group.begin());
        std::copy(v2.exchange.begin(), v2.exchange.end(), v3.exchange.begin());
        std::copy(v2.asset.begin(), v2.asset.end(), v3.asset.begin());
        std::copy(v2.cfi.begin(), v2.cfi.end(), v3.cfi.begin());
        std::copy(v2.security_type.begin(), v2.security_type.end(), v3.security_type.begin());
        std::copy(v2.unit_of_measure.begin(), v2.unit_of_measure.end(), v3.unit_of_measure.begin());
        std::copy(v2.underlying.begin(), v2.underlying.end(), v3.underlying.begin());
        std::copy(v2.strike_price_currency.begin(), v2.strike_price_currency.end(),
                  v3.strike_price_currency.begin());

        // Trailing single-byte fields
        v3.instrument_class = v2.instrument_class;
        v3.match_algorithm = v2.match_algorithm;
        v3.main_fraction = v2.main_fraction;
        v3.price_display_format = v2.price_display_format;
        v3.sub_fraction = v2.sub_fraction;
        v3.underlying_product = v2.underlying_product;
        v3.security_update_action = v2.security_update_action;
        v3.maturity_month = v2.maturity_month;
        v3.maturity_day = v2.maturity_day;
        v3.maturity_week = v2.maturity_week;
        v3.user_defined_instrument = v2.user_defined_instrument;
        v3.contract_multiplier_unit = v2.contract_multiplier_unit;
        v3.flow_schedule_type = v2.flow_schedule_type;
        v3.tick_rule = v2.tick_rule;
        v3.leg_side = databento::Side::None;  // New in V3

        return true;
    }
    return false;
}

// StatMsg v1/v2 -> v3 (quantity: i32 -> i64)
// Uses typed structs for correctness
inline bool ConvertStat(const std::byte* src, std::byte* dst, size_t dst_size) {
    if (dst_size < kStatMsgV3Size) return false;

    std::memset(dst, 0, dst_size);
    const auto& v1 = *reinterpret_cast<const databento::v1::StatMsg*>(src);
    auto& v3 = *reinterpret_cast<databento::StatMsg*>(dst);

    v3.hd = databento::RecordHeader{
        sizeof(databento::StatMsg) / databento::RecordHeader::kLengthMultiplier,
        databento::RType::Statistics, v1.hd.publisher_id, v1.hd.instrument_id,
        v1.hd.ts_event};
    v3.ts_recv = v1.ts_recv;
    v3.ts_ref = v1.ts_ref;
    v3.price = v1.price;
    // Sign-extend quantity from i32 to i64, handling undefined value
    v3.quantity = (v1.quantity == databento::v1::kUndefStatQuantity)
        ? databento::kUndefStatQuantity
        : v1.quantity;
    v3.sequence = v1.sequence;
    v3.ts_in_delta = v1.ts_in_delta;
    v3.stat_type = v1.stat_type;
    v3.channel_id = v1.channel_id;
    v3.update_action = v1.update_action;
    v3.stat_flags = v1.stat_flags;
    return true;
}

// ErrorMsg v1 -> v3 (err: 64 -> 302 chars, added code/is_last)
// Uses typed structs for correctness
inline bool ConvertError(const std::byte* src, std::byte* dst, size_t dst_size) {
    if (dst_size < kErrorMsgV3Size) return false;

    std::memset(dst, 0, dst_size);
    const auto& v1 = *reinterpret_cast<const databento::v1::ErrorMsg*>(src);
    auto& v3 = *reinterpret_cast<databento::ErrorMsg*>(dst);

    v3.hd = databento::RecordHeader{
        sizeof(databento::ErrorMsg) / databento::RecordHeader::kLengthMultiplier,
        databento::RType::Error, v1.hd.publisher_id, v1.hd.instrument_id,
        v1.hd.ts_event};
    std::copy(v1.err.begin(), v1.err.end(), v3.err.begin());
    v3.code = databento::ErrorCode::Unset;
    v3.is_last = 255;  // Unknown
    return true;
}

// SystemMsg v1 -> v3 (msg: 64 -> 303 chars, added code)
// Uses typed structs for correctness
inline bool ConvertSystem(const std::byte* src, std::byte* dst, size_t dst_size) {
    if (dst_size < kSystemMsgV3Size) return false;

    std::memset(dst, 0, dst_size);
    const auto& v1 = *reinterpret_cast<const databento::v1::SystemMsg*>(src);
    auto& v3 = *reinterpret_cast<databento::SystemMsg*>(dst);

    v3.hd = databento::RecordHeader{
        sizeof(databento::SystemMsg) / databento::RecordHeader::kLengthMultiplier,
        databento::RType::System, v1.hd.publisher_id, v1.hd.instrument_id,
        v1.hd.ts_event};
    std::copy(v1.msg.begin(), v1.msg.end(), v3.msg.begin());
    v3.code = databento::SystemCode::Unset;
    return true;
}

// SymbolMappingMsg v1 -> v3 (symbols: 22 -> 71 chars, added stype enums)
// Uses typed structs for correctness
inline bool ConvertSymbolMapping(const std::byte* src, std::byte* dst, size_t dst_size) {
    if (dst_size < kSymbolMappingMsgV3Size) return false;

    std::memset(dst, 0, dst_size);
    const auto& v1 = *reinterpret_cast<const databento::v1::SymbolMappingMsg*>(src);
    auto& v3 = *reinterpret_cast<databento::SymbolMappingMsg*>(dst);

    v3.hd = databento::RecordHeader{
        sizeof(databento::SymbolMappingMsg) / databento::RecordHeader::kLengthMultiplier,
        databento::RType::SymbolMapping, v1.hd.publisher_id, v1.hd.instrument_id,
        v1.hd.ts_event};
    // Intentionally invalid stype values for V1 data
    v3.stype_in = static_cast<databento::SType>(255);
    std::copy(v1.stype_in_symbol.begin(), v1.stype_in_symbol.end(), v3.stype_in_symbol.begin());
    v3.stype_out = static_cast<databento::SType>(255);
    std::copy(v1.stype_out_symbol.begin(), v1.stype_out_symbol.end(), v3.stype_out_symbol.begin());
    v3.start_ts = v1.start_ts;
    v3.end_ts = v1.end_ts;
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
