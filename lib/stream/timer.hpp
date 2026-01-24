// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include "lib/stream/event_loop.hpp"

namespace dbn_pipe {

// Timer using IEventLoop::Schedule()
// Portable - works with any IEventLoop implementation
class Timer {
public:
    using Callback = std::function<void()>;

    explicit Timer(IEventLoop& loop)
        : loop_(loop), alive_(std::make_shared<bool>(true)) {}

    ~Timer() {
        *alive_ = false;  // Invalidate any pending callbacks
        armed_ = false;
    }

    // Non-copyable, non-movable
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    Timer(Timer&&) = delete;
    Timer& operator=(Timer&&) = delete;

    void OnTimer(Callback cb) { callback_ = std::move(cb); }

    void Start(int delay_ms, int interval_ms = 0) {
        interval_ms_ = interval_ms;
        armed_ = true;
        ScheduleNext(delay_ms);
    }

    void Stop() {
        armed_ = false;
        interval_ms_ = 0;
    }

    bool IsArmed() const { return armed_; }

private:
    void ScheduleNext(int delay_ms) {
        // Capture shared_ptr by value - outlives Timer if needed
        std::shared_ptr<bool> alive = alive_;
        Timer* self = this;
        loop_.Schedule(std::chrono::milliseconds(delay_ms), [alive, self]() {
            if (*alive) {
                self->Fire();
            }
        });
    }

    void Fire() {
        if (!armed_) return;

        if (callback_) {
            callback_();
        }

        if (armed_ && interval_ms_ > 0) {
            ScheduleNext(interval_ms_);
        } else {
            armed_ = false;
        }
    }

    IEventLoop& loop_;
    Callback callback_;
    int interval_ms_ = 0;
    bool armed_ = false;
    std::shared_ptr<bool> alive_;
};

}  // namespace dbn_pipe
