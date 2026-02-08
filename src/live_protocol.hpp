// SPDX-License-Identifier: MIT

// src/live_protocol.hpp
#pragma once

#include <memory>
#include <string>

#include "dbn_pipe/stream/buffer_chain.hpp"
#include "dbn_pipe/stream/segment_allocator.hpp"
#include "dbn_pipe/cram_auth.hpp"
#include "dbn_pipe/dbn_parser_component.hpp"
#include "dbn_pipe/stream/event_loop.hpp"
#include "dbn_pipe/stream/component.hpp"
#include "dbn_pipe/stream/sink.hpp"
#include "dbn_pipe/stream/tcp_socket.hpp"

namespace dbn_pipe {

/// Parameters for a live data subscription request.
struct LiveRequest {
    std::string dataset;   ///< Dataset to subscribe to (e.g., "GLBX.MDP3").
    std::string symbols;   ///< Symbol(s) to subscribe to (e.g., "ESZ4").
    std::string schema;    ///< Schema for data (e.g., "mbp-1").
};

/// Default port for the Databento live streaming gateway.
constexpr uint16_t kLivePort = 13000;

/// ProtocolDriver implementation for live market-data streaming.
///
/// Satisfies the ProtocolDriver concept. Uses CramAuth for authentication
/// and subscription management.
///
/// Chain: TcpSocket -> CramAuth -> DbnParserComponent -> Sink
///
/// The live protocol is ready immediately on connect (OnConnect returns true).
/// SendRequest subscribes to the dataset/symbols/schema and starts streaming.
struct LiveProtocol {
    using Request = LiveRequest;         ///< Request type for this protocol.
    using SinkType = StreamRecordSink;   ///< Sink type that receives parsed records.

    /// @internal
    /// Type-erased interface for the full pipeline including TcpSocket.
    /// Hides concrete template parameters from callers.
    struct ChainType {
        virtual ~ChainType() = default;

        /// Initiate a TCP connection to the given address.
        virtual void Connect(const sockaddr_storage& addr) = 0;
        /// Close the connection and release resources.
        virtual void Close() = 0;

        /// Register a callback that fires when the chain is ready to send
        /// a request (immediately after connect for the live protocol).
        virtual void SetReadyCallback(std::function<void()> cb) = 0;

        /// Set the dataset, required for CRAM authentication.
        /// @param dataset  Dataset identifier (e.g., "GLBX.MDP3").
        virtual void SetDataset(const std::string& dataset) = 0;

        /// Pause reading from the socket (backpressure).
        virtual void Suspend() = 0;
        /// Resume reading from the socket.
        virtual void Resume() = 0;
        /// @return true if the chain is currently suspended.
        virtual bool IsSuspended() const = 0;

        /// Subscribe to the given symbols and schema.
        /// @param symbols  Symbol filter string.
        /// @param schema   Schema name (e.g., "mbp-1").
        virtual void Subscribe(std::string symbols, std::string schema) = 0;
        /// Signal the gateway to begin streaming data.
        virtual void StartStreaming() = 0;
    };

    /// @internal
    /// Concrete implementation of ChainType.
    /// TcpSocket is the head; static dispatch within the chain via template parameters.
    struct ChainImpl : ChainType {
        using ParserType = DbnParserComponent<StreamRecordSink>;
        using CramType = CramAuth<ParserType>;
        using HeadType = TcpSocket<CramType>;

        ChainImpl(IEventLoop& loop, StreamRecordSink& sink, const std::string& api_key,
                  SegmentAllocator* alloc = nullptr)
            : allocator_(alloc ? *alloc : SegmentAllocator{})
            , parser_(std::make_shared<ParserType>(sink))
            , cram_(CramType::Create(parser_, api_key))
            , head_(HeadType::Create(loop, cram_))
        {
            // Wire defer callback from event loop
            auto defer = [&loop](auto fn) { loop.Defer(std::move(fn)); };
            cram_->SetDefer(defer);

            // Wire allocator to all components
            head_->SetAllocator(&allocator_);
            cram_->SetAllocator(&allocator_);
            parser_->SetAllocator(&allocator_);

            // Wire connect callback for ready signal
            head_->OnConnect([this]() {
                // Live protocol is ready immediately on connect
                if (ready_cb_) ready_cb_();
            });
        }

