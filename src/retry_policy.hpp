// src/retry_policy.hpp
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>

namespace dbn_pipe {

struct RetryConfig {
    uint32_t max_retries = 5;
    std::chrono::milliseconds initial_delay{1000};
    std::chrono::milliseconds max_delay{60000};
    double backoff_multiplier = 2.0;
    double jitter_factor = 0.1;  // +/- 10%
};

class RetryPolicy {
public:
    explicit RetryPolicy(RetryConfig config = {})
        : config_(config), attempts_(0) {}

    bool ShouldRetry() const {
        return attempts_ < config_.max_retries;
    }

    void RecordAttempt() {
        ++attempts_;
    }

    void Reset() {
        attempts_ = 0;
    }

    // Calculate next delay with exponential backoff and jitter
    std::chrono::milliseconds GetNextDelay(
        std::optional<std::chrono::seconds> retry_after = std::nullopt) const {

        // If server specified Retry-After, use it
        if (retry_after.has_value()) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(*retry_after);
        }

        // Exponential backoff: initial * multiplier^attempts
        double delay_ms = static_cast<double>(config_.initial_delay.count());
        for (uint32_t i = 0; i < attempts_; ++i) {
            delay_ms *= config_.backoff_multiplier;
        }

        // Cap at max delay
        delay_ms = std::min(delay_ms, static_cast<double>(config_.max_delay.count()));

        // Add jitter
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<> dis(
            1.0 - config_.jitter_factor,
            1.0 + config_.jitter_factor);
        delay_ms *= dis(gen);

        return std::chrono::milliseconds(static_cast<int64_t>(delay_ms));
    }

    uint32_t Attempts() const { return attempts_; }

private:
    RetryConfig config_;
    uint32_t attempts_;
};

}  // namespace dbn_pipe
