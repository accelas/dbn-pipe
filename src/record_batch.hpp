#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <vector>

#include <databento/record.hpp>

namespace databento_async {

// RecordBatch owns a buffer and provides safe access to parsed DBN records.
// Uses offsets instead of pointers to avoid alignment UB.
//
// This struct is used for batched record delivery in the backpressure pipeline.
// Instead of delivering records one-by-one via callbacks, the parser batches
// multiple records into a RecordBatch and delivers them to the sink in one call.
struct RecordBatch {
    std::vector<std::byte> buffer;   // Owns the raw data (may not be aligned)
    std::vector<size_t> offsets;     // Byte offsets into buffer for each record

    // Number of records in the batch
    size_t size() const { return offsets.size(); }

    // Check if batch contains no records
    bool empty() const { return offsets.empty(); }

    // Safe record access - copies header to avoid alignment issues.
    // The returned header is a copy that can be safely accessed without
    // alignment concerns.
    databento::RecordHeader GetHeader(size_t index) const {
        assert(index < offsets.size() && "index out of bounds");
        assert(offsets[index] + sizeof(databento::RecordHeader) <= buffer.size()
               && "header would read past buffer");
        databento::RecordHeader header;
        std::memcpy(&header, buffer.data() + offsets[index], sizeof(header));
        return header;
    }

    // Get pointer to raw record data at the given index.
    // Caller is responsible for ensuring proper alignment or using memcpy
    // to access the data safely.
    const std::byte* GetRecordData(size_t index) const {
        assert(index < offsets.size() && "index out of bounds");
        return buffer.data() + offsets[index];
    }
};

}  // namespace databento_async
