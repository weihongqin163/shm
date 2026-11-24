# SHM Worker Recovery Implementation Plan

**Goal:** Keep workers alive until close and recover socket failures without
recreating the manager or its SHM objects.

**Architecture:** Follow ../specs/2026-11-24-shm-worker-recovery-design.md.
Serialize nonblocking transport calls separately from SHM operations, and use
interruptible condition waits for healthy idle and error delays.

**Tech Stack:** C11 atomics, POSIX UDP/SHM, pthread mutexes/conditions, Make.

### Task 1: Regression harness

- [x] Add tests/test_agora_shm_manager_recovery.c using private source inclusion
  and wrapper functions for localsock poll/create/keepalive. Do not expose test
  hooks in the public library. Link IPC/localsock/state but not manager object.
- [x] Add a Makefile test target. Prove a single injected EIO leaves the old
  worker unable to deliver later notifications; use a bounded timeout.
- [x] Test error sequences, reconstruction failure, recoverable API results,
  retained ownership and concurrent operations using condition-based polling.

### Task 2: Transport error detail

- [x] Add private poll event access for diagnostics, preserving errno.
- [x] Map POLLNVAL to EBADF, inspect SO_ERROR for POLLERR, handle POLLHUP.
- [x] Make client socket nonblocking and preserve errors across cleanup.

### Task 3: Manager recovery

- [x] Store original port/server settings; initialize transport/wait locks and
  condition; allocate receive storage before worker creation. Unwind every
  partially initialized resource on start failure.
- [x] Replace worker error exits with shared failure/recovery helpers. Use
  `recovering=1`, a consecutive-error counter and rebuild requests. Preserve
  successful callbacks and all read/write entries across reconstruction.
- [x] Enforce `20ms` failure waits and `3` consecutive errors; invalid sockets
  request immediate reconstruction. Successful polling clears failures.
- [x] Serialize notification sends with poll/replacement, return -EAGAIN while
  recovering, and schedule rebuild for an invalid socket discovered by sends.
- [x] Consume available poll cache before checking recovering; otherwise return
  -EAGAIN during recovery. Keep existing negative errno contract.
- [x] Wake condition waits on close and destroy synchronization after joining.

### Task 4: Validation and documentation

- [x] Run `make all test` with local socket/SHM permissions.
- [x] Run recovery tests under AddressSanitizer/UndefinedBehaviorSanitizer.
- [x] Review recovery/lock ownership, start unwinding and close interleavings.
- [x] Update manager header, README and manager design description with exact
  return semantics, fixed delays and notification-loss boundary.
- [x] Run `git diff --check`, inspect the final diff and report validation.

Validation: `make all` and `make test` passed locally on macOS. All four test
programs passed, including 22 recovery scenarios. ASan/UBSan recovery tests
passed with leak detection disabled (macOS). Independent review reported no
remaining correctness findings after switching retry waits to monotonic time.
