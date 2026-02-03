# Merge Reactor into EpollEventLoop Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove code duplication by merging Reactor features into EpollEventLoop and deleting Reactor.

**Architecture:** Keep EpollEventLoop as the single epoll implementation. Add Event/Timer as standalone classes using IEventLoop& interface (portable across backends). Add Wake() with eventfd to EpollEventLoop for cross-thread Defer() wakeup.

**Tech Stack:** C++20, epoll, eventfd, timerfd, gtest

---

### Task 1: Add Wake() to EpollEventLoop

**Files:**
- Modify: `lib/stream/epoll_event_loop.hpp`
- Modify: `lib/stream/epoll_event_loop.cpp`
- Test: `tests/epoll_event_loop_test.cpp`

**Step 1: Write the failing test**

Add to `tests/epoll_event_loop_test.cpp`:

```cpp
TEST(EpollEventLoopTest, DeferFromOtherThreadWakesLoop) {
    EpollEventLoop loop;

    std::atomic<bool> callback_called{false};
    std::thread other_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        loop.Defer([&]() { callback_called = true; });
    });

    // Poll with long timeout - should wake up when Defer() is called
    auto start = std::chrono::steady_clock::now();
    loop.Poll(1000);  // 1 second timeout
    auto elapsed = std::chrono::steady_clock::now() - start;

    other_thread.join();

    EXPECT_TRUE(callback_called);
    // Should have woken up quickly, not waited full second
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 100);
}
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:epoll_event_loop_test --test_filter=EpollEventLoopTest.DeferFromOtherThreadWakesLoop`
Expected: FAIL (callback takes ~1 second, not <100ms)

**Step 3: Add wake_fd_ member to header**

In `lib/stream/epoll_event_loop.hpp`, add to private section of `EpollEventLoop`:

```cpp
    int wake_fd_ = -1;  // eventfd for cross-thread wakeup
```

Add public method:

```cpp
    void Wake();  // Wake event loop from another thread
```

**Step 4: Implement Wake() and update constructor/destructor**

In `lib/stream/epoll_event_loop.cpp`:

Constructor - add after `epoll_fd_` creation:

```cpp
    // Create eventfd for cross-thread wakeup
    wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0) {
        close(epoll_fd_);
        throw std::runtime_error("Failed to create eventfd");
    }

    // Register wake_fd_ with epoll
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = nullptr;  // nullptr signals this is wake_fd_
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev) < 0) {
        close(wake_fd_);
        close(epoll_fd_);
        throw std::runtime_error("Failed to register wake_fd");
    }
```

Destructor - add before closing `epoll_fd_`:

```cpp
    if (wake_fd_ >= 0) {
        close(wake_fd_);
    }
```

Add `Wake()` method:

```cpp
void EpollEventLoop::Wake() {
    uint64_t val = 1;
    [[maybe_unused]] auto _ = write(wake_fd_, &val, sizeof(val));
}
```

Update `Poll()` - in the event handling loop, handle wake_fd_:

```cpp
    // Handle wake_fd_ (data.ptr == nullptr)
    if (events_[i].data.ptr == nullptr) {
        uint64_t val;
        [[maybe_unused]] auto _ = read(wake_fd_, &val, sizeof(val));
        continue;
    }
```

Update `Defer()` - after queueing callback:

```cpp
    if (!IsInEventLoopThread()) {
        Wake();
    }
```

**Step 5: Run test to verify it passes**

Run: `bazel test //tests:epoll_event_loop_test --test_filter=EpollEventLoopTest.DeferFromOtherThreadWakesLoop`
Expected: PASS

**Step 6: Commit**

```bash
git add lib/stream/epoll_event_loop.hpp lib/stream/epoll_event_loop.cpp tests/epoll_event_loop_test.cpp
git commit -m "feat(event_loop): add Wake() for cross-thread Defer() wakeup"
```

---

### Task 2: Create Event Class

**Files:**
- Create: `lib/stream/event.hpp`
- Test: `tests/event_test.cpp`
- Modify: `lib/stream/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write the failing test**

Create `tests/event_test.cpp`:

```cpp
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "dbn_pipe/stream/epoll_event_loop.hpp"
#include "dbn_pipe/stream/event.hpp"

using namespace dbn_pipe;