        /// @copydoc ChainType::Connect
        void Connect(const sockaddr_storage& addr) override {
            head_->Connect(addr);
        }

        /// @copydoc ChainType::Close
        void Close() override {
            head_->Close();
        }

        /// @copydoc ChainType::SetReadyCallback
        void SetReadyCallback(std::function<void()> cb) override {
            ready_cb_ = std::move(cb);
        }

        /// @copydoc ChainType::SetDataset
        void SetDataset(const std::string& dataset) override {
            cram_->SetDataset(dataset);
        }

        /// @copydoc ChainType::Suspend
        void Suspend() override { head_->Suspend(); }
        /// @copydoc ChainType::Resume
        void Resume() override { head_->Resume(); }
        /// @copydoc ChainType::IsSuspended
        bool IsSuspended() const override { return head_->IsSuspended(); }

        /// @copydoc ChainType::Subscribe
        void Subscribe(std::string symbols, std::string schema) override {
            cram_->Subscribe(std::move(symbols), std::move(schema));
        }

        /// @copydoc ChainType::StartStreaming
        void StartStreaming() override {
            cram_->StartStreaming();
        }

    private:
        SegmentAllocator allocator_;
        std::shared_ptr<ParserType> parser_;
        std::shared_ptr<CramType> cram_;
        std::shared_ptr<HeadType> head_;
        std::function<void()> ready_cb_;
    };

    /// Build the component chain for the live protocol.
    /// @param loop     Event loop that drives I/O.
    /// @param sink     Destination for parsed record batches.
    /// @param api_key  Databento API key for CRAM authentication.
    /// @return Owning pointer to the newly created chain.
    static std::shared_ptr<ChainType> BuildChain(
        IEventLoop& loop,
        StreamRecordSink& sink,
        const std::string& api_key
    ) {
        return std::make_shared<ChainImpl>(loop, sink, api_key);
    }

    /// Build the component chain with an explicit allocator.
    /// @param loop     Event loop that drives I/O.
    /// @param sink     Destination for parsed record batches.
    /// @param api_key  Databento API key for CRAM authentication.
    /// @param alloc    Allocator to copy into the chain; all components share it.
    /// @return Owning pointer to the newly created chain.
    static std::shared_ptr<ChainType> BuildChain(
        IEventLoop& loop,
        StreamRecordSink& sink,
        const std::string& api_key,
        SegmentAllocator* alloc
    ) {
        return std::make_shared<ChainImpl>(loop, sink, api_key, alloc);
    }

    /// Subscribe to the requested symbols/schema and begin streaming.
    ///
    /// SetDataset must be called before Connect so that CRAM authentication
    /// has the dataset available.
    /// @param chain    Active chain (must be connected and ready).
    /// @param request  Parameters describing the live subscription.
    static void SendRequest(std::shared_ptr<ChainType>& chain, const Request& request) {
        if (chain) {
            chain->Subscribe(request.symbols, request.schema);
            chain->StartStreaming();
        }
    }

    /// Close the chain and release its resources.
    /// @param chain  Chain to tear down; may be null.
    static void Teardown(std::shared_ptr<ChainType>& chain) {
        if (chain) {
            chain->Close();
        }
    }

    /// Derive the gateway hostname from the dataset.
    /// For example, "GLBX.MDP3" becomes "glbx-mdp3.lsg.databento.com".
    /// @param request  Request whose dataset field is used.
    /// @return Fully-qualified gateway hostname.
    static std::string GetHostname(const Request& request) {
        std::string hostname;
        hostname.reserve(request.dataset.size() + 20);
        for (char c : request.dataset) {
            hostname += (c == '.') ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        hostname += ".lsg.databento.com";
        return hostname;
    }

    /// @return The gateway port (always kLivePort).
    static uint16_t GetPort(const Request&) {
        return kLivePort;
    }
};

}  // namespace dbn_pipe
