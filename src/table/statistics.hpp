// SPDX-License-Identifier: MIT

#pragma once

#include "dbn_pipe/table/table.hpp"

namespace dbn_pipe {

/// Constexpr table definition for equity StatMsg records.
inline constexpr auto statistics_table = Table{"statistics",
    Column<"ts_event_ns",   Int64>{},
    Column<"ts_event",      TimestampCol>{},
    Column<"rtype",         Int16>{},
    Column<"publisher_id",  Int16>{},
    Column<"instrument_id", Int32>{},
    Column<"stat_type",     Int16>{},
    Column<"price",         Int64>{},
    Column<"quantity",      Int64>{},
    Column<"ts_ref_ns",     Int64>{},
    Column<"ts_recv_ns",    Int64>{},
    Column<"ts_recv",       TimestampCol>{},
    Column<"sequence",      Int32>{},
    Column<"update_action", Int16>{},
    Column<"stat_flags",    Int16>{}
};

/// Constexpr table definition for options StatMsg records (includes underlying_id).
inline constexpr auto options_statistics_table = Table{"statistics",
    Column<"ts_event_ns",   Int64>{},
    Column<"ts_event",      TimestampCol>{},
    Column<"rtype",         Int16>{},
    Column<"publisher_id",  Int16>{},
    Column<"instrument_id", Int32>{},
    Column<"underlying_id", Int32>{},
    Column<"stat_type",     Int16>{},
    Column<"price",         Int64>{},
    Column<"quantity",      Int64>{},
    Column<"ts_ref_ns",     Int64>{},
    Column<"ts_recv_ns",    Int64>{},
    Column<"ts_recv",       TimestampCol>{},
    Column<"sequence",      Int32>{},
    Column<"update_action", Int16>{},
    Column<"stat_flags",    Int16>{}
};

}  // namespace dbn_pipe
