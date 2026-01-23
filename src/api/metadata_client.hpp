// SPDX-License-Identifier: MIT

// src/api/metadata_client.hpp
#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "lib/stream/pipeline.hpp"
#include "src/api_protocol.hpp"
#include "src/dns_resolver.hpp"
#include "src/retry_policy.hpp"

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
// Automatic retry with exponential backoff on transient errors
// (ConnectionFailed, ServerError, TlsHandshakeFailed, RateLimited).
//
// Thread safety: Not thread-safe. All methods must be called from the event loop thread.
//
// Lifetime: Must be managed via shared_ptr (use Create() factory method).
// The client must outlive any in-flight requests.
class MetadataClient : public std::enable_shared_from_this<MetadataClient> {
public:
    static std::shared_ptr<MetadataClient> Create(
        IEventLoop& loop, std::string api_key,
        RetryConfig retry_config = RetryConfig::ApiDefaults()) {
        return std::shared_ptr<MetadataClient>(
            new MetadataClient(loop, std::move(api_key), retry_config));
    }

private:
    MetadataClient(IEventLoop& loop, std::string api_key, RetryConfig retry_config)
        : loop_(loop), api_key_(std::move(api_key)), retry_config_(retry_config) {}

public:

    void GetRecordCount(
        const std::string& dataset,
        const std::string& symbols,
        const std::string& schema,
        const std::string& start,
        const std::string& end,
        const std::string& stype_in,
        std::function<void(std::expected<uint64_t, Error>)> callback) {
        ApiRequest req{
            .method = "POST",
            .path = "/v0/metadata.get_record_count",
            .host = kHostname,
            .port = kPort,
            .query_params = {},
            .form_params = {
                {"dataset", dataset},
                {"symbols", symbols},
                {"schema", schema},
                {"start", start},
                {"end", end},
                {"stype_in", stype_in},
            },
        };
        CallApi<RecordCountBuilder>(req, std::move(callback));
    }

    void GetBillableSize(
        const std::string& dataset,
        const std::string& symbols,
        const std::string& schema,
        const std::string& start,
        const std::string& end,
        const std::string& stype_in,
        std::function<void(std::expected<uint64_t, Error>)> callback) {
        ApiRequest req{
            .method = "POST",
            .path = "/v0/metadata.get_billable_size",
            .host = kHostname,
            .port = kPort,
            .query_params = {},
            .form_params = {
                {"dataset", dataset},
                {"symbols", symbols},
                {"schema", schema},
                {"start", start},
                {"end", end},
                {"stype_in", stype_in},
            },
        };
        CallApi<BillableSizeBuilder>(req, std::move(callback));
    }

    void GetCost(
        const std::string& dataset,
        const std::string& symbols,
        const std::string& schema,
        const std::string& start,
        const std::string& end,
        const std::string& stype_in,
        std::function<void(std::expected<double, Error>)> callback) {
        ApiRequest req{
            .method = "POST",
            .path = "/v0/metadata.get_cost",
            .host = kHostname,
            .port = kPort,
            .query_params = {},
            .form_params = {
                {"dataset", dataset},
                {"symbols", symbols},
                {"schema", schema},
                {"start", start},
                {"end", end},
                {"stype_in", stype_in},
            },
        };
        CallApi<CostBuilder>(req, std::move(callback));
    }

    void GetDatasetRange(
        const std::string& dataset,
        std::function<void(std::expected<DatasetRange, Error>)> callback) {
        ApiRequest req{
            .method = "GET",
            .path = "/v0/metadata.get_dataset_range",
            .host = kHostname,
            .port = kPort,
            .query_params = {{"dataset", dataset}},
            .form_params = {},
        };
        CallApi<DatasetRangeBuilder>(req, std::move(callback));
    }

private:
    static constexpr const char* kHostname = "hist.databento.com";
    static constexpr uint16_t kPort = 443;

    template <typename Builder>
    void CallApi(
        const ApiRequest& req,
        std::function<void(std::expected<typename Builder::Result, Error>)> callback) {
        // Create retry state that persists across attempts
        auto retry_state = std::make_shared<RetryPolicy>(retry_config_);
        CallApiWithRetry<Builder>(req, std::move(callback), retry_state);
    }

    template <typename Builder>
    void CallApiWithRetry(
        const ApiRequest& req,
        std::function<void(std::expected<typename Builder::Result, Error>)> callback,
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
                    self->CallApiWithRetry<Builder>(req, callback, retry_state);
                });
                return;
            }
            callback(std::unexpected(std::move(error)));
            return;
        }

        // Capture shared_from_this to prevent use-after-free
        auto self = this->shared_from_this();

        // Create builder and store in shared_ptr (outlives pipeline)
        auto builder = std::make_shared<Builder>();

        // Create sink that delivers to callback
        using Protocol = ApiProtocol<Builder>;
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
                        self->CallApiWithRetry<Builder>(req, callback, retry_state);
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
