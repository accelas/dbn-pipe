// SPDX-License-Identifier: MIT

// lib/stream/tls_transport.hpp
#pragma once

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cerrno>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dbn_pipe/stream/buffer_chain.hpp"
#include "dbn_pipe/stream/component.hpp"
#include "dbn_pipe/stream/error.hpp"

namespace dbn_pipe {

// TLS handshake state machine
enum class TlsHandshakeState { NotStarted, InProgress, Complete };

// TlsTransport handles TLS encryption/decryption using OpenSSL with memory BIOs.
// Sits between TcpSocket (upstream) and HttpClient (downstream) in the pipeline.
// PipelineComponent provides Suspendable interface with suspend count semantics.
//
// Template parameter D must satisfy the Downstream concept.
template <Downstream D>
class TlsTransport : public PipelineComponent<TlsTransport<D>, D>,
                  public std::enable_shared_from_this<TlsTransport<D>> {
public:
    using UpstreamWriteCallback = std::function<void(BufferChain)>;

    // Factory method for shared_from_this safety
    static std::shared_ptr<TlsTransport> Create(std::shared_ptr<D> downstream) {
        // Use a helper struct to access private constructor
        struct MakeSharedEnabler : public TlsTransport {
            MakeSharedEnabler(std::shared_ptr<D> ds)
                : TlsTransport(std::move(ds)) {}
        };
        return std::make_shared<MakeSharedEnabler>(std::move(downstream));
    }

    ~TlsTransport() { Cleanup(); }

    // Downstream interface: receive encrypted data from TcpSocket
    // Satisfies Downstream concept requirement
    void OnData(BufferChain& chain) {
        // Move data out of chain (TcpSocket passes by ref, we consume all)
        BufferChain data = std::move(chain);
        chain.Clear();
        Read(std::move(data));
    }

    void OnError(const Error& e) {
        // Propagate error to our downstream
        this->PropagateError(e);
    }

    void OnDone() {
        // Process any remaining buffered data, then signal done
        auto guard = this->TryGuard();
        if (!guard) return;

        // Flush any remaining decrypted data
        if (this->IsSuspended()) {
            this->DeferOnDone();
            return;
        }

        // Complete with flush of pending data
        if (this->CompleteWithFlush(pending_read_chain_)) {
            this->RequestClose();
        }
    }

    // Internal: process encrypted data (called by OnData)
    void Read(BufferChain data);

    // Upstream interface: write plaintext (from downstream) to be encrypted
    void Write(BufferChain data);

    // TLS-specific methods
    void StartHandshake();
    bool IsHandshakeComplete() const { return handshake_state_ == TlsHandshakeState::Complete; }

    // Set hostname for SNI (Server Name Indication)
    void SetHostname(const std::string& hostname);

    // Set callback to send encrypted data upstream (to TcpSocket)
    void SetUpstreamWriteCallback(UpstreamWriteCallback cb) {
        upstream_write_ = std::move(cb);
    }

    // Set callback for when handshake completes
    void SetHandshakeCompleteCallback(std::function<void()> cb) {
        handshake_complete_cb_ = std::move(cb);
    }

    // Required by PipelineComponent
    void DisableWatchers() {
        // No direct epoll watchers; TLS operates on memory BIOs
    }

    void DoClose();

    void ProcessPending() {
        // Forward any buffered decrypted data first
        if (this->ForwardData(pending_read_chain_)) return;
        // Process more data from SSL
        ProcessPendingReads();
    }

    void FlushAndComplete() {
        // Flush pending data and complete - only close if completion happened
        if (this->CompleteWithFlush(pending_read_chain_)) {
            this->RequestClose();
        }
    }

private:
    // Private constructor - use Create() factory method
    TlsTransport(std::shared_ptr<D> downstream);

    // OpenSSL initialization (static, thread-safe)
    static void InitOpenSSL();

    // BIO operations
    void FlushWbio();
    void ProcessHandshake();
    void ProcessPendingReads();
    void RetryPendingWrites();

    // Error handling
    void HandleSSLError(int ssl_error, const char* operation);
    std::string GetSSLErrorString();

    // Cleanup
    void Cleanup();

    // Upstream write callback
    UpstreamWriteCallback upstream_write_ = [](BufferChain) {};

    // OpenSSL state
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    BIO* rbio_ = nullptr;  // Read BIO: encrypted input -> SSL
    BIO* wbio_ = nullptr;  // Write BIO: SSL -> encrypted output

    // State
    TlsHandshakeState handshake_state_ = TlsHandshakeState::NotStarted;
    std::function<void()> handshake_complete_cb_;

    // Pending write data (when SSL_write returns WANT_READ/WANT_WRITE)
    BufferChain write_pending_;

    // Pending read data (unconsumed decrypted data when suspended)
    BufferChain pending_read_chain_;

    // Buffer size limits
    static constexpr size_t kMaxPendingRead = 16 * 1024 * 1024;   // 16MB decrypted
    static constexpr size_t kMaxPendingWrite = 16 * 1024 * 1024;  // 16MB pending writes
    static constexpr size_t kMaxRbioSize = 16 * 1024 * 1024;      // 16MB encrypted input
};

// Implementation - must be in header due to template

template <Downstream D>
TlsTransport<D>::TlsTransport(std::shared_ptr<D> downstream) {
    this->SetDownstream(std::move(downstream));
    InitOpenSSL();

    // Create SSL context for client-side TLS
    ctx_ = SSL_CTX_new(TLS_client_method());
    if (!ctx_) {
        throw std::runtime_error("Failed to create SSL_CTX: " + GetSSLErrorString());
    }

    // Set reasonable security defaults
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx_, SSL_OP_NO_COMPRESSION);

