// SPDX-License-Identifier: MIT

// src/retry_policy.hpp
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>

#include "dbn_pipe/stream/error.hpp"

namespace dbn_pipe {

/// Configuration for exponential backoff retry behavior.
struct RetryConfig {
    uint32_t max_retries = 3;                          ///< Maximum retry attempts
    std::chrono::milliseconds initial_delay{1000};     ///< Delay before first retry
    std::chrono::milliseconds max_delay{30000};        ///< Delay cap
    double backoff_multiplier = 2.0;                   ///< Multiplier per attempt
    double jitter_factor = 0.1;                        ///< Random jitter range (+/- fraction)

    /// Preset for API calls (metadata, symbology): fast retry, fewer attempts.
    static RetryConfig ApiDefaults() {
        return RetryConfig{
            .max_retries = 3,
            .initial_delay = std::chrono::milliseconds{1000},
            .max_delay = std::chrono::milliseconds{10000},
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.1,
        };
    }

    /// Preset for downloads (historical data): more patience, more retries.
    static RetryConfig DownloadDefaults() {
        return RetryConfig{
            .max_retries = 5,
            .initial_delay = std::chrono::milliseconds{2000},
            .max_delay = std::chrono::milliseconds{60000},
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.1,
        };
    }
};

/// Stateful retry policy with exponential backoff, jitter, and error classification.
///
/// Tracks attempt count and computes delays. Classifies ErrorCode as retryable
/// or permanent to avoid wasting retries on non-transient failures.
class RetryPolicy {
public:
    explicit RetryPolicy(RetryConfig config = {})
        : config_(config), attempts_(0) {}

    /// Return true if the retry budget has not been exhausted.
    bool ShouldRetry() const {
        return attempts_ < config_.max_retries;
    }

    /// Increment the attempt counter.
    void RecordAttempt() {
        ++attempts_;
    }

    /// Reset the attempt counter to zero.
    void Reset() {
        attempts_ = 0;
    }

    /// Calculate next delay with exponential backoff and jitter.
    /// @param retry_after  Server-specified delay (overrides backoff if present)
    std::chrono::milliseconds GetNextDelay(
        std::optional<std::chrono::seconds> retry_after = std::nullopt) const {

        // If server specified Retry-After, use it
        if (retry_after.has_value()) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(*retry_after);
        }

        return CalculateBackoff();
    }

    /// Classify error and check retry budget.
    /// @return false for permanent errors (Unauthorized, NotFound, etc.)
    bool ShouldRetry(const Error& e) const {
        if (!IsRetryable(e.code)) {
            return false;
        }
        return attempts_ < config_.max_retries;
    }

    /// Compute delay using Error::retry_after if present, else backoff.
    std::chrono::milliseconds GetNextDelay(const Error& e) const {
        if (e.retry_after.has_value()) {
            return *e.retry_after;
        }
        return CalculateBackoff();
    }

    /// Return true if the error code represents a transient failure.
    static bool IsRetryable(ErrorCode code) {
        switch (code) {
            // Retryable errors (transient failures)
            case ErrorCode::ConnectionFailed:
            case ErrorCode::DnsResolutionFailed:  // DNS can be temporarily unavailable
            case ErrorCode::ServerError:
            case ErrorCode::TlsHandshakeFailed:
            case ErrorCode::RateLimited:
                return true;

            // Non-retryable errors (permanent failures)
            case ErrorCode::Unauthorized:
            case ErrorCode::NotFound:
            case ErrorCode::ValidationError:
            case ErrorCode::ParseError:
            default:
                return false;
        }
    }

    /// Return the number of recorded attempts.
    uint32_t Attempts() const { return attempts_; }

private:
    // Calculate delay using exponential backoff with jitter
    std::chrono::milliseconds CalculateBackoff() const {
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

    RetryConfig config_;
    uint32_t attempts_;
};

}  // namespace dbn_pipe
