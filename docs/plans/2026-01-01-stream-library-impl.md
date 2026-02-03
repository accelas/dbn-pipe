# Stream Library Extraction Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Extract generic streaming components from src/ into lib/stream/ for reuse.

**Architecture:** Move files with minimal changes; update include paths and Bazel deps. Merge epoll_event_loop into reactor since they're tightly coupled.

**Tech Stack:** C++23, Bazel, GoogleTest

---

## Task 1: Create lib/stream directory and BUILD.bazel skeleton

**Files:**
- Create: `lib/stream/BUILD.bazel`

**Step 1: Create directory**

Run: `mkdir -p lib/stream`

**Step 2: Create BUILD.bazel with error library**

```python
# lib/stream/BUILD.bazel
load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "error",
    hdrs = ["error.hpp"],
)
```

**Step 3: Build to verify syntax**

Run: `bazel build //lib/stream:error`
Expected: ERROR (file not found - that's okay, BUILD syntax is valid)

**Step 4: Commit**

```bash
git add lib/stream/BUILD.bazel
git commit -m "chore: create lib/stream directory with BUILD skeleton"
```

---

## Task 2: Move error.hpp to lib/stream

**Files:**
- Move: `src/error.hpp` → `lib/stream/error.hpp`
- Modify: `lib/stream/BUILD.bazel`
- Modify: `src/BUILD.bazel`

**Step 1: Copy error.hpp**

Run: `cp src/error.hpp lib/stream/error.hpp`

**Step 2: Build lib/stream:error**

Run: `bazel build //lib/stream:error`
Expected: SUCCESS

**Step 3: Update src/BUILD.bazel - change error to depend on lib/stream**

Replace the `error` target in `src/BUILD.bazel`:

```python
cc_library(
    name = "error",
    hdrs = ["error.hpp"],
    visibility = ["//visibility:public"],
    deps = ["//lib/stream:error"],
)
```

Wait - this creates a cycle. Instead, we need to delete src/error.hpp and have src targets depend on lib/stream:error directly. But that requires updating many deps at once.

**Alternative approach:** Keep src:error as an alias/re-export for now.

Actually, let's just update the header path in src/error.hpp to re-export:

```cpp
// src/error.hpp - Re-export from lib/stream
#pragma once
#include "dbn_pipe/stream/error.hpp"
```

**Step 3: Update src/error.hpp to re-export**

Replace contents of `src/error.hpp` with:
```cpp
// src/error.hpp - Re-export from lib/stream
#pragma once
#include "dbn_pipe/stream/error.hpp"
```

**Step 4: Update src/BUILD.bazel error target**

```python
cc_library(
    name = "error",
    hdrs = ["error.hpp"],
    visibility = ["//visibility:public"],
    deps = ["//lib/stream:error"],
)
```

**Step 5: Run error tests**

Run: `bazel test //tests:error_test`
Expected: PASS

**Step 6: Commit**

```bash
git add lib/stream/error.hpp src/error.hpp src/BUILD.bazel lib/stream/BUILD.bazel
git commit -m "refactor: move error.hpp to lib/stream"
```

---

## Task 3: Move buffer_chain.hpp to lib/stream

**Files:**
- Move: `src/buffer_chain.hpp` → `lib/stream/buffer_chain.hpp`
- Modify: `src/buffer_chain.hpp` (re-export)
- Modify: `lib/stream/BUILD.bazel`
- Modify: `src/BUILD.bazel`

**Step 1: Copy buffer_chain.hpp**

Run: `cp src/buffer_chain.hpp lib/stream/buffer_chain.hpp`

**Step 2: Add to lib/stream/BUILD.bazel**

Add after error:
```python
cc_library(
    name = "buffer_chain",
    hdrs = ["buffer_chain.hpp"],
)
```

**Step 3: Build lib/stream:buffer_chain**

Run: `bazel build //lib/stream:buffer_chain`
Expected: SUCCESS

**Step 4: Update src/buffer_chain.hpp to re-export**

```cpp
// src/buffer_chain.hpp - Re-export from lib/stream
#pragma once
#include "dbn_pipe/stream/buffer_chain.hpp"
```

**Step 5: Update src/BUILD.bazel buffer_chain target**

```python
cc_library(
    name = "buffer_chain",
    hdrs = ["buffer_chain.hpp"],
    visibility = ["//visibility:public"],
    deps = ["//lib/stream:buffer_chain"],
)
```

**Step 6: Run buffer_chain tests**

Run: `bazel test //tests:buffer_chain_test`
Expected: PASS

**Step 7: Commit**

```bash
git add lib/stream/buffer_chain.hpp src/buffer_chain.hpp src/BUILD.bazel lib/stream/BUILD.bazel
git commit -m "refactor: move buffer_chain.hpp to lib/stream"
```

---

## Task 4: Move event_loop.hpp/.cpp to lib/stream

**Files:**
- Move: `src/event_loop.hpp` → `lib/stream/event_loop.hpp`
- Move: `src/event_loop.cpp` → `lib/stream/event_loop.cpp`
- Move: `src/epoll_event_loop.hpp` → `lib/stream/epoll_event_loop.hpp`
- Move: `src/epoll_event_loop.cpp` → `lib/stream/epoll_event_loop.cpp`
- Modify: `lib/stream/BUILD.bazel`
- Modify: `src/BUILD.bazel`

**Step 1: Copy event loop files**

Run:
```bash
cp src/event_loop.hpp lib/stream/
cp src/event_loop.cpp lib/stream/
cp src/epoll_event_loop.hpp lib/stream/
cp src/epoll_event_loop.cpp lib/stream/
```

**Step 2: Add to lib/stream/BUILD.bazel**

```python
# Internal epoll implementation
cc_library(
    name = "epoll_event_loop_internal",
    srcs = ["epoll_event_loop.cpp"],
    hdrs = ["epoll_event_loop.hpp", "event_loop.hpp"],
)

# Public event loop interface + type-erased wrapper
cc_library(
    name = "event_loop",
    srcs = ["event_loop.cpp"],
    hdrs = ["event_loop.hpp"],
    deps = [":epoll_event_loop_internal"],
)

# For tests that need direct access to EpollEventLoop
cc_library(
    name = "epoll_event_loop",
    hdrs = ["epoll_event_loop.hpp"],
    deps = [":epoll_event_loop_internal"],
)
```

**Step 3: Build lib/stream:event_loop**

Run: `bazel build //lib/stream:event_loop`
Expected: SUCCESS

**Step 4: Update src/event_loop.hpp to re-export**

```cpp
// src/event_loop.hpp - Re-export from lib/stream
#pragma once
#include "dbn_pipe/stream/event_loop.hpp"
```

**Step 5: Update src/epoll_event_loop.hpp to re-export**

```cpp
// src/epoll_event_loop.hpp - Re-export from lib/stream
#pragma once
#include "dbn_pipe/stream/epoll_event_loop.hpp"
```

**Step 6: Update src/BUILD.bazel event loop targets**

Replace the event loop targets:
```python
cc_library(
    name = "event_loop",
    hdrs = ["event_loop.hpp"],
    visibility = ["//visibility:public"],
    deps = ["//lib/stream:event_loop"],
)

cc_library(
    name = "epoll_event_loop_internal",
    hdrs = ["epoll_event_loop.hpp", "event_loop.hpp"],
    visibility = ["//tests:__pkg__"],
    deps = ["//lib/stream:epoll_event_loop_internal"],
)

cc_library(
    name = "epoll_event_loop",
    hdrs = ["epoll_event_loop.hpp"],
    visibility = ["//tests:__pkg__"],
    deps = ["//lib/stream:epoll_event_loop"],
)
```

**Step 7: Delete src/event_loop.cpp and src/epoll_event_loop.cpp**

Run:
```bash
rm src/event_loop.cpp src/epoll_event_loop.cpp
```

**Step 8: Run event loop tests**

Run: `bazel test //tests:epoll_event_loop_test`
Expected: PASS

**Step 9: Commit**

```bash
git add lib/stream/event_loop.hpp lib/stream/event_loop.cpp
git add lib/stream/epoll_event_loop.hpp lib/stream/epoll_event_loop.cpp
git add lib/stream/BUILD.bazel
git add src/event_loop.hpp src/epoll_event_loop.hpp src/BUILD.bazel
git rm src/event_loop.cpp src/epoll_event_loop.cpp
git commit -m "refactor: move event_loop to lib/stream"
```

---

## Task 5: Move reactor.hpp/.cpp to lib/stream

**Files:**
- Move: `src/reactor.hpp` → `lib/stream/reactor.hpp`
- Move: `src/reactor.cpp` → `lib/stream/reactor.cpp`
- Modify: `lib/stream/BUILD.bazel`
- Modify: `src/BUILD.bazel`

**Step 1: Copy reactor files**

Run:
```bash
cp src/reactor.hpp lib/stream/
cp src/reactor.cpp lib/stream/
```

**Step 2: Update lib/stream/reactor.hpp include path**

The reactor.hpp includes `"event_loop.hpp"`. Update to:
```cpp
#include "dbn_pipe/stream/event_loop.hpp"
```

Wait - since both are in lib/stream/, we can use relative includes. Check if it uses quotes or angle brackets. If quotes, `"event_loop.hpp"` should work within the same directory.

Actually, Bazel resolves includes from workspace root, so `"event_loop.hpp"` won't work. Need to use `"lib/stream/event_loop.hpp"`.

**Step 2: Update include in lib/stream/reactor.hpp**

Change line:
```cpp
#include "event_loop.hpp"
```
to:
```cpp
#include "dbn_pipe/stream/event_loop.hpp"
```

**Step 3: Add reactor to lib/stream/BUILD.bazel**

```python
cc_library(
    name = "reactor",
    srcs = ["reactor.cpp"],
    hdrs = ["reactor.hpp"],
    deps = [":event_loop"],
)
```

**Step 4: Build lib/stream:reactor**

Run: `bazel build //lib/stream:reactor`
Expected: SUCCESS

**Step 5: Update src/reactor.hpp to re-export**

```cpp
// src/reactor.hpp - Re-export from lib/stream
#pragma once
#include "dbn_pipe/stream/reactor.hpp"
```

**Step 6: Update src/BUILD.bazel reactor target**

```python
cc_library(
    name = "reactor",
    hdrs = ["reactor.hpp"],
    visibility = ["//visibility:public"],
    deps = ["//lib/stream:reactor"],
)
```

**Step 7: Delete src/reactor.cpp**

Run: `rm src/reactor.cpp`

**Step 8: Run reactor tests**

Run: `bazel test //tests:reactor_test`
Expected: PASS

**Step 9: Commit**

```bash
git add lib/stream/reactor.hpp lib/stream/reactor.cpp lib/stream/BUILD.bazel
git add src/reactor.hpp src/BUILD.bazel
git rm src/reactor.cpp
git commit -m "refactor: move reactor to lib/stream"
```

---

## Task 6: Move pipeline_component.hpp to lib/stream/component.hpp

**Files:**
- Move: `src/pipeline_component.hpp` → `lib/stream/component.hpp`
- Modify: `lib/stream/BUILD.bazel`
- Modify: `src/BUILD.bazel`

**Step 1: Copy pipeline_component.hpp**

Run: `cp src/pipeline_component.hpp lib/stream/component.hpp`

**Step 2: Update includes in lib/stream/component.hpp**

Change:
```cpp
#include "error.hpp"
#include "event_loop.hpp"
```
to:
```cpp
#include "dbn_pipe/stream/error.hpp"
#include "dbn_pipe/stream/event_loop.hpp"
```

**Step 3: Add to lib/stream/BUILD.bazel**

```python
cc_library(
    name = "component",
    hdrs = ["component.hpp"],
    deps = [
        ":error",
        ":event_loop",
    ],
)
```

**Step 4: Build lib/stream:component**

Run: `bazel build //lib/stream:component`
Expected: SUCCESS

**Step 5: Update src/pipeline_component.hpp to re-export**

```cpp
// src/pipeline_component.hpp - Re-export from lib/stream
#pragma once
#include "dbn_pipe/stream/component.hpp"
```

**Step 6: Update src/BUILD.bazel pipeline_component target**

```python
cc_library(
    name = "pipeline_component",
    hdrs = ["pipeline_component.hpp"],
    visibility = ["//visibility:public"],
    deps = ["//lib/stream:component"],
)
```

**Step 7: Run pipeline_component tests**

Run: `bazel test //tests:pipeline_component_test`
Expected: PASS

**Step 8: Commit**

```bash
git add lib/stream/component.hpp lib/stream/BUILD.bazel
git add src/pipeline_component.hpp src/BUILD.bazel
git commit -m "refactor: move pipeline_component to lib/stream/component"
```

---

## Task 7: Move tcp_socket.hpp to lib/stream

**Files:**
- Move: `src/tcp_socket.hpp` → `lib/stream/tcp_socket.hpp`
- Modify: `lib/stream/BUILD.bazel`
- Modify: `src/BUILD.bazel`

**Step 1: Copy tcp_socket.hpp**

Run: `cp src/tcp_socket.hpp lib/stream/tcp_socket.hpp`

**Step 2: Update includes in lib/stream/tcp_socket.hpp**

Change:
```cpp
#include "buffer_chain.hpp"
#include "error.hpp"
#include "pipeline_component.hpp"
#include "event_loop.hpp"
```
to:
```cpp
#include "dbn_pipe/stream/buffer_chain.hpp"
#include "dbn_pipe/stream/error.hpp"
#include "dbn_pipe/stream/component.hpp"
#include "dbn_pipe/stream/event_loop.hpp"
```

**Step 3: Add to lib/stream/BUILD.bazel**

```python
cc_library(
    name = "tcp_socket",
    hdrs = ["tcp_socket.hpp"],
    deps = [
        ":buffer_chain",
        ":component",
        ":error",
        ":event_loop",
    ],
)
```

**Step 4: Build lib/stream:tcp_socket**

Run: `bazel build //lib/stream:tcp_socket`
Expected: SUCCESS

**Step 5: Update src/tcp_socket.hpp to re-export**

```cpp
// src/tcp_socket.hpp - Re-export from lib/stream
#pragma once
#include "dbn_pipe/stream/tcp_socket.hpp"
```

**Step 6: Update src/BUILD.bazel tcp_socket target**

```python
cc_library(
    name = "tcp_socket",
    hdrs = ["tcp_socket.hpp"],
    visibility = ["//visibility:public"],
    deps = ["//lib/stream:tcp_socket"],
)
```

**Step 7: Run tcp_socket tests**

Run: `bazel test //tests:tcp_socket_test`
Expected: PASS

**Step 8: Commit**

```bash
git add lib/stream/tcp_socket.hpp lib/stream/BUILD.bazel
git add src/tcp_socket.hpp src/BUILD.bazel
git commit -m "refactor: move tcp_socket to lib/stream"
```

---

## Task 8: Create src/stream.hpp re-export header

**Files:**
- Create: `src/stream.hpp`
- Modify: `src/BUILD.bazel`

**Step 1: Create src/stream.hpp**

```cpp
// src/stream.hpp - Re-export all lib/stream components
#pragma once

#include "dbn_pipe/stream/buffer_chain.hpp"
#include "dbn_pipe/stream/component.hpp"
#include "dbn_pipe/stream/error.hpp"
#include "dbn_pipe/stream/event_loop.hpp"
#include "dbn_pipe/stream/reactor.hpp"
#include "dbn_pipe/stream/tcp_socket.hpp"
```

**Step 2: Add to src/BUILD.bazel**

```python
cc_library(
    name = "stream",
    hdrs = ["stream.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//lib/stream:buffer_chain",
        "//lib/stream:component",
        "//lib/stream:error",
        "//lib/stream:event_loop",
        "//lib/stream:reactor",
        "//lib/stream:tcp_socket",
    ],
)
```

**Step 3: Build src:stream**

Run: `bazel build //src:stream`
Expected: SUCCESS

**Step 4: Commit**

```bash
git add src/stream.hpp src/BUILD.bazel
git commit -m "feat: add src/stream.hpp re-export header"
```

---

## Task 9: Run all tests

**Step 1: Run all tests**

Run: `bazel test //tests/...`
Expected: All tests PASS

**Step 2: If failures, debug and fix**

Common issues:
- Include path mismatches
- Missing deps in BUILD.bazel
- Namespace issues

**Step 3: Commit any fixes**

```bash
git add -A
git commit -m "fix: resolve test failures after lib/stream extraction"
```

---

## Task 10: Update docs/libuv-integration.md

**Files:**
- Modify: `docs/libuv-integration.md`

**Step 1: Update include paths**

Change:
```cpp
#include "event_loop.hpp"
```
to:
```cpp
#include "dbn_pipe/stream.hpp"
```

**Step 2: Commit**

```bash
git add docs/libuv-integration.md
git commit -m "docs: update libuv integration for lib/stream"
```

---

## Task 11: Final cleanup and verification

**Step 1: Run full build**

Run: `bazel build //...`
Expected: SUCCESS

**Step 2: Run all tests**

Run: `bazel test //...`
Expected: All PASS

**Step 3: Clean up any leftover files**

Check for orphaned .cpp files in src/ that should have been removed.

**Step 4: Final commit if needed**

```bash
git add -A
git commit -m "chore: final cleanup after lib/stream extraction"
```

---

## Summary

After completion:
- `lib/stream/` contains: error.hpp, buffer_chain.hpp, event_loop.hpp/.cpp, epoll_event_loop.hpp/.cpp, reactor.hpp/.cpp, component.hpp, tcp_socket.hpp
- `src/` files re-export from lib/stream for backward compatibility
- All tests pass
- Documentation updated