    // Load default CA certificates
    SSL_CTX_set_default_verify_paths(ctx_);

    // Enable peer certificate verification
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);

    // Create SSL connection
    ssl_ = SSL_new(ctx_);
    if (!ssl_) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
        throw std::runtime_error("Failed to create SSL: " + GetSSLErrorString());
    }

    // Create memory BIOs
    rbio_ = BIO_new(BIO_s_mem());
    wbio_ = BIO_new(BIO_s_mem());
    if (!rbio_ || !wbio_) {
        if (rbio_) BIO_free(rbio_);
        if (wbio_) BIO_free(wbio_);
        SSL_free(ssl_);
        SSL_CTX_free(ctx_);
        ssl_ = nullptr;
        ctx_ = nullptr;
        throw std::runtime_error("Failed to create BIOs");
    }

    // Connect BIOs to SSL - SSL takes ownership
    SSL_set_bio(ssl_, rbio_, wbio_);

    // Set client mode
    SSL_set_connect_state(ssl_);
}

template <Downstream D>
void TlsTransport<D>::InitOpenSSL() {
    // Thread-safe static initialization using C++11 magic statics
    [[maybe_unused]] static bool _ = []() {
        OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS |
                         OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);
        return true;
    }();
}

template <Downstream D>
void TlsTransport<D>::SetHostname(const std::string& hostname) {
    if (ssl_) {
        // Set SNI hostname
        SSL_set_tlsext_host_name(ssl_, hostname.c_str());
        // Set hostname for certificate verification
        SSL_set1_host(ssl_, hostname.c_str());
    }
}

template <Downstream D>
void TlsTransport<D>::StartHandshake() {
    if (handshake_state_ != TlsHandshakeState::NotStarted) return;
    handshake_state_ = TlsHandshakeState::InProgress;

    ProcessHandshake();
}

template <Downstream D>
void TlsTransport<D>::ProcessHandshake() {
    if (handshake_state_ == TlsHandshakeState::Complete) return;

    int ret = SSL_do_handshake(ssl_);
    if (ret == 1) {
        // Handshake complete
        handshake_state_ = TlsHandshakeState::Complete;
        FlushWbio();
        // Drain any application data that arrived with the final handshake bytes
        // Only if not suspended - ProcessPendingReads checks but be explicit
        if (!this->IsSuspended()) {
            ProcessPendingReads();
        }
        // Notify that handshake is complete
        if (handshake_complete_cb_) {
            handshake_complete_cb_();
        }
        return;
    }

    int ssl_error = SSL_get_error(ssl_, ret);
    switch (ssl_error) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            // Need more data or need to send data
            FlushWbio();
            break;

        case SSL_ERROR_ZERO_RETURN:
            // Clean shutdown during handshake - defer if suspended
            if (this->IsSuspended()) {
                this->DeferOnDone();
                return;
            }
            if (this->CompleteWithFlush(pending_read_chain_)) {
                this->RequestClose();
            }
            break;

        default:
            HandleSSLError(ssl_error, "handshake");
            break;
    }
}

