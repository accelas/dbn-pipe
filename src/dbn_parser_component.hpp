// src/dbn_parser_component.hpp
#pragma once

#include <cstddef>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <vector>

#include <databento/record.hpp>

#include "error.hpp"
#include "pipeline.hpp"
#include "reactor.hpp"
#include "tls_socket.hpp"  // For Suspendable

namespace databento_async {

// DbnParserComponent - Pipeline component that parses DBN bytes into records.
// Receives raw bytes from upstream (via Read()) and emits parsed records
// to RecordDownstream (via OnRecord()).
//
// Uses CRTP with PipelineComponent base for reentrancy-safe lifecycle.
// Implements Suspendable for backpressure control.
//
// Template parameter D must satisfy RecordDownstream concept.
// NOTE: Records are only valid for the duration of the OnRecord() call.
// Implementations must copy any data they need to retain.
template <RecordDownstream D>
class DbnParserComponent
    : public PipelineComponent<DbnParserComponent<D>>,
      public Suspendable,
      public std::enable_shared_from_this<DbnParserComponent<D>> {

    using Base = PipelineComponent<DbnParserComponent<D>>;
    friend Base;

    // Enable shared_from_this in constructor via MakeSharedEnabler pattern
    struct MakeSharedEnabler;

public:
    // Factory method for shared_from_this safety
    static std::shared_ptr<DbnParserComponent> Create(
        Reactor& reactor,
        std::shared_ptr<D> downstream
    ) {
        return std::make_shared<MakeSharedEnabler>(reactor, std::move(downstream));
    }

    ~DbnParserComponent() = default;

    // Downstream interface (bytes in from upstream)
    void Read(std::pmr::vector<std::byte> data);
    void OnError(const Error& e);
    void OnDone();

    // Suspendable interface
    void Suspend() override;
    void Resume() override;
    void Close() { this->RequestClose(); }

    // Set upstream for backpressure propagation
    void SetUpstream(Suspendable* up) { upstream_ = up; }

    // PipelineComponent requirements
    void DisableWatchers() {}
    void DoClose();

private:
    DbnParserComponent(Reactor& reactor, std::shared_ptr<D> downstream);

    // Parse and emit complete records from buffer
    void DrainBuffer();

    // Check if buffer contains a complete record
    // Non-const because it may emit ParseError on invalid record size
    bool HasCompleteRecord();

    // Peek at record size from header (0 if header not available)
    std::size_t PeekRecordSize() const;

    std::shared_ptr<D> downstream_;
    Suspendable* upstream_ = nullptr;

    std::vector<std::byte> buffer_;
    std::size_t read_pos_ = 0;
    std::size_t write_pos_ = 0;

    static constexpr std::size_t kInitialBufferSize = 64 * 1024;       // 64KB
    static constexpr std::size_t kMaxBufferSize = 16 * 1024 * 1024;    // 16MB cap
};

// MakeSharedEnabler - allows make_shared with private constructor
template <RecordDownstream D>
struct DbnParserComponent<D>::MakeSharedEnabler : public DbnParserComponent<D> {
    MakeSharedEnabler(Reactor& reactor, std::shared_ptr<D> downstream)
        : DbnParserComponent<D>(reactor, std::move(downstream)) {}
};

// Implementation

template <RecordDownstream D>
DbnParserComponent<D>::DbnParserComponent(Reactor& reactor, std::shared_ptr<D> downstream)
    : Base(reactor)
    , downstream_(std::move(downstream))
    , buffer_(kInitialBufferSize)
{}

template <RecordDownstream D>
void DbnParserComponent<D>::DoClose() {
    downstream_.reset();
    // Note: upstream_ is Suspendable which doesn't have Close().
    // Upstream components manage their own lifecycle.
}

template <RecordDownstream D>
void DbnParserComponent<D>::Read(std::pmr::vector<std::byte> data) {
    auto guard = this->TryGuard();
    if (!guard) return;

    // Check buffer overflow
    std::size_t unread = write_pos_ - read_pos_;
    if (unread + data.size() > kMaxBufferSize) {
        this->EmitError(*downstream_,
            Error{ErrorCode::BufferOverflow, "Parser buffer overflow"});
        this->RequestClose();
        return;
    }

    // Compact buffer if needed to make room
    if (write_pos_ + data.size() > buffer_.size()) {
        if (read_pos_ > 0) {
            if (unread > 0) {
                std::memmove(buffer_.data(), buffer_.data() + read_pos_, unread);
            }
            read_pos_ = 0;
            write_pos_ = unread;
        }
        // Grow buffer if still not enough room
        if (write_pos_ + data.size() > buffer_.size()) {
            buffer_.resize(write_pos_ + data.size());
        }
    }

    // Append incoming data
    std::memcpy(buffer_.data() + write_pos_, data.data(), data.size());
    write_pos_ += data.size();

    // Parse and emit records if not suspended
    if (!suspended_) {
        DrainBuffer();
    }
}

template <RecordDownstream D>
void DbnParserComponent<D>::DrainBuffer() {
    while (!suspended_ && HasCompleteRecord()) {
        std::size_t record_size = PeekRecordSize();

        // Create aligned storage for the record
        alignas(databento::RecordHeader) std::vector<std::byte> aligned_record(record_size);
        std::memcpy(aligned_record.data(), buffer_.data() + read_pos_, record_size);

        auto* hdr = reinterpret_cast<databento::RecordHeader*>(aligned_record.data());
        databento::Record rec{hdr};
        read_pos_ += record_size;

        downstream_->OnRecord(rec);
    }
}

template <RecordDownstream D>
bool DbnParserComponent<D>::HasCompleteRecord() {
    std::size_t available = write_pos_ - read_pos_;
    if (available < sizeof(databento::RecordHeader)) {
        return false;
    }

    std::size_t size = PeekRecordSize();

    // Validate record size - must be non-zero and at least header size
    if (size == 0 || size < sizeof(databento::RecordHeader)) {
        this->EmitError(*downstream_,
            Error{ErrorCode::ParseError, "Invalid record size in DBN stream"});
        this->RequestClose();
        return false;
    }

    return available >= size;
}

template <RecordDownstream D>
std::size_t DbnParserComponent<D>::PeekRecordSize() const {
    if (write_pos_ - read_pos_ < sizeof(databento::RecordHeader)) {
        return 0;
    }
    // Use memcpy to avoid undefined behavior from unaligned access
    databento::RecordHeader hdr;
    std::memcpy(&hdr, buffer_.data() + read_pos_, sizeof(hdr));
    return hdr.Size();
}

template <RecordDownstream D>
void DbnParserComponent<D>::OnError(const Error& e) {
    auto guard = this->TryGuard();
    if (!guard) return;

    this->EmitError(*downstream_, e);
    this->RequestClose();
}

template <RecordDownstream D>
void DbnParserComponent<D>::OnDone() {
    auto guard = this->TryGuard();
    if (!guard) return;

    // Check for incomplete record at end of stream
    if (write_pos_ > read_pos_) {
        this->EmitError(*downstream_,
            Error{ErrorCode::ParseError, "Incomplete record at end of stream"});
    } else {
        this->EmitDone(*downstream_);
    }
    this->RequestClose();
}

template <RecordDownstream D>
void DbnParserComponent<D>::Suspend() {
    auto guard = this->TryGuard();
    if (!guard) return;

    suspended_ = true;
    if (upstream_) upstream_->Suspend();
}

template <RecordDownstream D>
void DbnParserComponent<D>::Resume() {
    auto guard = this->TryGuard();
    if (!guard) return;

    suspended_ = false;
    DrainBuffer();

    // Propagate resume upstream if not re-suspended during DrainBuffer
    if (!suspended_ && upstream_) {
        upstream_->Resume();
    }
}

}  // namespace databento_async
