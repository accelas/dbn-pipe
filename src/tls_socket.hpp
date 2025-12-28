// src/tls_socket.hpp
#pragma once

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cerrno>
#include <cstddef>
#include <functional>
#include <memory>
#include <memory_resource>
#include <string>
#include <vector>

#include "error.hpp"
#include "pipeline.hpp"
#include "reactor.hpp"

namespace databento_async {

// Suspendable base class for upstream control flow
class Suspendable {
public:
    virtual ~Suspendable() = default;

    virtual void Suspend() = 0;
    virtual void Resume() = 0;
    bool IsSuspended() const { return suspended_; }

protected:
    bool suspended_ = false;
};

// TlsSocket handles TLS encryption/decryption using OpenSSL with memory BIOs.
// Sits between TcpSocket (upstream) and HttpClient (downstream) in the pipeline.
//
// Template parameter D must satisfy the Downstream concept.
template <Downstream D>
class TlsSocket : public PipelineComponent<TlsSocket<D>>,
                  public Suspendable,
                  public std::enable_shared_from_this<TlsSocket<D>> {
public:
    using UpstreamWriteCallback = std::function<void(std::pmr::vector<std::byte>)>;

    // Factory method for shared_from_this safety
    static std::shared_ptr<TlsSocket> Create(Reactor& reactor,
                                              std::shared_ptr<D> downstream) {
        // Use a helper struct to access private constructor
        struct MakeSharedEnabler : public TlsSocket {
            MakeSharedEnabler(Reactor& r, std::shared_ptr<D> ds)
                : TlsSocket(r, std::move(ds)) {}
        };
        return std::make_shared<MakeSharedEnabler>(reactor, std::move(downstream));
    }

    ~TlsSocket() { Cleanup(); }

    // Upstream interface: receive encrypted data from TcpSocket
    void Read(std::pmr::vector<std::byte> data);

    // Upstream interface: write plaintext (from downstream) to be encrypted
    void Write(std::pmr::vector<std::byte> data);

    // Suspendable interface
    void Suspend() override {
        suspended_ = true;
        // Propagate suspend to upstream if callback set
    }

    void Resume() override {
        suspended_ = false;
        // Process any buffered data
        ProcessPendingReads();
    }

    // Pipeline interface
    void Close() { this->RequestClose(); }

    // TLS-specific methods
    void StartHandshake();
    bool IsHandshakeComplete() const { return handshake_complete_; }

    // Set hostname for SNI (Server Name Indication)
    void SetHostname(const std::string& hostname);

    // Set callback to send encrypted data upstream (to TcpSocket)
    void SetUpstreamWriteCallback(UpstreamWriteCallback cb) {
        upstream_write_ = std::move(cb);
    }

    // Required by PipelineComponent
    void DisableWatchers() {
        // No direct epoll watchers; TLS operates on memory BIOs
    }

    void DoClose();

private:
    // Private constructor - use Create() factory method
    TlsSocket(Reactor& reactor, std::shared_ptr<D> downstream);

    // OpenSSL initialization (static, thread-safe)
    static void InitOpenSSL();

    // BIO operations
    void FlushWbio();
    void ProcessHandshake();
    void ProcessPendingReads();

    // Error handling
    void HandleSSLError(int ssl_error, const char* operation);
    std::string GetSSLErrorString();

    // Cleanup
    void Cleanup();

    // Downstream component
    std::shared_ptr<D> downstream_;

    // Upstream write callback
    UpstreamWriteCallback upstream_write_ = [](std::pmr::vector<std::byte>) {};

    // OpenSSL state
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    BIO* rbio_ = nullptr;  // Read BIO: encrypted input -> SSL
    BIO* wbio_ = nullptr;  // Write BIO: SSL -> encrypted output

    // State
    bool handshake_complete_ = false;
    bool handshake_started_ = false;

    // PMR allocator for encryption/decryption buffers
    std::pmr::unsynchronized_pool_resource pool_;
    std::pmr::polymorphic_allocator<std::byte> alloc_{&pool_};

    // Buffer size constants
    static constexpr size_t kBufferSize = 16384;  // 16KB, typical TLS record size
};

// Implementation - must be in header due to template

template <Downstream D>
TlsSocket<D>::TlsSocket(Reactor& reactor, std::shared_ptr<D> downstream)
    : PipelineComponent<TlsSocket<D>>(reactor), downstream_(std::move(downstream)) {
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
void TlsSocket<D>::InitOpenSSL() {
    // Thread-safe static initialization using C++11 magic statics
    [[maybe_unused]] static bool _ = []() {
        OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS |
                         OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);
        return true;
    }();
}

template <Downstream D>
void TlsSocket<D>::SetHostname(const std::string& hostname) {
    if (ssl_) {
        // Set SNI hostname
        SSL_set_tlsext_host_name(ssl_, hostname.c_str());
        // Set hostname for certificate verification
        SSL_set1_host(ssl_, hostname.c_str());
    }
}

template <Downstream D>
void TlsSocket<D>::StartHandshake() {
    if (handshake_started_) return;
    handshake_started_ = true;

    ProcessHandshake();
}

template <Downstream D>
void TlsSocket<D>::ProcessHandshake() {
    if (handshake_complete_) return;

    int ret = SSL_do_handshake(ssl_);
    if (ret == 1) {
        // Handshake complete
        handshake_complete_ = true;
        FlushWbio();
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
            // Clean shutdown
            this->EmitDone(*downstream_);
            break;

        default:
            HandleSSLError(ssl_error, "handshake");
            break;
    }
}

template <Downstream D>
void TlsSocket<D>::Read(std::pmr::vector<std::byte> data) {
    auto guard = this->TryGuard();
    if (!guard) return;

    // Feed encrypted data to the read BIO
    int written = BIO_write(rbio_, data.data(), static_cast<int>(data.size()));
    if (written <= 0) {
        this->EmitError(*downstream_,
                        {ErrorCode::TlsHandshakeFailed, "BIO_write failed"});
        this->RequestClose();
        return;
    }

    // If handshake not complete, continue it
    if (!handshake_complete_) {
        ProcessHandshake();
        return;
    }

    // If suspended, buffer the data
    if (suspended_) {
        // Data is already in BIO, will be read on resume
        return;
    }

    // Read decrypted data from SSL
    ProcessPendingReads();
}

template <Downstream D>
void TlsSocket<D>::ProcessPendingReads() {
    if (this->IsClosed() || !handshake_complete_) return;

    std::pmr::vector<std::byte> plaintext(kBufferSize, alloc_);

    while (true) {
        int n = SSL_read(ssl_, plaintext.data(), static_cast<int>(plaintext.size()));
        if (n > 0) {
            plaintext.resize(static_cast<size_t>(n));
            downstream_->Read(std::move(plaintext));
            plaintext = std::pmr::vector<std::byte>(kBufferSize, alloc_);
        } else {
            int ssl_error = SSL_get_error(ssl_, n);
            switch (ssl_error) {
                case SSL_ERROR_WANT_READ:
                    // No more data available
                    return;

                case SSL_ERROR_WANT_WRITE:
                    // Need to flush write BIO
                    FlushWbio();
                    return;

                case SSL_ERROR_ZERO_RETURN:
                    // Clean TLS shutdown
                    this->EmitDone(*downstream_);
                    return;

                default:
                    HandleSSLError(ssl_error, "SSL_read");
                    return;
            }
        }
    }
}

template <Downstream D>
void TlsSocket<D>::Write(std::pmr::vector<std::byte> data) {
    auto guard = this->TryGuard();
    if (!guard) return;

    if (!handshake_complete_) {
        this->EmitError(*downstream_,
            {ErrorCode::TlsHandshakeFailed, "Write called before handshake complete"});
        return;
    }

    // Write plaintext to SSL for encryption
    int written = SSL_write(ssl_, data.data(), static_cast<int>(data.size()));
    if (written <= 0) {
        int ssl_error = SSL_get_error(ssl_, written);
        if (ssl_error != SSL_ERROR_WANT_WRITE && ssl_error != SSL_ERROR_WANT_READ) {
            HandleSSLError(ssl_error, "SSL_write");
            return;
        }
    }

    // Flush encrypted data to upstream
    FlushWbio();
}

template <Downstream D>
void TlsSocket<D>::FlushWbio() {
    // Read encrypted data from write BIO and send upstream
    int pending = BIO_ctrl_pending(wbio_);
    if (pending <= 0) return;

    std::pmr::vector<std::byte> encrypted(static_cast<size_t>(pending), alloc_);
    int read = BIO_read(wbio_, encrypted.data(), pending);
    if (read > 0) {
        encrypted.resize(static_cast<size_t>(read));
        upstream_write_(std::move(encrypted));
    }
}

template <Downstream D>
void TlsSocket<D>::HandleSSLError(int ssl_error, const char* operation) {
    std::string msg = std::string(operation) + " failed: ";

    switch (ssl_error) {
        case SSL_ERROR_SSL:
            msg += GetSSLErrorString();
            this->EmitError(*downstream_,
                            {ErrorCode::TlsHandshakeFailed, std::move(msg)});
            break;

        case SSL_ERROR_SYSCALL: {
            int err = errno;
            msg += "system error: " + std::string(strerror(err));
            this->EmitError(*downstream_,
                            {ErrorCode::ConnectionFailed, std::move(msg), err});
            break;
        }

        default:
            msg += "error code " + std::to_string(ssl_error);
            this->EmitError(*downstream_,
                            {ErrorCode::TlsHandshakeFailed, std::move(msg)});
            break;
    }
}

template <Downstream D>
std::string TlsSocket<D>::GetSSLErrorString() {
    unsigned long err = ERR_get_error();
    if (err == 0) return "unknown error";

    char buf[256];
    ERR_error_string_n(err, buf, sizeof(buf));
    return std::string(buf);
}

template <Downstream D>
void TlsSocket<D>::Cleanup() {
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
void TlsSocket<D>::DoClose() {
    if (ssl_ && handshake_complete_) {
        // Initiate TLS shutdown
        SSL_shutdown(ssl_);
        FlushWbio();
    }

    Cleanup();

    // Notify downstream
    this->EmitDone(*downstream_);
}

}  // namespace databento_async