TEST(EventTest, CallbackOnReadable) {
    EpollEventLoop loop;

    int efd = eventfd(0, EFD_NONBLOCK);
    ASSERT_GE(efd, 0);

    bool called = false;
    Event event(loop, efd, true, false);
    event.OnEvent([&](uint32_t) { called = true; });

    // Write to make readable
    uint64_t val = 1;
    write(efd, &val, sizeof(val));

    loop.Poll(0);
    EXPECT_TRUE(called);

    close(efd);
}

TEST(EventTest, UnregistersOnDestruction) {
    EpollEventLoop loop;

    int efd = eventfd(0, EFD_NONBLOCK);
    ASSERT_GE(efd, 0);

    bool called = false;
    {
        Event event(loop, efd, false, true);
        event.OnEvent([&](uint32_t) { called = true; });

        loop.Poll(0);
        EXPECT_TRUE(called);
        called = false;
    }

    // After destruction, no callback
    loop.Poll(0);
    EXPECT_FALSE(called);

    close(efd);
}

TEST(EventTest, Update) {
    EpollEventLoop loop;

    int efd = eventfd(0, EFD_NONBLOCK);
    ASSERT_GE(efd, 0);

    int call_count = 0;
    Event event(loop, efd, true, false);  // Read only
    event.OnEvent([&](uint32_t) { call_count++; });

    loop.Poll(0);
    EXPECT_EQ(call_count, 0);  // Not readable

    event.Update(false, true);  // Switch to write
    loop.Poll(0);
    EXPECT_EQ(call_count, 1);  // eventfd always writable

    close(efd);
}
```

**Step 2: Add BUILD target for test**

In `tests/BUILD.bazel`, add:

```bazel
cc_test(
    name = "event_test",
    srcs = ["event_test.cpp"],
    deps = [
        "//lib/stream:epoll_event_loop",
        "//lib/stream:event",
        "@googletest//:gtest_main",
    ],
)
```

**Step 3: Run test to verify it fails**

Run: `bazel test //tests:event_test`
Expected: FAIL (event.hpp not found)

**Step 4: Create Event header**

Create `lib/stream/event.hpp`:

```cpp
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "dbn_pipe/stream/event_loop.hpp"

namespace dbn_pipe {

// Event wraps fd registration with IEventLoop
// Portable - works with any IEventLoop implementation
class Event {
public:
    using Callback = std::function<void(uint32_t events)>;

    Event(IEventLoop& loop, int fd, bool want_read, bool want_write)
        : fd_(fd) {
        handle_ = loop.Register(
            fd, want_read, want_write,
            [this]() { if (callback_) callback_(EPOLLIN); },
            [this]() { if (callback_) callback_(EPOLLOUT); },
            [this](int err) { if (callback_) callback_(EPOLLERR); (void)err; }
        );
    }

    ~Event() = default;

    // Non-copyable, non-movable
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&&) = delete;
    Event& operator=(Event&&) = delete;

    void OnEvent(Callback cb) { callback_ = std::move(cb); }

    void Update(bool want_read, bool want_write) {
        handle_->Update(want_read, want_write);
    }

    int fd() const { return fd_; }

private:
    int fd_;
    std::unique_ptr<IEventHandle> handle_;
    Callback callback_;
};

}  // namespace dbn_pipe
```

**Step 5: Add BUILD target for library**

In `lib/stream/BUILD.bazel`, add:

```bazel
cc_library(
    name = "event",
    hdrs = ["event.hpp"],
    deps = [":event_loop"],
)
```

**Step 6: Run test to verify it passes**

Run: `bazel test //tests:event_test`
Expected: PASS

**Step 7: Commit**

```bash
git add lib/stream/event.hpp lib/stream/BUILD.bazel tests/event_test.cpp tests/BUILD.bazel
git commit -m "feat(event): add portable Event class using IEventLoop"
```

---

### Task 3: Create Timer Class

**Files:**
- Create: `lib/stream/timer.hpp`
- Test: `tests/timer_test.cpp`
- Modify: `lib/stream/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write the failing test**

Create `tests/timer_test.cpp`:

```cpp
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "dbn_pipe/stream/epoll_event_loop.hpp"
#include "dbn_pipe/stream/timer.hpp"

using namespace dbn_pipe;

TEST(TimerTest, OneShotFires) {
    EpollEventLoop loop;

    Timer timer(loop);

    bool fired = false;
    timer.OnTimer([&]() {
        fired = true;
        loop.Stop();
    });

    timer.Start(10);  // 10ms one-shot
    EXPECT_TRUE(timer.IsArmed());

    loop.Run();

    EXPECT_TRUE(fired);
}

