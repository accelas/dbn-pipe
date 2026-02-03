// SPDX-License-Identifier: MIT

#pragma once

#include "dbn_pipe/table/table.hpp"

namespace dbn_pipe {

// MBP1 (Market By Price level 1) for equity quotes - 21 columns
inline constexpr auto mbp1_table = Table{"mbp1",
    Column<"ts_event_ns",   Int64>{},
    Column<"ts_event",      Timestamp>{},
    Column<"rtype",         Int16>{},
    Column<"publisher_id",  Int16>{},
    Column<"instrument_id", Int32>{},
    Column<"action",        Char>{},
    Column<"side",          Char>{},
    Column<"depth",         Int16>{},
    Column<"price",         Int64>{},
    Column<"size",          Int64>{},
    Column<"flags",         Int16>{},
    Column<"ts_recv_ns",    Int64>{},
    Column<"ts_recv",       Timestamp>{},
    Column<"ts_in_delta",   Int32>{},
    Column<"sequence",      Int64>{},
    Column<"bid_px",        Int64>{},
    Column<"ask_px",        Int64>{},
    Column<"bid_sz",        Int64>{},
    Column<"ask_sz",        Int64>{},
    Column<"bid_ct",        Int64>{},
    Column<"ask_ct",        Int64>{}
};

// CMBP1 (Consolidated MBP1) for options quotes - 20 columns
inline constexpr auto cmbp1_table = Table{"cmbp1",
    Column<"ts_event_ns",   Int64>{},
    Column<"ts_event",      Timestamp>{},
    Column<"rtype",         Int16>{},
    Column<"publisher_id",  Int16>{},
    Column<"instrument_id", Int32>{},
    Column<"underlying_id", Int32>{},
    Column<"action",        Char>{},
    Column<"side",          Char>{},
    Column<"price",         Int64>{},
    Column<"size",          Int64>{},
    Column<"flags",         Int16>{},
    Column<"ts_recv_ns",    Int64>{},
    Column<"ts_recv",       Timestamp>{},
    Column<"ts_in_delta",   Int32>{},
    Column<"bid_px",        Int64>{},
    Column<"ask_px",        Int64>{},
    Column<"bid_sz",        Int64>{},
    Column<"ask_sz",        Int64>{},
    Column<"bid_pb",        Int16>{},
    Column<"ask_pb",        Int16>{}
};

}  // namespace dbn_pipe
