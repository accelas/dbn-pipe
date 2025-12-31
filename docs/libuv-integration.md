# Libuv Integration Guide

This guide shows how to integrate the databento-async library with an existing libuv event loop.

## Overview

The `IEventLoop` interface allows you to integrate databento-async with any event loop system. This guide demonstrates how to create a libuv adapter that wraps an existing `uv_loop_t*` without taking ownership.

Key design principles:
- **Non-owning adapter**: Wraps your existing `uv_loop_t*` - you retain full control
- **Uses `uv_poll_t`**: For file descriptor events (readable/writable)
- **Uses `uv_async_t`**: For thread-safe `Defer()` cross-thread wakeup
- **Your loop drives everything**: No hidden threads or separate event loops

## Example Implementation

### LibuvEventHandle

Implements `IEventHandle` using `uv_poll_t` for fd monitoring:

```cpp
#include <uv.h>

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "event_loop.hpp"

namespace dbn_pipe {

class LibuvEventLoop;

class LibuvEventHandle : public IEventHandle {
public:
    LibuvEventHandle(LibuvEventLoop& loop, int fd,
                     bool want_read, bool want_write,
                     IEventLoop::ReadCallback on_read,
                     IEventLoop::WriteCallback on_write,
                     IEventLoop::ErrorCallback on_error);

    ~LibuvEventHandle() override;

    // Non-copyable, non-movable
    LibuvEventHandle(const LibuvEventHandle&) = delete;
    LibuvEventHandle& operator=(const LibuvEventHandle&) = delete;
    LibuvEventHandle(LibuvEventHandle&&) = delete;
    LibuvEventHandle& operator=(LibuvEventHandle&&) = delete;

    // IEventHandle interface
    void Update(bool want_read, bool want_write) override;
    int fd() const override { return fd_; }

private:
    static void OnPoll(uv_poll_t* handle, int status, int events);
    int ComputeUvEvents(bool want_read, bool want_write) const;

    LibuvEventLoop& loop_;
    int fd_;
    uv_poll_t poll_handle_;
    IEventLoop::ReadCallback on_read_;
    IEventLoop::WriteCallback on_write_;
    IEventLoop::ErrorCallback on_error_;
};

// Implementation

LibuvEventHandle::LibuvEventHandle(
    LibuvEventLoop& loop, int fd,
    bool want_read, bool want_write,
    IEventLoop::ReadCallback on_read,
    IEventLoop::WriteCallback on_write,
    IEventLoop::ErrorCallback on_error)
    : loop_(loop)
    , fd_(fd)
    , on_read_(std::move(on_read))
    , on_write_(std::move(on_write))
    , on_error_(std::move(on_error))
{
    uv_poll_init(loop_.uv_loop(), &poll_handle_, fd);
    poll_handle_.data = this;

    int events = ComputeUvEvents(want_read, want_write);
    if (events != 0) {
        uv_poll_start(&poll_handle_, events, OnPoll);
    }
}

LibuvEventHandle::~LibuvEventHandle() {
    uv_poll_stop(&poll_handle_);
    // Note: In production, you may need uv_close() with a callback
    // if the handle might outlive the event loop iteration
}

void LibuvEventHandle::Update(bool want_read, bool want_write) {
    int events = ComputeUvEvents(want_read, want_write);
    if (events == 0) {
        uv_poll_stop(&poll_handle_);
    } else {
        uv_poll_start(&poll_handle_, events, OnPoll);
    }
}

int LibuvEventHandle::ComputeUvEvents(bool want_read, bool want_write) const {
    int events = 0;
    if (want_read) events |= UV_READABLE;
    if (want_write) events |= UV_WRITABLE;
    return events;
}

void LibuvEventHandle::OnPoll(uv_poll_t* handle, int status, int events) {
    auto* self = static_cast<LibuvEventHandle*>(handle->data);

    if (status < 0) {
        // Error occurred - map libuv error to errno-style
        if (self->on_error_) {
            self->on_error_(-status);  // libuv errors are negative errno values
        }
        return;
    }

    if ((events & UV_READABLE) && self->on_read_) {
        self->on_read_();
    }
    if ((events & UV_WRITABLE) && self->on_write_) {
        self->on_write_();
    }
}

}  // namespace dbn_pipe
```

### LibuvEventLoop

Implements `IEventLoop` wrapping an existing `uv_loop_t*`:

