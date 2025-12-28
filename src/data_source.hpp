// src/data_source.hpp
#pragma once

#include <functional>
#include <span>

#include <databento/record.hpp>

#include "error.hpp"
#include "parser.hpp"

namespace databento_async {

class DataSource {
public:
    virtual ~DataSource() = default;

    // Control
    virtual void Start() = 0;
    virtual void Stop() = 0;

    // Backpressure
    void Pause() { paused_ = true; }
    void Resume();
    bool IsPaused() const { return paused_; }

    // Callbacks
    template <typename Handler>
    void OnRecord(Handler&& h) {
        record_handler_ = std::forward<Handler>(h);
    }

    template <typename Handler>
    void OnError(Handler&& h) {
        error_handler_ = std::forward<Handler>(h);
    }

protected:
    void DeliverBytes(std::span<const std::byte> data);
    void DeliverError(Error e);

    DbnParser parser_;
    bool paused_ = false;

private:
    void DrainParser();

    std::function<void(const databento::Record&)> record_handler_;
    std::function<void(const Error&)> error_handler_;
};

// Inline implementations

inline void DataSource::Resume() {
    paused_ = false;
    DrainParser();
}

inline void DataSource::DeliverBytes(std::span<const std::byte> data) {
    parser_.Push(data.data(), data.size());
    if (!paused_) {
        DrainParser();
    }
}

inline void DataSource::DeliverError(Error e) {
    if (error_handler_) {
        error_handler_(e);
    }
}

inline void DataSource::DrainParser() {
    while (!paused_) {
        const databento::Record* rec = parser_.Pull();
        if (!rec) break;
        if (record_handler_) {
            record_handler_(*rec);
        }
    }
}

}  // namespace databento_async
