// SPDX-License-Identifier: MIT

#pragma once

#include "dbn_pipe/table/table.hpp"

namespace dbn_pipe {

/// @name Equity OHLCV tables
/// Constexpr table definitions for equity OHLCV (Open/High/Low/Close/Volume) bars.
/// @{

/// Constexpr table definition for 1-second equity OHLCV bars.
inline constexpr auto ohlcv_1s_table = Table{"ohlcv_1s",
    Column<"ts_event_ns",   Int64>{},
    Column<"ts_event",      Timestamp>{},
    Column<"publisher_id",  Int16>{},
    Column<"instrument_id", Int32>{},
    Column<"open",          Int64>{},
    Column<"high",          Int64>{},
    Column<"low",           Int64>{},
    Column<"close",         Int64>{},
    Column<"volume",        Int64>{}
};

/// Constexpr table definition for 1-minute equity OHLCV bars.
inline constexpr auto ohlcv_1m_table = Table{"ohlcv_1m",
    Column<"ts_event_ns",   Int64>{},
    Column<"ts_event",      Timestamp>{},
    Column<"publisher_id",  Int16>{},
    Column<"instrument_id", Int32>{},
    Column<"open",          Int64>{},
    Column<"high",          Int64>{},
    Column<"low",           Int64>{},
    Column<"close",         Int64>{},
    Column<"volume",        Int64>{}
};

/// Constexpr table definition for 1-hour equity OHLCV bars.
inline constexpr auto ohlcv_1h_table = Table{"ohlcv_1h",
    Column<"ts_event_ns",   Int64>{},
    Column<"ts_event",      Timestamp>{},
    Column<"publisher_id",  Int16>{},
    Column<"instrument_id", Int32>{},
    Column<"open",          Int64>{},
    Column<"high",          Int64>{},
    Column<"low",           Int64>{},
    Column<"close",         Int64>{},
    Column<"volume",        Int64>{}
};

/// Constexpr table definition for 1-day equity OHLCV bars.
inline constexpr auto ohlcv_1d_table = Table{"ohlcv_1d",
    Column<"ts_event_ns",   Int64>{},
    Column<"ts_event",      Timestamp>{},
    Column<"publisher_id",  Int16>{},
    Column<"instrument_id", Int32>{},
    Column<"open",          Int64>{},
    Column<"high",          Int64>{},
    Column<"low",           Int64>{},
    Column<"close",         Int64>{},
    Column<"volume",        Int64>{}
};

/// @}

/// @name Options OHLCV tables
/// Constexpr table definitions for options OHLCV bars (includes underlying_id).
/// @{

/// Constexpr table definition for 1-second options OHLCV bars.
inline constexpr auto options_ohlcv_1s_table = Table{"options_ohlcv_1s",
    Column<"ts_event_ns",   Int64>{},
    Column<"ts_event",      Timestamp>{},
    Column<"publisher_id",  Int16>{},
    Column<"instrument_id", Int32>{},
    Column<"underlying_id", Int32>{},
    Column<"open",          Int64>{},
    Column<"high",          Int64>{},
    Column<"low",           Int64>{},
    Column<"close",         Int64>{},
    Column<"volume",        Int64>{}
};

/// Constexpr table definition for 1-minute options OHLCV bars.
inline constexpr auto options_ohlcv_1m_table = Table{"options_ohlcv_1m",
    Column<"ts_event_ns",   Int64>{},
    Column<"ts_event",      Timestamp>{},
    Column<"publisher_id",  Int16>{},
    Column<"instrument_id", Int32>{},
    Column<"underlying_id", Int32>{},
    Column<"open",          Int64>{},
    Column<"high",          Int64>{},
    Column<"low",           Int64>{},
    Column<"close",         Int64>{},
    Column<"volume",        Int64>{}
};

/// Constexpr table definition for 1-hour options OHLCV bars.
inline constexpr auto options_ohlcv_1h_table = Table{"options_ohlcv_1h",
    Column<"ts_event_ns",   Int64>{},
    Column<"ts_event",      Timestamp>{},
    Column<"publisher_id",  Int16>{},
    Column<"instrument_id", Int32>{},
    Column<"underlying_id", Int32>{},
    Column<"open",          Int64>{},
    Column<"high",          Int64>{},
    Column<"low",           Int64>{},
    Column<"close",         Int64>{},
    Column<"volume",        Int64>{}
};

/// Constexpr table definition for 1-day options OHLCV bars.
inline constexpr auto options_ohlcv_1d_table = Table{"options_ohlcv_1d",
    Column<"ts_event_ns",   Int64>{},
    Column<"ts_event",      Timestamp>{},
    Column<"publisher_id",  Int16>{},
    Column<"instrument_id", Int32>{},
    Column<"underlying_id", Int32>{},
    Column<"open",          Int64>{},
    Column<"high",          Int64>{},
    Column<"low",           Int64>{},
    Column<"close",         Int64>{},
    Column<"volume",        Int64>{}
};

/// @}

}  // namespace dbn_pipe
