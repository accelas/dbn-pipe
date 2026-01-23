// SPDX-License-Identifier: MIT

#include "dbwriter/types.hpp"
#include <bit>

namespace dbwriter {

void ByteBuffer::put_int16_be(int16_t val) {
    if constexpr (std::endian::native == std::endian::little) {
        val = __builtin_bswap16(val);
    }
    auto* bytes = reinterpret_cast<const std::byte*>(&val);
    data_.insert(data_.end(), bytes, bytes + 2);
}

void ByteBuffer::put_int32_be(int32_t val) {
    if constexpr (std::endian::native == std::endian::little) {
        val = __builtin_bswap32(val);
    }
    auto* bytes = reinterpret_cast<const std::byte*>(&val);
    data_.insert(data_.end(), bytes, bytes + 4);
}

void ByteBuffer::put_int64_be(int64_t val) {
    if constexpr (std::endian::native == std::endian::little) {
        val = __builtin_bswap64(val);
    }
    auto* bytes = reinterpret_cast<const std::byte*>(&val);
    data_.insert(data_.end(), bytes, bytes + 8);
}

void ByteBuffer::put_byte(std::byte b) {
    data_.push_back(b);
}

void ByteBuffer::put_bytes(std::span<const std::byte> data) {
    data_.insert(data_.end(), data.begin(), data.end());
}

}  // namespace dbwriter
