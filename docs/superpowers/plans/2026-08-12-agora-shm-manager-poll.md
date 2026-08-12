# Agora SHM Manager Poll Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a callback-compatible polling receive mode that buffers the latest 200 ms of audio or the latest complete video frame per SHM read entry.

**Architecture:** Keep the existing callback path unchanged when `on_frame` is non-NULL. When it is NULL, lazily initialize per-read-entry media state and storage from the first stable SHM snapshot, update that storage under the existing manager mutex, and expose consuming copies through a new public `ssize_t` poll API.

**Tech Stack:** C11, POSIX shared memory, pthreads, loopback UDP localsocket, Make.

---

## File Map

- Modify `src/agora_shm_manager.h`: expose `ssize_t` and document the poll API.
- Modify `src/agora_shm_manager.c`: add read-entry buffer state, receive buffering, cleanup, and poll consumption.
- Create `tests/test_agora_shm_manager_poll.c`: exercise polling behavior through public manager APIs and real IPC/signaling.
- Modify `Makefile`: build and run the new test binary.
- Modify `README.md`: document callback versus poll startup and consumption.

### Task 1: Public API Contract and Failing Test Harness

**Files:**
- Modify: `src/agora_shm_manager.h`
- Create: `tests/test_agora_shm_manager_poll.c`
- Modify: `Makefile`

- [ ] **Step 1: Add a compile-time public API test**

Create the test harness with a function-pointer assignment that requires the exact API:

```c
static ssize_t (*const poll_api)(AgoraShmManager *, const char *,
                                 AgoraShmIpcHeader *, char *, size_t) =
    agora_shm_manager_poll;
```

Add helpers that reserve a loopback UDP port, start a poll-mode receiver and a writer manager, retry poll until an expected entry/data state appears, and clean up SHM names after every assertion path.

- [ ] **Step 2: Add the test target and verify compilation fails**

Add `build/test_agora_shm_manager_poll` and a `test` phony target to `Makefile`, linking `agora_shm_ipc.o`, `agora_localsock.o`, and `agora_shm_manager.o`.

Run:

```bash
make test
```

Expected: compilation fails because `agora_shm_manager_poll` is undeclared and a NULL callback is not yet accepted at runtime.

- [ ] **Step 3: Declare the public API**

Add `<sys/types.h>` and this documented declaration:

```c
ssize_t agora_shm_manager_poll(AgoraShmManager *m, const char *shm_name,
                               AgoraShmIpcHeader *header, char *out,
                               size_t max_payload_size);
```

Document positive byte counts, empty `0`, and `EINVAL`, `ENOTSUP`, `ENOENT`, and `ENOBUFS` failures.

- [ ] **Step 4: Rebuild to expose the missing implementation**

Run:

```bash
make test
```

Expected: link failure for undefined `agora_shm_manager_poll`.

### Task 2: Read-Entry Poll Storage and Receive Algorithms

**Files:**
- Modify: `src/agora_shm_manager.c`
- Test: `tests/test_agora_shm_manager_poll.c`

- [ ] **Step 1: Add failing audio and video receive tests**

Exercise these observable cases:

```c
/* Audio: write sequential byte blocks, then assert poll returns them FIFO. */
/* Audio overflow: write more than 19,200 total bytes, then assert only the
 * newest 19,200 bytes remain. */
/* Video: write frame A then frame B before polling, then assert only B is
 * returned. */
/* Duplicate signal: resend WRITECMD without another SHM write and assert no
 * second copy is buffered. */
```

For every positive poll, assert `header.seq` is the newest accepted sequence and
`header.data_len == (uint32_t)return_value`.

- [ ] **Step 2: Run tests and verify receive behavior fails**

Run:

```bash
make test
```

Expected: poll-mode start or buffering assertions fail.

- [ ] **Step 3: Add entry state and cleanup**

Extend `AgoraShmManagerReadEntry` with locked media type, allocated buffer,
capacity/current length, latest accepted sequence validity/value, and latest
header. Add an audio capacity constant equal to `48u * 2u * 200u`.

Update `clear_read_entry` to `free(e->buffer)` before zeroing the entry. Do not
allocate any polling buffer when `m->on_frame != NULL`.

