#pragma once

#include <cstddef>
#include <cstring>
#include <vector>

#include <databento/constants.hpp>
#include <databento/record.hpp>

namespace databento_async {

// Streaming DBN parser
// Push bytes in, pull records out
// Uses databento-cpp types directly
class DbnParser {
public:
    explicit DbnParser(std::size_t initial_capacity = 64 * 1024)
        : buffer_(initial_capacity) {}

    // Push bytes from network into the parser
    void Push(const std::byte* data, std::size_t len) {
        // Shift buffer if needed to make room
        if (write_pos_ + len > buffer_.size()) {
            Compact();
            if (write_pos_ + len > buffer_.size()) {
                buffer_.resize(write_pos_ + len);
            }
        }
        std::memcpy(buffer_.data() + write_pos_, data, len);
        write_pos_ += len;
    }

    // Try to pull next complete record
    // Returns nullptr if not enough data available
    const databento::Record* Pull() {
        std::size_t available = write_pos_ - read_pos_;

        // Need at least a header
        if (available < sizeof(databento::RecordHeader)) {
            return nullptr;
        }

        auto* hdr = reinterpret_cast<databento::RecordHeader*>(
            buffer_.data() + read_pos_);
        std::size_t record_size = hdr->Size();

        // Need full record
        if (available < record_size) {
            return nullptr;
        }

        current_ = databento::Record{hdr};
        read_pos_ += record_size;
        return &current_;
    }

    // Peek at next record size without consuming
    // Returns 0 if header not available
    std::size_t PeekSize() const {
        if (write_pos_ - read_pos_ < sizeof(databento::RecordHeader)) {
            return 0;
        }
        auto* hdr = reinterpret_cast<const databento::RecordHeader*>(
            buffer_.data() + read_pos_);
        return hdr->Size();
    }

    // Check if a complete record is available
    bool HasRecord() const {
        std::size_t size = PeekSize();
        return size > 0 && (write_pos_ - read_pos_) >= size;
    }

    // Bytes available to read
    std::size_t Available() const { return write_pos_ - read_pos_; }

    // Reset parser state
    void Reset() {
        read_pos_ = 0;
        write_pos_ = 0;
    }

    // Move unread data to start of buffer
    void Compact() {
        if (read_pos_ == 0) return;

        std::size_t unread = write_pos_ - read_pos_;
        if (unread > 0) {
            std::memmove(buffer_.data(), buffer_.data() + read_pos_, unread);
        }
        read_pos_ = 0;
        write_pos_ = unread;
    }

private:
    std::vector<std::byte> buffer_;
    std::size_t read_pos_ = 0;
    std::size_t write_pos_ = 0;
    databento::Record current_{nullptr};
};

}  // namespace databento_async
