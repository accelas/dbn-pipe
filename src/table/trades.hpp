// SPDX-License-Identifier: MIT

#pragma once

#include "src/table/table.hpp"

namespace dbn_pipe {

inline constexpr auto trades_table = Table{"trades",
    Column<"ts_event_ns",   Int64>{},
    Column<"ts_event",      Timestamp>{},
    Column<"rtype",         Int16>{},
    Column<"publisher_id",  Int16>{},
    Column<"instrument_id", Int32>{},
    Column<"underlying_id", Int32>{},
    Column<"action",        Char>{},
    Column<"side",          Char>{},
    Column<"depth",         Int16>{},
    Column<"price",         Int64>{},
    Column<"size",          Int64>{},
    Column<"flags",         Int16>{},
    Column<"ts_recv_ns",    Int64>{},
    Column<"ts_recv",       Timestamp>{},
    Column<"ts_in_delta",   Int32>{},
    Column<"sequence",      Int64>{}
};

}  // namespace dbn_pipe