```cpp
namespace dbn_pipe {

class LibuvEventLoop : public IEventLoop {
public:
    // Non-owning: wraps existing loop, does not take ownership
    explicit LibuvEventLoop(uv_loop_t* loop);
    ~LibuvEventLoop() override;

    // Non-copyable, non-movable
    LibuvEventLoop(const LibuvEventLoop&) = delete;
    LibuvEventLoop& operator=(const LibuvEventLoop&) = delete;
    LibuvEventLoop(LibuvEventLoop&&) = delete;
    LibuvEventLoop& operator=(LibuvEventLoop&&) = delete;

    // IEventLoop interface
    std::unique_ptr<IEventHandle> Register(
        int fd,
        bool want_read,
        bool want_write,
        ReadCallback on_read,
        WriteCallback on_write,
        ErrorCallback on_error) override;

    void Defer(std::function<void()> fn) override;
    bool IsInEventLoopThread() const override;

    // Access underlying loop (for LibuvEventHandle)
    uv_loop_t* uv_loop() const { return loop_; }

    // Call once from event loop thread to record thread ID
    void SetEventLoopThread();

private:
    static void OnAsync(uv_async_t* handle);
    void ProcessDeferredCallbacks();

    uv_loop_t* loop_;  // Non-owning
    uv_async_t async_handle_;
    std::atomic<std::thread::id> loop_thread_id_{};

    std::mutex deferred_mutex_;
    std::vector<std::function<void()>> deferred_callbacks_;
};

// Implementation

LibuvEventLoop::LibuvEventLoop(uv_loop_t* loop)
    : loop_(loop)
{
    uv_async_init(loop_, &async_handle_, OnAsync);
    async_handle_.data = this;
}

LibuvEventLoop::~LibuvEventLoop() {
    // Stop and close the async handle
    // Note: Full cleanup requires uv_close() with callback in production
    uv_close(reinterpret_cast<uv_handle_t*>(&async_handle_), nullptr);
}

std::unique_ptr<IEventHandle> LibuvEventLoop::Register(
    int fd,
    bool want_read,
    bool want_write,
    ReadCallback on_read,
    WriteCallback on_write,
    ErrorCallback on_error)
{
    return std::make_unique<LibuvEventHandle>(
        *this, fd, want_read, want_write,
        std::move(on_read), std::move(on_write), std::move(on_error));
}

void LibuvEventLoop::Defer(std::function<void()> fn) {
    {
        std::lock_guard lock(deferred_mutex_);
        deferred_callbacks_.push_back(std::move(fn));
    }
    // Wake up event loop from any thread
    uv_async_send(&async_handle_);
}

bool LibuvEventLoop::IsInEventLoopThread() const {
    auto expected = loop_thread_id_.load(std::memory_order_acquire);
    return expected == std::thread::id{} ||
           expected == std::this_thread::get_id();
}

void LibuvEventLoop::SetEventLoopThread() {
    loop_thread_id_.store(std::this_thread::get_id(), std::memory_order_release);
}

void LibuvEventLoop::OnAsync(uv_async_t* handle) {
    auto* self = static_cast<LibuvEventLoop*>(handle->data);
    self->ProcessDeferredCallbacks();
}

void LibuvEventLoop::ProcessDeferredCallbacks() {
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard lock(deferred_mutex_);
        callbacks.swap(deferred_callbacks_);
    }
    for (auto& cb : callbacks) {
        cb();
    }
}

}  // namespace dbn_pipe
```

## Usage Example

Here is how to use the libuv adapter with a Pipeline:

```cpp
#include <uv.h>

#include "event_loop.hpp"
#include "pipeline.hpp"
#include "live_protocol.hpp"

// Include the libuv adapter implementation
#include "libuv_event_loop.hpp"

int main() {
    // Your existing libuv loop
    uv_loop_t* loop = uv_default_loop();

    // Create adapter wrapping your loop (non-owning)
    dbn_pipe::LibuvEventLoop event_loop(loop);

    // Record event loop thread (call from main/event loop thread)
    event_loop.SetEventLoopThread();

    // Create pipeline using the adapter
    using LivePipeline = dbn_pipe::Pipeline<
        dbn_pipe::LiveProtocol,
        databento::Record>;

    auto pipeline = LivePipeline::Create(event_loop, "your-api-key");

    // Configure the pipeline
    pipeline->SetRequest(dbn_pipe::LiveRequest{
        .dataset = "GLBX.MDP3",
        .symbols = "ES.FUT",
        .schema = databento::Schema::Trades,
        .stype_in = databento::SType::Parent
    });

    pipeline->OnRecord([](const databento::Record& record) {
        // Handle received records
        std::cout << "Received record with rtype: "
                  << static_cast<int>(record.header().rtype()) << "\n";
    });

    pipeline->OnError([](const dbn_pipe::Error& error) {
        std::cerr << "Error: " << error.message << "\n";
    });

    pipeline->OnComplete([]() {
        std::cout << "Stream completed\n";
    });

    // Connect and start
    pipeline->Connect();
    pipeline->Start();

    // Your existing libuv loop drives everything
    uv_run(loop, UV_RUN_DEFAULT);

    // Cleanup
    uv_loop_close(loop);
    return 0;
}
```

