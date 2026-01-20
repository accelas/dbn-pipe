// src/api/metadata_client.hpp
#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "src/api/api_pipeline.hpp"

namespace dbn_pipe {

// DatasetRange - represents the available date range for a dataset
struct DatasetRange {
    std::string start;  // Start date (inclusive)
    std::string end;    // End date (exclusive)
};

// RecordCountBuilder - builds a uint64_t record count from JSON
//
// Expected JSON: a single integer value (e.g., 12345)
// Satisfies JsonBuilder concept.
struct RecordCountBuilder {
    using Result = uint64_t;
    std::optional<uint64_t> value;

    void OnKey(std::string_view) {}
    void OnString(std::string_view) {}
    void OnInt(int64_t v) { value = static_cast<uint64_t>(v); }
    void OnUint(uint64_t v) { value = v; }
    void OnDouble(double) {}
    void OnBool(bool) {}
    void OnNull() {}
    void OnStartObject() {}
    void OnEndObject() {}
    void OnStartArray() {}
    void OnEndArray() {}

    std::expected<Result, std::string> Build() {
        if (!value) return std::unexpected("missing record count value");
        return *value;
    }
};

// BillableSizeBuilder - builds a uint64_t billable size from JSON
//
// Expected JSON: a single integer value representing bytes (e.g., 1048576)
// Satisfies JsonBuilder concept.
struct BillableSizeBuilder {
    using Result = uint64_t;
    std::optional<uint64_t> value;

    void OnKey(std::string_view) {}
    void OnString(std::string_view) {}
    void OnInt(int64_t v) { value = static_cast<uint64_t>(v); }
    void OnUint(uint64_t v) { value = v; }
    void OnDouble(double) {}
    void OnBool(bool) {}
    void OnNull() {}
    void OnStartObject() {}
    void OnEndObject() {}
    void OnStartArray() {}
    void OnEndArray() {}

    std::expected<Result, std::string> Build() {
        if (!value) return std::unexpected("missing billable size value");
        return *value;
    }
};

// CostBuilder - builds a double cost value from JSON
//
// Expected JSON: a single numeric value representing cost (e.g., 0.50)
// Satisfies JsonBuilder concept.
struct CostBuilder {
    using Result = double;
    std::optional<double> value;

    void OnKey(std::string_view) {}
    void OnString(std::string_view) {}
    void OnInt(int64_t v) { value = static_cast<double>(v); }
    void OnUint(uint64_t v) { value = static_cast<double>(v); }
    void OnDouble(double v) { value = v; }
    void OnBool(bool) {}
    void OnNull() {}
    void OnStartObject() {}
    void OnEndObject() {}
    void OnStartArray() {}
    void OnEndArray() {}

    std::expected<Result, std::string> Build() {
        if (!value) return std::unexpected("missing cost value");
        return *value;
    }
};

// DatasetRangeBuilder - builds a DatasetRange from JSON
//
// Expected JSON: {"start": "2023-01-01", "end": "2026-01-20"}
// Satisfies JsonBuilder concept.
struct DatasetRangeBuilder {
    using Result = DatasetRange;
    std::string current_key;
    std::optional<std::string> start;
    std::optional<std::string> end;

    void OnKey(std::string_view key) { current_key = key; }
    void OnString(std::string_view value) {
        if (current_key == "start") {
            start = value;
        } else if (current_key == "end") {
            end = value;
        }
    }
    void OnInt(int64_t) {}
    void OnUint(uint64_t) {}
    void OnDouble(double) {}
    void OnBool(bool) {}
    void OnNull() {}
    void OnStartObject() {}
    void OnEndObject() {}
    void OnStartArray() {}
    void OnEndArray() {}

    std::expected<Result, std::string> Build() {
        if (!start) return std::unexpected("missing 'start' field");
        if (!end) return std::unexpected("missing 'end' field");
        return DatasetRange{*start, *end};
    }
};

// MetadataClient - client for Databento metadata API endpoints
//
// Provides methods to query metadata information:
// - get_record_count: Number of records for a query
// - get_billable_size: Billable size in bytes for a query
// - get_cost: Cost estimate for a query
// - get_dataset_range: Available date range for a dataset
//
// Note: This is a placeholder class. Full implementation requires
// DNS resolution and retry policy infrastructure from future tasks.
class MetadataClient {
public:
    // Placeholder - full implementation in future tasks
    MetadataClient() = default;
};

}  // namespace dbn_pipe
