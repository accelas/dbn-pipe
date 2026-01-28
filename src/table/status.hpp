// SPDX-License-Identifier: MIT

#pragma once

#include "src/table/table.hpp"

namespace dbn_pipe {

// Status table for trading halt/resume events - 13 columns
inline constexpr auto status_table = Table{"status",
    Column<"ts_event_ns",   Int64>{},
    Column<"ts_event",      Timestamp>{},
    Column<"rtype",         Int16>{},
    Column<"publisher_id",  Int16>{},
    Column<"instrument_id", Int32>{},
    Column<"action",        Int16>{},
    Column<"reason",        Int16>{},
    Column<"trading_event", Int16>{},
    Column<"is_trading",    Char>{},
    Column<"is_quoting",    Char>{},
    Column<"is_short_sell_restricted", Char>{},
    Column<"ts_recv_ns",    Int64>{},
    Column<"ts_recv",       Timestamp>{}
};

// Options status table - 14 columns (adds underlying_id)
inline constexpr auto options_status_table = Table{"status",
    Column<"ts_event_ns",   Int64>{},
    Column<"ts_event",      Timestamp>{},
    Column<"rtype",         Int16>{},
    Column<"publisher_id",  Int16>{},
    Column<"instrument_id", Int32>{},
    Column<"underlying_id", Int32>{},
    Column<"action",        Int16>{},
    Column<"reason",        Int16>{},
    Column<"trading_event", Int16>{},
    Column<"is_trading",    Char>{},
    Column<"is_quoting",    Char>{},
    Column<"is_short_sell_restricted", Char>{},
    Column<"ts_recv_ns",    Int64>{},
    Column<"ts_recv",       Timestamp>{}
};

}  // namespace dbn_pipe
