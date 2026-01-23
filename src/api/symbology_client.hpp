// SPDX-License-Identifier: MIT

// src/api/symbology_client.hpp
#pragma once

#include <expected>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "lib/stream/pipeline.hpp"
#include "src/api_protocol.hpp"
#include "src/dns_resolver.hpp"
#include "src/retry_policy.hpp"
#include "src/stype.hpp"

namespace dbn_pipe {

// SymbolInterval - represents a time range during which a symbol mapping is valid
//
// Maps to JSON: {"d0": "2025-01-01", "d1": "2025-12-31", "s": "15144"}
struct SymbolInterval {
    std::string start_date;  // d0: Start date (inclusive)
    std::string end_date;    // d1: End date (exclusive)
    std::string symbol;      // s: Resolved symbol (e.g., instrument_id as string)
};

// SymbologyResponse - result of a symbology resolution request
//
// Maps to JSON:
// {
//   "result": {
//     "SPY": [{"d0": "2025-01-01", "d1": "2025-12-31", "s": "15144"}],
//     "QQQ": [{"d0": "2025-01-01", "d1": "2025-12-31", "s": "13340"}]
//   },
//   "partial": ["AMBIGUOUS"],
//   "not_found": ["INVALID"]
// }
struct SymbologyResponse {
    // Maps input symbols to their resolved intervals
    std::map<std::string, std::vector<SymbolInterval>> result;

    // Symbols that had partial/ambiguous resolution
    std::vector<std::string> partial;

    // Symbols that were not found
    std::vector<std::string> not_found;
};

// SymbologyBuilder - builds a SymbologyResponse from JSON using a state machine
//
// This parser handles the nested JSON structure:
// - Root object with "result", "partial", "not_found" keys
// - "result" is an object where each key is a symbol name
// - Each symbol maps to an array of interval objects
// - Each interval has d0, d1, s fields
//
// State transitions:
// - Root -> InResult (on "result" key + start object)
// - InResult -> InSymbolArray (on symbol key + start array)
// - InSymbolArray -> InInterval (on start object)
// - InInterval -> InSymbolArray (on end object, save interval)
// - InSymbolArray -> InResult (on end array)
// - Root -> InPartialArray (on "partial" key + start array)
// - Root -> InNotFoundArray (on "not_found" key + start array)
//
// Satisfies JsonBuilder concept.
class SymbologyBuilder {
public:
    using Result = SymbologyResponse;

    enum class State {
        Root,           // At root level
        InResult,       // Inside "result" object
        InSymbolArray,  // Inside a symbol's array of intervals
        InInterval,     // Inside an interval object
        InPartialArray, // Inside "partial" array
        InNotFoundArray // Inside "not_found" array
    };

    void OnKey(std::string_view key) {
        current_key_ = key;

        if (state_ == State::InResult) {
            // This is a symbol name in the result object
            current_symbol_ = key;
        }
    }

    void OnString(std::string_view value) {
        switch (state_) {
            case State::InInterval:
                if (current_key_ == "d0") {
                    current_interval_.start_date = value;
                } else if (current_key_ == "d1") {
                    current_interval_.end_date = value;
                } else if (current_key_ == "s") {
                    current_interval_.symbol = value;
                }
                break;

            case State::InPartialArray:
                response_.partial.emplace_back(value);
                break;

            case State::InNotFoundArray:
                response_.not_found.emplace_back(value);
                break;

            default:
                break;
        }
    }

    void OnInt(int64_t v) {
        // Handle numeric "s" field (instrument_id as integer)
        if (state_ == State::InInterval && current_key_ == "s") {
            current_interval_.symbol = std::to_string(v);
        }
    }
    void OnUint(uint64_t v) {
        // Handle numeric "s" field (instrument_id as integer)
        if (state_ == State::InInterval && current_key_ == "s") {
            current_interval_.symbol = std::to_string(v);
        }
    }
    void OnDouble(double) {}
    void OnBool(bool) {}
    void OnNull() {}

