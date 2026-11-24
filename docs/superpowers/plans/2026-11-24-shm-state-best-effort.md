# Best-effort manager state implementation plan

**Goal:** State persistence success or failure does not determine media startup
or creation success.

**Architecture:** Retain diagnostic errors in state helpers; consume them as
warnings at manager API boundaries. Keep the live Writer table authoritative
for normal resource cleanup, and preserve real SHM/transport failures.

**Tech Stack:** C11, pthreads, POSIX SHM, local UDP, Make.

## Tasks

- [x] Add `tests/test_agora_shm_manager_best_effort.c` using the existing
  test-only inclusion/wrapper pattern and an isolated mkdtemp state root.
  Inject `state_open` errors, direct `open_at` at a regular file, inject
  `state_add` errors, and remove a test-owned live instance directory.
  For client and server Writers, assert successful creation and frame delivery,
  duplicate rejection, successful remove/recreate, and SHM removal on close.
- [x] Add a Makefile target and verify pre-change failure at startup or Writer
  creation for injected open/add failures and deleted directories. The direct
  open_at case was initially denied sandbox escalation; it passed with the
  completed full regression run after explicit user authorization.
- [x] In `src/agora_shm_manager.c`, initialize `m->state.lock_fd = -1` before
  state initialization. Log state errors with mode, port, operation, errno,
  directory and SHM name. Continue startup and creation, skip state updates
  without a state lock, and exclude manifest errors from remove's result.
  Preserve state on close-time persistence failures.
- [x] Update `src/agora_shm_manager.h`, `README.md`, and
  `PLAN_shm_ipc_manager.md` to document best-effort persistence and crash limits.
- [x] Run the new test and `make test`, inspect the final patch, and report
  verification and remaining crash-recovery limits. Preserve existing user
  changes in the current checkout; no commit is part of this handoff.

## Verification

On macOS, `make test` completed with exit code 0 after explicit user
authorization for local UDP and POSIX shared memory. All five suites passed:
state, poll, transport recovery, socket errors, and best-effort ownership
persistence. `git diff --check` passed. No production deployment was performed.