- [ ] **Step 4: Implement poll-mode frame acceptance**

After a successful stable read and while holding `m->lock`:

```c
if (m->on_frame != NULL) {
  /* Preserve existing unlock-then-callback path. */
} else {
  /* Validate/lock media type from snap.media_type. */
  /* Lazily malloc 19,200 bytes for audio or m->read_cap for video. */
  /* Ignore a sequence equal to the last successfully accepted sequence. */
  /* Append/roll audio or replace video. */
  /* Store the full latest header and sequence only after acceptance. */
}
```

Audio overflow discards exactly `current_len + incoming_len - capacity` oldest
bytes. An incoming block at least as large as capacity retains its final 19,200
bytes. Video copies a complete frame and never damages the old frame if a
defensive capacity check fails.

- [ ] **Step 5: Allow NULL callback startup and run receive tests**

Remove only `on_frame == NULL` from `agora_shm_manager_start` argument rejection.

Run:

```bash
make test
```

Expected: receive setup succeeds; tests advance to failures caused by the still
missing poll consumption implementation.

### Task 3: Poll Consumption and Error Semantics

**Files:**
- Modify: `src/agora_shm_manager.c`
- Test: `tests/test_agora_shm_manager_poll.c`

- [ ] **Step 1: Add failing contract tests**

Cover:

```c
/* NULL/empty arguments and zero capacity => -1/EINVAL. */
/* A callback-mode manager => -1/ENOTSUP. */
/* An unknown shm_name => -1/ENOENT. */
/* An attached entry with no buffered data => 0 and unchanged outputs. */
/* Partial audio poll consumes a prefix and preserves/moves the remainder. */
/* Undersized video poll => -1/ENOBUFS and the next large poll gets the frame. */
```

- [ ] **Step 2: Run the focused binary and verify failures**

Run:

```bash
make build/test_agora_shm_manager_poll
./build/test_agora_shm_manager_poll
```

Expected: contract assertions fail until the implementation exists.

- [ ] **Step 3: Implement `agora_shm_manager_poll`**

Validate arguments, reject callback mode, then hold `m->lock` through entry
lookup, copies, and state updates. Return zero without touching outputs for an
empty entry. For audio, copy `min(data_len, max_payload_size)` and `memmove` the
remainder. For video, fail atomically with `ENOBUFS` unless the whole frame fits.
On successful consumption, snapshot `latest_header`, overwrite its `data_len`
with the copied size, then return that size as `ssize_t`.

Guard the conversion by ensuring a copied size is not greater than
`(size_t)SSIZE_MAX`; return `-1/EOVERFLOW` without consuming data if it is.

- [ ] **Step 4: Run the full test target**

Run:

```bash
make test
```

Expected: all poll API tests pass.

### Task 4: Compatibility, Documentation, and Final Verification

**Files:**
- Modify: `README.md`
- Test: `tests/test_agora_shm_manager_poll.c`

- [ ] **Step 1: Verify callback compatibility in the test**

Start a receiver with a non-NULL callback, deliver a frame, and assert the
callback receives the expected bytes and stable header. Assert polling that
manager returns `-1/ENOTSUP`.

- [ ] **Step 2: Document both receive modes**

Update the manager section in `README.md` with the exact poll signature and a
short caller-owned-buffer example. State that audio polls may return partial
aggregates, video polls require full-frame capacity, empty returns zero, and
the returned header's `data_len` equals the returned byte count.

- [ ] **Step 3: Run formatting and static build checks**

Run:

```bash
clang-format -i src/agora_shm_manager.c src/agora_shm_manager.h \
  tests/test_agora_shm_manager_poll.c
make clean
make all
make test
git diff --check
```

Expected: all builds/tests exit zero, compiler emits no warnings, and diff check
prints nothing.

- [ ] **Step 4: Review the final diff for scope and compatibility**

Run:

```bash
git diff -- src/agora_shm_manager.c src/agora_shm_manager.h \
  tests/test_agora_shm_manager_poll.c Makefile README.md
git status --short
```

Expected: only the planned implementation, test, Makefile, README, and plan
changes are present; the unrelated untracked `sip_0420.md` remains untouched.
