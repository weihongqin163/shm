# Audio Pre-buffer Implementation Plan

> Execute inline with TDD; preserve the existing unrelated untracked documents.

**Goal:** Smooth audio burst writes with timed batches of at most four frames and
increase poll receive retention to 200 ms.

**Architecture:** Per-write-entry copied FIFO, bounded for live speech; one batch
per entry per 10 ms monotonic deadline in the existing worker. Retain SHM ABI,
public signatures, signaling, and synchronous video writes.

**Tech Stack:** C11, pthreads, POSIX SHM, existing Makefile tests.

## Task 1: Regression tests

- [x] Add `tests/test_agora_shm_manager_audio.c`, including manager implementation
  privately as the recovery tests do. Inject a monotonic clock and a one-iteration
  server poll to drive the actual worker without timer sleeps. Through the public
  write API, assert that three copied frames remain absent from SHM before 10 ms,
  and appear concatenated after the deadline even if caller memory was reused.
- [x] Cover four-frame maximum, a fifth frame deferred another 10 ms, delayed
  scheduling without catch-up, payload-size limits, metadata boundaries, independent
  names, bounded backlog, format validation, remove/re-add, and recovery.
- [x] Add the binary to `Makefile` test dependencies and execution.
- [x] Change poll tests to expect 19200 bytes (48 kHz mono 16-bit, 200 ms),
  retaining 7200 bytes of the first 12000-byte input followed by the second input.
- [x] Run the new tests against the original implementation; verify deferred
  write and expanded receive-retention assertions fail for the intended reasons.

## Task 2: Implementation

- [x] Add a private FIFO node containing `next`, `len`, copied metadata, and a
  flexible payload array, plus head/tail/count/bytes/deadline/scratch in write entries.
- [x] Validate audio PCM with checked unsigned arithmetic. Enqueue copied data
  under the entry lock, bound the backlog, and return zero upon acceptance.
- [x] On each worker iteration before transport recovery, read monotonic time;
  flush due entries by concatenating at most four matching frames that fit SHM,
  commit once, and notify once. Advance the deadline from commit completion time.
  Release entry lock before notifying or logging.
- [x] Free all nodes and merge scratch in entry removal and close. Keep video
  on its existing immediate path.
- [x] Increase poll audio retention to 200 ms with checked format/capacity math.
- [x] Run `make test`; resolve regressions and add an end-to-end burst test using
  the real worker and receiver.

## Task 3: Documentation and verification

- [x] Update `src/agora_shm_manager.h`, `README.md`, and
  `PLAN_shm_ipc_manager.md` with queue acceptance, asynchronous errors, 10 ms
  cadence, four-frame/capacity constraints, overflow policy and 200 ms retention.
- [x] Run `make all test`, then an isolated ASan/UBSan build of the audio and poll
  tests, and `git diff --check`. Review the final diff for lifecycle races and
  video compatibility. Report exact checks and the delivery limitation.

## Verification results

- `make all test`: all six test binaries passed, without compiler warnings.
- Audio and poll binaries rebuilt with `-fsanitize=address,undefined` and passed;
  macOS runs used `ASAN_OPTIONS=detect_leaks=0`.
- Independent review identified the stale pre-commit deadline; a deterministic
  20 ms write-duration regression failed before the fix and passed afterward.
  Reviewer confirmed the correction with no remaining blockers.
- `git diff --check`: passed. Existing unrelated untracked documents preserved.
