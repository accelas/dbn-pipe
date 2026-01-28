// SPDX-License-Identifier: MIT

#pragma once

#include "src/table/table.hpp"

namespace dbn_pipe {

// Equity definitions table for InstrumentDefMsg records - 43 columns
inline constexpr auto definitions_table = Table{"definitions",
    // Record header
    Column<"ts_event_ns",               Int64>{},
    Column<"ts_event",                  Timestamp>{},
    Column<"rtype",                     Int16>{},
    Column<"publisher_id",              Int16>{},
    Column<"instrument_id",             Int32>{},
    // Identification
    Column<"raw_symbol",                Text>{},
    Column<"raw_instrument_id",         Int64>{},
    Column<"security_type",             Text>{},
    Column<"exchange",                  Text>{},
    // Pricing
    Column<"currency",                  Text>{},
    Column<"min_price_increment",       Int64>{},
    Column<"min_price_increment_amount",Int64>{},
    Column<"display_factor",            Int64>{},
    Column<"price_ratio",               Int64>{},
    Column<"price_display_format",      Int16>{},
    Column<"main_fraction",             Int16>{},
    Column<"sub_fraction",              Int16>{},
    Column<"tick_rule",                 Int16>{},
    // Trading limits
    Column<"high_limit_price",          Int64>{},
    Column<"low_limit_price",           Int64>{},
    Column<"max_price_variation",       Int64>{},
    // Lot sizes
    Column<"min_lot_size",              Int32>{},
    Column<"min_lot_size_block",        Int32>{},
    Column<"min_lot_size_round_lot",    Int32>{},
    Column<"min_trade_vol",             Int64>{},
    Column<"max_trade_vol",             Int64>{},
    // Market structure
    Column<"market_depth",              Int32>{},
    Column<"market_depth_implied",      Int32>{},
    Column<"market_segment_id",         Int64>{},
    Column<"appl_id",                   Int16>{},
    Column<"channel_id",               Int16>{},
    // Classification
    Column<"inst_attrib_value",         Int32>{},
    Column<"secsubtype",                Text>{},
    Column<"group_code",                Text>{},
    Column<"asset",                     Text>{},
    Column<"cfi",                       Text>{},
    Column<"unit_of_measure",           Text>{},
    Column<"unit_of_measure_qty",       Int64>{},
    // Status
    Column<"match_algorithm",           Char>{},
    Column<"security_update_action",    Char>{},
    Column<"user_defined_instrument",   Char>{},
    // Gateway timestamp
    Column<"ts_recv_ns",                Int64>{},
    Column<"ts_recv",                   Timestamp>{}
};

// Options definitions table - 53 columns
inline constexpr auto options_definitions_table = Table{"options_definitions",
    // Record header
    Column<"ts_event_ns",               Int64>{},
    Column<"ts_event",                  Timestamp>{},
    Column<"rtype",                     Int16>{},
    Column<"publisher_id",              Int16>{},
    Column<"instrument_id",             Int32>{},
    // Identification
    Column<"raw_symbol",                Text>{},
    Column<"raw_instrument_id",         Int64>{},
    Column<"security_type",             Text>{},
    Column<"exchange",                  Text>{},
    // Underlying link
    Column<"underlying_id",             Int32>{},
    Column<"underlying",                Text>{},
    // Option contract specs
    Column<"strike_price",              Int64>{},
    Column<"instrument_class",          Char>{},
    // Expiration
    Column<"expiration_ns",             Int64>{},
    Column<"expiration",                Timestamp>{},
    Column<"maturity_year",             Int16>{},
    Column<"maturity_month",            Int16>{},
    Column<"maturity_day",              Int16>{},
    // Pricing
    Column<"currency",                  Text>{},
    Column<"min_price_increment",       Int64>{},
    Column<"min_price_increment_amount",Int64>{},
    Column<"display_factor",            Int64>{},
    Column<"price_ratio",               Int64>{},
    Column<"price_display_format",      Int16>{},
    Column<"main_fraction",             Int16>{},
    Column<"sub_fraction",              Int16>{},
    Column<"tick_rule",                 Int16>{},
    Column<"contract_multiplier",       Int32>{},
    // Trading limits
    Column<"high_limit_price",          Int64>{},
    Column<"low_limit_price",           Int64>{},
    Column<"max_price_variation",       Int64>{},
    // Lot sizes
    Column<"min_lot_size",              Int32>{},
    Column<"min_lot_size_block",        Int32>{},
    Column<"min_lot_size_round_lot",    Int32>{},
    Column<"min_trade_vol",             Int64>{},
    Column<"max_trade_vol",             Int64>{},
    // Market structure
    Column<"market_depth",              Int32>{},
    Column<"market_depth_implied",      Int32>{},
    Column<"market_segment_id",         Int64>{},
    Column<"appl_id",                   Int16>{},
    Column<"channel_id",               Int16>{},
    // Classification
    Column<"inst_attrib_value",         Int32>{},
    Column<"secsubtype",                Text>{},
    Column<"group_code",                Text>{},
    Column<"asset",                     Text>{},
    Column<"cfi",                       Text>{},
    Column<"unit_of_measure",           Text>{},
    Column<"unit_of_measure_qty",       Int64>{},
    // Status
    Column<"match_algorithm",           Char>{},
    Column<"security_update_action",    Char>{},
    Column<"user_defined_instrument",   Char>{},
    // Gateway timestamp
    Column<"ts_recv_ns",                Int64>{},
    Column<"ts_recv",                   Timestamp>{}
};

}  // namespace dbn_pipe