## Integration with Existing Application

If your application already has a libuv event loop with other handlers:

```cpp
#include <uv.h>
#include "libuv_event_loop.hpp"
#include "pipeline.hpp"
#include "live_protocol.hpp"

class MyApplication {
public:
    MyApplication()
        : loop_(uv_default_loop())
        , event_loop_(loop_)
    {
        event_loop_.SetEventLoopThread();

        // Set up your existing libuv handlers
        SetupTimers();
        SetupSignals();
        SetupOtherIO();
    }

    void AddDatabentoStream(const std::string& api_key) {
        using LivePipeline = dbn_pipe::Pipeline<
            dbn_pipe::LiveProtocol,
            databento::Record>;

        pipeline_ = LivePipeline::Create(event_loop_, api_key);

        pipeline_->SetRequest(dbn_pipe::LiveRequest{
            .dataset = "GLBX.MDP3",
            .symbols = "ES.FUT",
            .schema = databento::Schema::Trades,
            .stype_in = databento::SType::Parent
        });

        pipeline_->OnRecord([this](const databento::Record& r) {
            HandleMarketData(r);
        });

        pipeline_->OnError([this](const dbn_pipe::Error& e) {
            HandleError(e);
        });

        pipeline_->Connect();
        pipeline_->Start();
    }

    void Run() {
        uv_run(loop_, UV_RUN_DEFAULT);
    }

private:
    void SetupTimers() { /* ... */ }
    void SetupSignals() { /* ... */ }
    void SetupOtherIO() { /* ... */ }

    void HandleMarketData(const databento::Record& r) {
        // Process market data alongside other application logic
    }

    void HandleError(const dbn_pipe::Error& e) {
        // Handle errors
    }

    uv_loop_t* loop_;
    dbn_pipe::LibuvEventLoop event_loop_;
    std::shared_ptr<dbn_pipe::Pipeline<
        dbn_pipe::LiveProtocol,
        databento::Record>> pipeline_;
};

int main() {
    MyApplication app;
    app.AddDatabentoStream("your-api-key");
    app.Run();
    return 0;
}
```

## Thread Safety Notes

1. **Defer() is thread-safe**: You can call `Defer()` from any thread. It uses `uv_async_send()` which is the only libuv function safe to call from other threads.

2. **All other methods must be called from the event loop thread**: `Register()`, `Update()`, and handle destruction must happen on the event loop thread.

3. **SetEventLoopThread()**: Call this once from your event loop thread (typically at startup) to enable thread-safety assertions.

## Handle Lifecycle

The `LibuvEventHandle` destructor calls `uv_poll_stop()` synchronously. In some edge cases (especially during shutdown), you may need to use `uv_close()` with a callback to ensure proper cleanup:

```cpp
LibuvEventHandle::~LibuvEventHandle() {
    uv_poll_stop(&poll_handle_);
    // For robust cleanup, consider:
    // uv_close(reinterpret_cast<uv_handle_t*>(&poll_handle_), [](uv_handle_t*) {});
    // But this requires the event loop to run another iteration
}
```

## Comparison with EpollEventLoop

| Feature | EpollEventLoop | LibuvEventLoop |
|---------|---------------|----------------|
| Ownership | Owns epoll fd | Non-owning wrapper |
| Poll method | `Poll()` / `Run()` | Uses your `uv_run()` |
| Cross-thread wakeup | eventfd | `uv_async_t` |
| Platform | Linux only | Cross-platform (via libuv) |

Choose `EpollEventLoop` when you want databento-async to manage the event loop. Choose `LibuvEventLoop` when integrating into an existing libuv-based application.
