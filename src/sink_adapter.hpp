// src/sink_adapter.hpp
#pragma once

#include <cstring>
#include <type_traits>

#include "error.hpp"
#include "pipeline_base.hpp"
#include "record_batch.hpp"

namespace databento_async {

// SinkAdapter - Bridges RecordSink (DbnParserComponent output) to Sink<Record>
//
// DbnParserComponent outputs batched records via RecordSink interface.
// This adapter converts those batches to individual Record callbacks on Sink.
//
// Template parameter Record must be trivially copyable and sized to match
// the expected DBN record format.
template <typename Record>
class SinkAdapter {
public:
    static_assert(std::is_trivially_copyable_v<Record>,
                  "Record must be trivially copyable");

    explicit SinkAdapter(Sink<Record>& sink) : sink_(sink) {}

    // RecordSink interface
    void OnData(RecordBatch&& batch) {
        for (size_t i = 0; i < batch.size(); ++i) {
            const std::byte* data = batch.GetRecordData(i);
            size_t size = batch.GetRecordSize(i);
            if (size < sizeof(Record)) continue;  // Skip malformed records

            Record rec;
            std::memcpy(&rec, data, sizeof(Record));
            sink_.OnRecord(rec);
        }
    }

    void OnError(const Error& e) {
        sink_.OnError(e);
    }

    void OnComplete() {
        sink_.OnComplete();
    }

private:
    Sink<Record>& sink_;
};

}  // namespace databento_async