template <Downstream D>
void TlsTransport<D>::Read(BufferChain data) {
    auto guard = this->TryGuard();
    if (!guard) return;

    // Check for buffer overflow before accepting more data (guard against underflow)
    size_t rbio_pending = BIO_ctrl_pending(rbio_);
    if (rbio_pending >= kMaxRbioSize || data.Size() > kMaxRbioSize - rbio_pending) {
        this->EmitError(
            Error{ErrorCode::BufferOverflow, "TLS encrypted input buffer overflow"});
        this->RequestClose();
        return;
    }

    // Feed encrypted data from chain to the read BIO
    while (!data.Empty()) {
        // Get contiguous chunk from chain
        size_t chunk_size = data.ContiguousSize();
        const std::byte* chunk_ptr = data.DataAt(0);

        int written = BIO_write(rbio_, chunk_ptr, static_cast<int>(chunk_size));
        if (written <= 0) {
            this->EmitError(
                            {ErrorCode::TlsHandshakeFailed, "BIO_write failed"});
            this->RequestClose();
            return;
        }
        data.Consume(static_cast<size_t>(written));
    }

    // If handshake not complete, continue it
    if (handshake_state_ != TlsHandshakeState::Complete) {
        ProcessHandshake();
        return;
    }

    // Read decrypted data from SSL
    ProcessPendingReads();
}

template <Downstream D>
void TlsTransport<D>::ProcessPendingReads() {
    if (this->IsClosed() || handshake_state_ != TlsHandshakeState::Complete) return;

    // Ensure recycling callback is set
    pending_read_chain_.SetRecycleCallback(this->GetAllocator().MakeRecycler());

    // Encrypted data stays in rbio_, decrypted in pending_read_chain_
    if (this->IsSuspended()) return;

    while (true) {
        auto seg = this->GetAllocator().Allocate();
        int n = SSL_read(ssl_, seg->data.data(), static_cast<int>(Segment::kSize));
        if (n > 0) {
            // Check overflow before appending (use subtraction pattern)
            if (static_cast<size_t>(n) > kMaxPendingRead - pending_read_chain_.Size()) {
                this->EmitError(
                    Error{ErrorCode::BufferOverflow, "TLS decrypted output buffer overflow"});
                this->RequestClose();
                return;
            }
            seg->size = static_cast<size_t>(n);
            pending_read_chain_.Append(std::move(seg));

            // Deliver accumulated data and check for suspension
            if (pending_read_chain_.Size() >= Segment::kSize) {
                this->GetDownstream().OnData(pending_read_chain_);
                if (this->IsSuspended()) {
                    return;
                }
            }
        } else {
            int ssl_error = SSL_get_error(ssl_, n);
            switch (ssl_error) {
                case SSL_ERROR_WANT_READ:
                    // No more data available - deliver what we have
                    if (!pending_read_chain_.Empty()) {
                        this->GetDownstream().OnData(pending_read_chain_);
                    }
                    return;

                case SSL_ERROR_WANT_WRITE:
                    // Need to flush write BIO
                    if (!pending_read_chain_.Empty()) {
                        this->GetDownstream().OnData(pending_read_chain_);
                    }
                    FlushWbio();
                    // Retry any pending writes
                    RetryPendingWrites();
                    return;

                case SSL_ERROR_ZERO_RETURN:
                    // Clean TLS shutdown - defer if suspended
                    if (this->IsSuspended()) {
                        this->DeferOnDone();
                        return;
                    }
                    if (this->CompleteWithFlush(pending_read_chain_)) {
                        this->RequestClose();
                    }
                    return;

                default:
                    HandleSSLError(ssl_error, "SSL_read");
                    return;
            }
        }
    }
}

template <Downstream D>
void TlsTransport<D>::Write(BufferChain data) {
    auto guard = this->TryGuard();
    if (!guard) return;

    if (handshake_state_ != TlsHandshakeState::Complete) {
        this->EmitError(
            {ErrorCode::TlsHandshakeFailed, "Write called before handshake complete"});
        this->RequestClose();
        return;
    }

    // Check for buffer overflow (combined incoming + pending)
    if (write_pending_.WouldOverflow(data.Size(), kMaxPendingWrite)) {
        this->EmitError(
            Error{ErrorCode::BufferOverflow, "TLS write buffer overflow"});
        this->RequestClose();
        return;
    }

    // Prepend any pending write data
    if (!write_pending_.Empty()) {
        write_pending_.CompactAndSplice(data);
        data = std::move(write_pending_);
    }

    // Write plaintext to SSL for encryption (iterate over chain segments)
    while (!data.Empty()) {
        size_t chunk_size = data.ContiguousSize();
        const std::byte* ptr = data.DataAt(0);
        int written = SSL_write(ssl_, ptr, static_cast<int>(chunk_size));
        if (written <= 0) {
            int ssl_error = SSL_get_error(ssl_, written);
            if (ssl_error != SSL_ERROR_WANT_WRITE && ssl_error != SSL_ERROR_WANT_READ) {
                HandleSSLError(ssl_error, "SSL_write");
                return;
            }
            // Save remaining data for later retry (use move, not Splice,
            // because data may have consumed_offset_ > 0 from partial write)
            write_pending_ = std::move(data);
            break;
        }
        data.Consume(static_cast<size_t>(written));
    }

    // Flush encrypted data to upstream
    FlushWbio();
}

