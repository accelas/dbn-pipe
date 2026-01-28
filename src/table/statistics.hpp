// SPDX-License-Identifier: MIT

#pragma once

#include "src/table/table.hpp"

namespace dbn_pipe {

// Statistics table for equity statistics - 14 columns
inline constexpr auto statistics_table = Table{"statistics",
    Column<"ts_event_ns",   Int64>{},
    Column<"ts_event",      Timestamp>{},
    Column<"rtype",         Int16>{},
    Column<"publisher_id",  Int16>{},
    Column<"instrument_id", Int32>{},
    Column<"stat_type",     Int16>{},
    Column<"price",         Int64>{},
    Column<"quantity",      Int64>{},
    Column<"ts_ref_ns",     Int64>{},
    Column<"ts_recv_ns",    Int64>{},
    Column<"ts_recv",       Timestamp>{},
    Column<"sequence",      Int32>{},
    Column<"update_action", Int16>{},
    Column<"stat_flags",    Int16>{}
};

// Options statistics table - 15 columns (adds underlying_id)
inline constexpr auto options_statistics_table = Table{"statistics",
    Column<"ts_event_ns",   Int64>{},
    Column<"ts_event",      Timestamp>{},
    Column<"rtype",         Int16>{},
    Column<"publisher_id",  Int16>{},
    Column<"instrument_id", Int32>{},
    Column<"underlying_id", Int32>{},
    Column<"stat_type",     Int16>{},
    Column<"price",         Int64>{},
    Column<"quantity",      Int64>{},
    Column<"ts_ref_ns",     Int64>{},
    Column<"ts_recv_ns",    Int64>{},
    Column<"ts_recv",       Timestamp>{},
    Column<"sequence",      Int32>{},
    Column<"update_action", Int16>{},
    Column<"stat_flags",    Int16>{}
};

}  // namespace dbn_pipe
