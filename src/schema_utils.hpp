// SPDX-License-Identifier: MIT

// src/schema_utils.hpp
#pragma once

#include <cctype>
#include <optional>
#include <string>
#include <string_view>

#include <databento/enums.hpp>

namespace dbn_pipe {

// Schema enum matching Databento schemas
enum class Schema {
    Mbo,
    Mbp1,
    Mbp10,
    Trades,
    Tbbo,
    Ohlcv1S,
    Ohlcv1M,
    Ohlcv1H,
    Ohlcv1D,
    Definition,
    Statistics,
    Status,
    Imbalance,
    Cbbo,
    Cbbo1S,
    Cbbo1M,
    Tcbbo,
    Bbo1S,
    Bbo1M
};

// Schema from string
inline std::optional<Schema> SchemaFromString(std::string_view s) {
    if (s == "mbo") return Schema::Mbo;
    if (s == "mbp-1") return Schema::Mbp1;
    if (s == "mbp-10") return Schema::Mbp10;
    if (s == "trades") return Schema::Trades;
    if (s == "tbbo") return Schema::Tbbo;
    if (s == "ohlcv-1s") return Schema::Ohlcv1S;
    if (s == "ohlcv-1m") return Schema::Ohlcv1M;
    if (s == "ohlcv-1h") return Schema::Ohlcv1H;
    if (s == "ohlcv-1d") return Schema::Ohlcv1D;
    if (s == "definition") return Schema::Definition;
    if (s == "statistics") return Schema::Statistics;
    if (s == "status") return Schema::Status;
    if (s == "imbalance") return Schema::Imbalance;
    if (s == "cbbo") return Schema::Cbbo;
    if (s == "cbbo-1s") return Schema::Cbbo1S;
    if (s == "cbbo-1m") return Schema::Cbbo1M;
    if (s == "tcbbo") return Schema::Tcbbo;
    if (s == "bbo-1s") return Schema::Bbo1S;
    if (s == "bbo-1m") return Schema::Bbo1M;
    return std::nullopt;
}

// Schema to string
inline std::string_view SchemaToString(Schema schema) {
    switch (schema) {
        case Schema::Mbo: return "mbo";
        case Schema::Mbp1: return "mbp-1";
        case Schema::Mbp10: return "mbp-10";
        case Schema::Trades: return "trades";
        case Schema::Tbbo: return "tbbo";
        case Schema::Ohlcv1S: return "ohlcv-1s";
        case Schema::Ohlcv1M: return "ohlcv-1m";
        case Schema::Ohlcv1H: return "ohlcv-1h";
        case Schema::Ohlcv1D: return "ohlcv-1d";
        case Schema::Definition: return "definition";
        case Schema::Statistics: return "statistics";
        case Schema::Status: return "status";
        case Schema::Imbalance: return "imbalance";
        case Schema::Cbbo: return "cbbo";
        case Schema::Cbbo1S: return "cbbo-1s";
        case Schema::Cbbo1M: return "cbbo-1m";
        case Schema::Tcbbo: return "tcbbo";
        case Schema::Bbo1S: return "bbo-1s";
        case Schema::Bbo1M: return "bbo-1m";
    }
    return "";  // Unreachable
}

// Schema to RType mapping
inline std::optional<databento::RType> SchemaToRType(Schema schema) {
    switch (schema) {
        case Schema::Mbo: return databento::RType::Mbo;
        case Schema::Mbp1: return databento::RType::Mbp1;
        case Schema::Mbp10: return databento::RType::Mbp10;
        case Schema::Trades: return databento::RType::Mbp0;  // Trades use Mbp0
        case Schema::Tbbo: return databento::RType::Mbp1;
        case Schema::Ohlcv1S: return databento::RType::Ohlcv1S;
        case Schema::Ohlcv1M: return databento::RType::Ohlcv1M;
        case Schema::Ohlcv1H: return databento::RType::Ohlcv1H;
        case Schema::Ohlcv1D: return databento::RType::Ohlcv1D;
        case Schema::Definition: return databento::RType::InstrumentDef;
        case Schema::Statistics: return databento::RType::Statistics;
        case Schema::Status: return databento::RType::Status;
        case Schema::Imbalance: return databento::RType::Imbalance;
        case Schema::Cbbo: return databento::RType::Cmbp1;
        case Schema::Cbbo1S: return databento::RType::Cbbo1S;
        case Schema::Cbbo1M: return databento::RType::Cbbo1M;
        case Schema::Tcbbo: return databento::RType::Tcbbo;
        case Schema::Bbo1S: return databento::RType::Bbo1S;
        case Schema::Bbo1M: return databento::RType::Bbo1M;
    }
    return std::nullopt;
}

// Dataset to schema name (e.g., "OPRA.PILLAR" -> "opra_pillar")
inline std::string DatasetToSchemaName(const std::string& dataset) {
    std::string result;
    result.reserve(dataset.size());
    for (char c : dataset) {
        if (c == '.') {
            result += '_';
        } else {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return result;
}

}  // namespace dbn_pipe