    void OnStartObject() {
        switch (state_) {
            case State::Root:
                if (current_key_ == "result") {
                    state_ = State::InResult;
                }
                // Stay in Root if this is the root object itself
                break;

            case State::InSymbolArray:
                state_ = State::InInterval;
                current_interval_ = SymbolInterval{};
                break;

            default:
                break;
        }
    }

    void OnEndObject() {
        switch (state_) {
            case State::InInterval:
                // Only save interval if all required fields are present
                // Skip incomplete intervals to avoid silent invalid mappings
                if (!current_interval_.start_date.empty() &&
                    !current_interval_.end_date.empty() &&
                    !current_interval_.symbol.empty()) {
                    response_.result[current_symbol_].push_back(std::move(current_interval_));
                }
                state_ = State::InSymbolArray;
                break;

            case State::InResult:
                state_ = State::Root;
                break;

            default:
                break;
        }
    }

    void OnStartArray() {
        switch (state_) {
            case State::Root:
                if (current_key_ == "partial") {
                    state_ = State::InPartialArray;
                } else if (current_key_ == "not_found") {
                    state_ = State::InNotFoundArray;
                }
                break;

            case State::InResult:
                // Starting array for a symbol
                state_ = State::InSymbolArray;
                break;

            default:
                break;
        }
    }

    void OnEndArray() {
        switch (state_) {
            case State::InSymbolArray:
                state_ = State::InResult;
                break;

            case State::InPartialArray:
            case State::InNotFoundArray:
                state_ = State::Root;
                break;

            default:
                break;
        }
    }

    std::expected<Result, std::string> Build() {
        // The response is always valid, even if empty
        return response_;
    }

    // For testing: get current state
    State GetState() const { return state_; }

private:
    State state_ = State::Root;
    std::string current_key_;
    std::string current_symbol_;
    SymbolInterval current_interval_;
    SymbologyResponse response_;
};

// SymbologyClient - client for Databento symbology API endpoints
//
// Provides methods to resolve symbol mappings:
// - resolve: Convert symbols between different stype formats
//
// Automatic retry with exponential backoff on transient errors
// (ConnectionFailed, ServerError, TlsHandshakeFailed, RateLimited).
//
// Thread safety: Not thread-safe. All methods must be called from the event loop thread.
//
// Lifetime: Must be managed via shared_ptr (use Create() factory method).
// The client must outlive any in-flight requests.
class SymbologyClient : public std::enable_shared_from_this<SymbologyClient> {
public:
    static std::shared_ptr<SymbologyClient> Create(
        IEventLoop& loop, std::string api_key,
        RetryConfig retry_config = RetryConfig::ApiDefaults()) {
        return std::shared_ptr<SymbologyClient>(
            new SymbologyClient(loop, std::move(api_key), retry_config));
    }

private:
    SymbologyClient(IEventLoop& loop, std::string api_key, RetryConfig retry_config)
        : loop_(loop), api_key_(std::move(api_key)), retry_config_(retry_config) {}

public:

    void Resolve(
        const std::string& dataset,
        const std::vector<std::string>& symbols,
        SType stype_in,
        SType stype_out,
        const std::string& start_date,
        const std::string& end_date,
        std::function<void(std::expected<SymbologyResponse, Error>)> callback) {
        // Build comma-separated symbols string
        std::string symbols_str;
        symbols_str.reserve(symbols.size() * 12);  // ~10 chars per symbol + comma
        auto out = std::back_inserter(symbols_str);
        for (size_t i = 0; i < symbols.size(); ++i) {
            if (i > 0) *out++ = ',';
            out = fmt::format_to(out, "{}", symbols[i]);
        }

        ApiRequest req{
            .method = "POST",
            .path = "/v0/symbology.resolve",
            .host = kHostname,
            .port = kPort,
            .query_params = {},
            .form_params = {
                {"dataset", dataset},
                {"symbols", symbols_str},
                {"stype_in", std::string(STypeToString(stype_in))},
                {"stype_out", std::string(STypeToString(stype_out))},
                {"start_date", start_date},
                {"end_date", end_date},
            },
        };

        auto retry_state = std::make_shared<RetryPolicy>(retry_config_);
        CallApiWithRetry(req, std::move(callback), retry_state);
    }

private:
    static constexpr const char* kHostname = "hist.databento.com";
    static constexpr uint16_t kPort = 443;

