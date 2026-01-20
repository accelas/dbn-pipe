// src/api/symbology_client.hpp
#pragma once

#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "src/api/api_pipeline.hpp"
#include "src/dns_resolver.hpp"
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

    void OnInt(int64_t) {}
    void OnUint(uint64_t) {}
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
                // Save the interval and return to array state
                response_.result[current_symbol_].push_back(std::move(current_interval_));
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
// Thread safety: Not thread-safe. All methods must be called from the event loop thread.
class SymbologyClient {
public:
    SymbologyClient(IEventLoop& loop, std::string api_key)
        : loop_(loop), api_key_(std::move(api_key)) {}

    void Resolve(
        const std::string& dataset,
        const std::vector<std::string>& symbols,
        SType stype_in,
        SType stype_out,
        const std::string& start_date,
        const std::string& end_date,
        std::function<void(std::expected<SymbologyResponse, Error>)> callback) {
        // Build comma-separated symbols string
        std::ostringstream symbols_ss;
        bool first = true;
        for (const auto& sym : symbols) {
            if (!first) symbols_ss << ",";
            first = false;
            symbols_ss << sym;
        }

        ApiRequest req{
            .method = "POST",
            .path = "/v0/symbology.resolve",
            .query_params = {},
            .form_params = {
                {"dataset", dataset},
                {"symbols", symbols_ss.str()},
                {"stype_in", std::string(STypeToString(stype_in))},
                {"stype_out", std::string(STypeToString(stype_out))},
                {"start_date", start_date},
                {"end_date", end_date},
            },
        };

        auto addr = ResolveHostname(kHostname, kPort);
        if (!addr) {
            callback(std::unexpected(Error{
                ErrorCode::DnsResolutionFailed,
                std::string("Failed to resolve ") + kHostname}));
            return;
        }

        auto builder = std::make_shared<SymbologyBuilder>();
        auto pipeline = ApiPipeline<SymbologyBuilder>::Create(
            loop_,
            kHostname,
            *builder,
            [callback, builder](auto result) {
                callback(std::move(result));
            });

        std::string http_request = req.BuildHttpRequest(kHostname, api_key_);

        pipeline->SetReadyCallback([pipeline, http_request]() {
            pipeline->SendRequest(http_request);
        });

        pipeline->Connect(*addr);
    }

private:
    static constexpr const char* kHostname = "hist.databento.com";
    static constexpr uint16_t kPort = 443;

    IEventLoop& loop_;
    std::string api_key_;
};

}  // namespace dbn_pipe
