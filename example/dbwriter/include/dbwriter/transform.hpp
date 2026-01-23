// Copyright 2026 Kai Wang
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>

namespace dbwriter {

// Instrument map interface for underlying lookups
class IInstrumentMap {
public:
    virtual ~IInstrumentMap() = default;
    virtual std::optional<uint32_t> underlying(uint32_t instrument_id) const = 0;
};

// Transform template - specialize for each Record → Table mapping
template <typename Record, typename Table>
struct Transform {
    // Specializations must provide:
    // - const IInstrumentMap& instruments (or other dependencies)
    // - RowType operator()(const Record&) const
};

}  // namespace dbwriter