    void CallApiWithRetry(
        const ApiRequest& req,
        std::function<void(std::expected<SymbologyResponse, Error>)> callback,
        std::shared_ptr<RetryPolicy> retry_state) {
        auto addr = ResolveHostname(kHostname, kPort);
        if (!addr) {
            auto error = Error{
                ErrorCode::DnsResolutionFailed,
                std::string("Failed to resolve ") + kHostname};
            if (retry_state->ShouldRetry(error)) {
                // Get delay before recording attempt (so first retry uses initial_delay)
                auto delay = retry_state->GetNextDelay(error);
                retry_state->RecordAttempt();
                // Schedule retry via event loop timer (non-blocking)
                // Capture shared_from_this to prevent use-after-free if client is destroyed
                auto self = this->shared_from_this();
                loop_.Schedule(delay, [self, req, callback, retry_state]() {
                    self->CallApiWithRetry(req, callback, retry_state);
                });
                return;
            }
            callback(std::unexpected(std::move(error)));
            return;
        }

        // Capture shared_from_this to prevent use-after-free
        auto self = this->shared_from_this();

        // Create builder and store in shared_ptr (outlives pipeline)
        auto builder = std::make_shared<SymbologyBuilder>();

        // Create sink that delivers to callback
        using Protocol = ApiProtocol<SymbologyBuilder>;
        using SinkType = typename Protocol::SinkType;
        using PipelineType = Pipeline<Protocol>;

        // Use a shared_ptr holder to keep the pipeline alive until completion.
        // This must be created before the sink so the sink lambda can capture it.
        auto pipeline_holder = std::make_shared<std::shared_ptr<PipelineType>>();

        auto sink = std::make_shared<SinkType>(
            // Capture pipeline_holder to extend pipeline lifetime until callback completes
            [self, req, callback, retry_state, builder, pipeline_holder](auto result) {
                if (!result && retry_state->ShouldRetry(result.error())) {
                    // Get delay before recording attempt
                    auto delay = retry_state->GetNextDelay(result.error());
                    retry_state->RecordAttempt();
                    // Schedule retry via event loop timer (non-blocking)
                    self->loop_.Schedule(delay, [self, req, callback, retry_state]() {
                        self->CallApiWithRetry(req, callback, retry_state);
                    });
                } else {
                    callback(std::move(result));
                }
                // Break the reference cycle (pipeline -> sink -> lambda -> pipeline_holder)
                // by deferring the reset to avoid destroying pipeline on its own call stack
                self->loop_.Defer([pipeline_holder]() {
                    pipeline_holder->reset();
                });
            });

        // Build chain
        auto chain = Protocol::BuildChain(loop_, *sink, api_key_);

        // Set builder on chain (must be done before Connect)
        chain->SetBuilder(*builder);

        // Set TLS SNI hostname (must be done before Connect)
        chain->SetHost(req.host);

        // Create pipeline
        *pipeline_holder = std::make_shared<PipelineType>(
            typename PipelineType::PrivateTag{},
            loop_, chain, sink, req);

        // Set up ready callback - when TLS handshake completes, start the request
        std::weak_ptr<PipelineType> weak_pipeline = *pipeline_holder;
        chain->SetReadyCallback([weak_pipeline]() {
            if (auto p = weak_pipeline.lock()) {
                p->MarkReady();
                p->Start();
            }
        });

        // Connect (triggers TLS handshake, then ready callback)
        (*pipeline_holder)->Connect(*addr);
    }

    IEventLoop& loop_;
    std::string api_key_;
    RetryConfig retry_config_;
};

}  // namespace dbn_pipe