TEST(TimerTest, RepeatingFires) {
    EpollEventLoop loop;

    Timer timer(loop);

    int count = 0;
    timer.OnTimer([&]() {
        count++;
        if (count >= 3) {
            loop.Stop();
        }
    });

    timer.Start(10, 10);  // 10ms delay, 10ms interval

    loop.Run();

    EXPECT_GE(count, 3);
}

TEST(TimerTest, StopPreventsCallback) {
    EpollEventLoop loop;

    Timer timer(loop);

    bool fired = false;
    timer.OnTimer([&]() { fired = true; });

    timer.Start(50);  // 50ms
    timer.Stop();
    EXPECT_FALSE(timer.IsArmed());

    loop.Poll(100);  // Wait past when it would have fired

    EXPECT_FALSE(fired);
}
```

**Step 2: Add BUILD target for test**

In `tests/BUILD.bazel`, add:

```bazel
cc_test(
    name = "timer_test",
    srcs = ["timer_test.cpp"],
    deps = [
        "//lib/stream:epoll_event_loop",
        "//lib/stream:timer",
        "@googletest//:gtest_main",
    ],
)
```

**Step 3: Run test to verify it fails**

Run: `bazel test //tests:timer_test`
Expected: FAIL (timer.hpp not found)

**Step 4: Create Timer header**

Create `lib/stream/timer.hpp`:

```cpp
// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include "dbn_pipe/stream/event_loop.hpp"

namespace dbn_pipe {

// Timer using IEventLoop::Schedule()
// Portable - works with any IEventLoop implementation
class Timer {
public:
    using Callback = std::function<void()>;

    explicit Timer(IEventLoop& loop) : loop_(loop) {}

    ~Timer() { Stop(); }

    // Non-copyable, non-movable
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    Timer(Timer&&) = delete;
    Timer& operator=(Timer&&) = delete;

    void OnTimer(Callback cb) { callback_ = std::move(cb); }

    void Start(int delay_ms, int interval_ms = 0) {
        interval_ms_ = interval_ms;
        armed_ = true;
        ScheduleNext(delay_ms);
    }

    void Stop() {
        armed_ = false;
        interval_ms_ = 0;
    }

    bool IsArmed() const { return armed_; }

private:
    void ScheduleNext(int delay_ms) {
        // Prevent leaking scheduled callbacks after this Timer is destroyed
        // Use weak_ptr pattern via shared_ptr + flag
        loop_.Schedule(std::chrono::milliseconds(delay_ms), [this]() {
            Fire();
        });
    }

    void Fire() {
        if (!armed_) return;  // Stopped before callback executed

        if (callback_) {
            callback_();
        }

        // Re-schedule for repeating timer
        if (armed_ && interval_ms_ > 0) {
            ScheduleNext(interval_ms_);
        } else {
            armed_ = false;
        }
    }

    IEventLoop& loop_;
    Callback callback_;
    int interval_ms_ = 0;
    bool armed_ = false;
};

}  // namespace dbn_pipe
```

**Step 5: Add BUILD target for library**

In `lib/stream/BUILD.bazel`, add:

```bazel
cc_library(
    name = "timer",
    hdrs = ["timer.hpp"],
    deps = [":event_loop"],
)
```

**Step 6: Run test to verify it passes**

Run: `bazel test //tests:timer_test`
Expected: PASS

**Step 7: Commit**

```bash
git add lib/stream/timer.hpp lib/stream/BUILD.bazel tests/timer_test.cpp tests/BUILD.bazel
git commit -m "feat(timer): add portable Timer class using IEventLoop"
```

---

### Task 4: Update reactor_test.cpp to use new classes

**Files:**
- Modify: `tests/reactor_test.cpp`
- Modify: `tests/BUILD.bazel`

**Step 1: Update test file**

Replace `#include "dbn_pipe/stream/reactor.hpp"` with:

```cpp
#include "dbn_pipe/stream/epoll_event_loop.hpp"
#include "dbn_pipe/stream/event.hpp"
#include "dbn_pipe/stream/timer.hpp"
```

Replace all `Reactor` with `EpollEventLoop`.

Replace `Event event(reactor, efd, EPOLLIN)` with `Event event(reactor, efd, true, false)`.

Replace `event.Modify(EPOLLIN | EPOLLOUT)` with `event.Update(true, true)`.

