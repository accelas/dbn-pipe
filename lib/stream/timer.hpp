// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include "dbn_pipe/stream/event_loop.hpp"

namespace dbn_pipe {

/// One-shot or periodic timer built on IEventLoop::Schedule().
///
/// Works with any IEventLoop implementation (epoll, libuv adapter, etc.).
/// Safe to destroy while armed — pending callbacks are silently discarded.
///
/// @code
/// Timer heartbeat(loop);
/// heartbeat.OnTimer([] { send_ping(); });
/// heartbeat.Start(0, 5000);  // fire immediately, repeat every 5s
/// @endcode
class Timer {
public:
    using Callback = std::function<void()>;

    /// @param loop  Event loop that drives this timer
    explicit Timer(IEventLoop& loop)
        : loop_(loop), alive_(std::make_shared<bool>(true)) {}

    ~Timer() {
        *alive_ = false;
        armed_ = false;
    }

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    Timer(Timer&&) = delete;
    Timer& operator=(Timer&&) = delete;

    /// Set the callback invoked on each timer tick.
    void OnTimer(Callback cb) { callback_ = std::move(cb); }

    /// Arm the timer.
    /// @param delay_ms     Initial delay before first tick
    /// @param interval_ms  Repeat interval (0 = one-shot)
    void Start(int delay_ms, int interval_ms = 0) {
        interval_ms_ = interval_ms;
        armed_ = true;
        ScheduleNext(delay_ms);
    }

    /// Disarm the timer. No further callbacks will fire.
    void Stop() {
        armed_ = false;
        interval_ms_ = 0;
    }

    /// Return true if the timer is armed.
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
