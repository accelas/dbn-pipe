// SPDX-License-Identifier: MIT

#pragma once

#include "src/table/table.hpp"

namespace dbn_pipe {

// OHLCV (Open/High/Low/Close/Volume) tables for equity - 9 columns each

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

// OHLCV tables for options - 10 columns each (adds underlying_id)

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

}  // namespace dbn_pipe