**Step 2: Update BUILD deps**

In `tests/BUILD.bazel`, update `reactor_test` deps:

```bazel
cc_test(
    name = "reactor_test",
    srcs = ["reactor_test.cpp"],
    deps = [
        "//lib/stream:epoll_event_loop",
        "//lib/stream:event",
        "//lib/stream:timer",
        "@googletest//:gtest_main",
    ],
)
```

**Step 3: Run tests**

Run: `bazel test //tests:reactor_test`
Expected: PASS

**Step 4: Commit**

```bash
git add tests/reactor_test.cpp tests/BUILD.bazel
git commit -m "refactor(tests): migrate reactor_test to EpollEventLoop/Event/Timer"
```

---

### Task 5: Update pipeline_integration_test.cpp

**Files:**
- Modify: `tests/pipeline_integration_test.cpp`
- Modify: `tests/BUILD.bazel`

**Step 1: Update includes and usage**

Replace `#include "dbn_pipe/stream/reactor.hpp"` with:

```cpp
#include "dbn_pipe/stream/epoll_event_loop.hpp"
#include "dbn_pipe/stream/event.hpp"
#include "dbn_pipe/stream/timer.hpp"
```

Replace `Reactor reactor_` with `EpollEventLoop reactor_`.

**Step 2: Update BUILD deps**

Update `pipeline_integration_test` deps to remove `:reactor` and add `:epoll_event_loop`, `:event`, `:timer`.

**Step 3: Run tests**

Run: `bazel test //tests:pipeline_integration_test`
Expected: PASS

**Step 4: Commit**

```bash
git add tests/pipeline_integration_test.cpp tests/BUILD.bazel
git commit -m "refactor(tests): migrate pipeline_integration_test to EpollEventLoop"
```

---

### Task 6: Delete Reactor

**Files:**
- Delete: `lib/stream/reactor.hpp`
- Delete: `lib/stream/reactor.cpp`
- Modify: `lib/stream/BUILD.bazel`

**Step 1: Remove reactor target from BUILD**

In `lib/stream/BUILD.bazel`, delete:

```bazel
cc_library(
    name = "reactor",
    srcs = ["reactor.cpp"],
    hdrs = ["reactor.hpp"],
    deps = [":event_loop"],
)
```

**Step 2: Delete reactor files**

```bash
rm lib/stream/reactor.hpp lib/stream/reactor.cpp
```

**Step 3: Build and test**

Run: `bazel build //lib/stream:all && bazel test //tests:all`
Expected: PASS

**Step 4: Commit**

```bash
git add -A
git commit -m "refactor: remove Reactor (merged into EpollEventLoop + Event + Timer)"
```

---

### Task 7: Fix libuv-integration.md

**Files:**
- Modify: `docs/libuv-integration.md`

**Step 1: Add Schedule() implementation**

In `docs/libuv-integration.md`, add to `LibuvEventLoop` class after `Defer()`:

```cpp
    void Schedule(std::chrono::milliseconds delay, TimerCallback fn) override {
        auto* timer = new uv_timer_t;
        uv_timer_init(loop_, timer);

        struct TimerData {
            TimerCallback callback;
        };
        auto* data = new TimerData{std::move(fn)};
        timer->data = data;

        uv_timer_start(timer, [](uv_timer_t* handle) {
            auto* data = static_cast<TimerData*>(handle->data);
            data->callback();
            delete data;
            uv_timer_stop(handle);
            uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* h) {
                delete reinterpret_cast<uv_timer_t*>(h);
            });
        }, delay.count(), 0);
    }
```

**Step 2: Commit**

```bash
git add docs/libuv-integration.md
git commit -m "docs: add missing Schedule() to libuv integration example"
```

---

### Task 8: Final verification and cleanup

**Step 1: Run all tests**

```bash
bazel test //tests:all
```

Expected: All tests pass

**Step 2: Verify no dangling references to Reactor**

```bash
grep -r "Reactor" lib/ src/ tests/ --include="*.hpp" --include="*.cpp" | grep -v "\.worktrees"
```

Expected: No matches (or only comments)

**Step 3: Commit any final fixes**

If needed.

---

## Verification

```bash
# Full build
bazel build //...

# All tests
bazel test //tests:all

# Specific tests for this change
bazel test //tests:epoll_event_loop_test //tests:event_test //tests:timer_test //tests:reactor_test //tests:pipeline_integration_test
```