template <Downstream D>
void TlsTransport<D>::RetryPendingWrites() {
    if (write_pending_.Empty()) return;

    // Move pending data to local and retry
    BufferChain data = std::move(write_pending_);

    while (!data.Empty()) {
        size_t chunk_size = data.ContiguousSize();
        const std::byte* ptr = data.DataAt(0);
        int written = SSL_write(ssl_, ptr, static_cast<int>(chunk_size));
        if (written <= 0) {
            int ssl_error = SSL_get_error(ssl_, written);
            if (ssl_error != SSL_ERROR_WANT_WRITE && ssl_error != SSL_ERROR_WANT_READ) {
                HandleSSLError(ssl_error, "SSL_write");
                return;
            }
            // Still blocked, save remaining data (use move, not Splice,
            // because data may have consumed_offset_ > 0 from partial write)
            write_pending_ = std::move(data);
            break;
        }
        data.Consume(static_cast<size_t>(written));
    }

    FlushWbio();
}

template <Downstream D>
void TlsTransport<D>::FlushWbio() {
    // Read encrypted data from write BIO and send upstream
    int pending = BIO_ctrl_pending(wbio_);
    if (pending <= 0) return;

    // Create BufferChain with encrypted data
    BufferChain chain;
    while (pending > 0) {
        auto seg = this->GetAllocator().Allocate();
        size_t to_read = std::min(static_cast<size_t>(pending), Segment::kSize);
        int read = BIO_read(wbio_, seg->data.data(), static_cast<int>(to_read));
        if (read > 0) {
            seg->size = static_cast<size_t>(read);
            chain.Append(std::move(seg));
            pending -= read;
        } else {
            break;
        }
    }

    if (!chain.Empty()) {
        upstream_write_(std::move(chain));
    }
}

template <Downstream D>
void TlsTransport<D>::HandleSSLError(int ssl_error, const char* operation) {
    std::string msg = std::string(operation) + " failed: ";

    switch (ssl_error) {
        case SSL_ERROR_SSL:
            msg += GetSSLErrorString();
            this->EmitError(
                            {ErrorCode::TlsHandshakeFailed, std::move(msg)});
            break;

        case SSL_ERROR_SYSCALL: {
            int err = errno;
            msg += "system error: " + std::string(strerror(err));
            this->EmitError(
                            {ErrorCode::ConnectionFailed, std::move(msg), err});
            break;
        }

        default:
            msg += "error code " + std::to_string(ssl_error);
            this->EmitError(
                            {ErrorCode::TlsHandshakeFailed, std::move(msg)});
            break;
    }

    // Close the socket after any SSL error
    this->RequestClose();
}

template <Downstream D>
std::string TlsTransport<D>::GetSSLErrorString() {
    unsigned long err = ERR_get_error();
    if (err == 0) return "unknown error";

    char buf[256];
    ERR_error_string_n(err, buf, sizeof(buf));
    return std::string(buf);
}

template <Downstream D>
void TlsTransport<D>::Cleanup() {
    // Note: SSL_set_bio() transfers ownership of BIOs to SSL,
    // so we only free SSL (which frees the BIOs)
    if (ssl_) {
        SSL_free(ssl_);
        ssl_ = nullptr;
        rbio_ = nullptr;  // Freed by SSL_free
        wbio_ = nullptr;  // Freed by SSL_free
    }

    if (ctx_) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
}

template <Downstream D>
void TlsTransport<D>::DoClose() {
    if (ssl_ && handshake_state_ == TlsHandshakeState::Complete) {
        // Initiate TLS shutdown
        SSL_shutdown(ssl_);
        FlushWbio();
    }

    Cleanup();

    // Best-effort delivery of remaining decrypted data during cleanup
    // Skip if already finalized or if suspended (can't emit Done while suspended)
    if (!this->IsFinalized() && !this->IsSuspended()) {
        this->ForwardData(pending_read_chain_);
        // Re-check suspension after ForwardData (downstream may have suspended)
        // and verify chain was consumed before emitting Done
        if (!this->IsSuspended() && pending_read_chain_.Empty()) {
            this->EmitDone();
        }
    }

    // Clear pending buffers to release segments promptly
    pending_read_chain_.Clear();
    write_pending_.Clear();

    // Release downstream reference for cleanup consistency
    this->ResetDownstream();
}

}  // namespace dbn_pipe
