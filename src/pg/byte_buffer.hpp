// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace dbn_pipe::pg {

// Byte buffer for binary COPY data
class ByteBuffer {
public:
    void put_int16_be(int16_t val);
    void put_int32_be(int32_t val);
    void put_int64_be(int64_t val);
    void put_byte(std::byte b);
    void put_bytes(std::span<const std::byte> data);

    std::span<const std::byte> view() const { return data_; }
    void clear() { data_.clear(); }
    size_t size() const { return data_.size(); }

private:
    std::vector<std::byte> data_;
};

}  // namespace dbn_pipe::pg
