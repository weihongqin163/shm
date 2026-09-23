# Agora SHM Manager Remove and Crash Recovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add REMOVECMD-based reader detachment and crash-safe creator-owned SHM cleanup without changing the public manager API.

**Architecture:** Keep wire dispatch and entry ownership in `agora_shm_manager.c`, and add a focused internal state module that owns per-manager locked directories, atomic manifests, and stale-owner recovery. Each successful writer add is recorded before it becomes visible to callers; remove and close notify readers, unlink creator resources, and then retire the persisted name.

**Tech Stack:** C11, POSIX SHM, pthreads, loopback UDP, `flock`, `openat`/`renameat`/`unlinkat`, Make, fork-based integration tests.

---

## File Structure

- Create `src/agora_shm_manager_state.h`: internal state handle and operations for open/recover, add/remove manifest records, and close.
- Create `src/agora_shm_manager_state.c`: secure directory traversal, `flock` ownership, manifest parsing/writing, and stale SHM recovery.
- Create `tests/test_agora_shm_manager_state.c`: isolated tests using a `mkdtemp` state root.
- Modify `src/agora_localsock.h`: assign wire value `4` to REMOVECMD.
- Modify `src/agora_localsock.c`: classify REMOVECMD as a known peer-registering message.
- Modify `src/agora_shm_manager.c`: initialize persistent state, dispatch/send REMOVECMD, and couple writer lifecycle to the manifest.
- Modify `src/agora_shm_manager.h`: document remove/close notification and recovery behavior.
- Modify `tests/test_agora_shm_manager_poll.c`: protocol and manager-lifecycle integration tests.
- Modify `Makefile`: build the state object and its test, and include both tests in `make test`.
- Modify `README.md`, `PLAN_localsocket.md`, `PLAN_shm_ipc_manager.md`, and `CHANGELOG.md`: record protocol value, ownership, recovery, and Pod deployment rules.

### Task 1: Locked Manager State and Stale Recovery

**Files:**
- Create: `src/agora_shm_manager_state.h`
- Create: `src/agora_shm_manager_state.c`
- Create: `tests/test_agora_shm_manager_state.c`
- Modify: `Makefile`

- [ ] **Step 1: Add failing state lifecycle tests**

Define the internal API in the test expectation before implementation:

```c
#include "agora_shm_manager_state.h"

static int test_add_remove_and_close_manifest(void) {
  char root[] = "/tmp/agora_shm_state_test_XXXXXX";
  CHECK(mkdtemp(root) != NULL);

  AgoraShmManagerState state;
  CHECK(agora_shm_manager_state_open_at(&state, root) == 0);
  CHECK(agora_shm_manager_state_add(&state, "/state_one") == 0);
  CHECK(manifest_contains(state.dir_path, "/state_one") == 1);
  CHECK(agora_shm_manager_state_remove(&state, "/state_one") == 0);
  CHECK(manifest_contains(state.dir_path, "/state_one") == 0);

  char owned_dir[PATH_MAX];
  memcpy(owned_dir, state.dir_path, sizeof(owned_dir));
  agora_shm_manager_state_close(&state);
  CHECK(access(owned_dir, F_OK) == -1 && errno == ENOENT);
  CHECK(rmdir(root) == 0);
  return 0;
}
```

Add a same-process lock isolation test. It is specifically required to reject
POSIX process-associated record locks as an implementation shortcut:

```c
static int test_live_manager_is_not_recovered_in_same_process(void) {
  char root[] = "/tmp/agora_shm_state_test_XXXXXX";
  char shm_name[AGORA_SHM_IPC_SHM_NAME_BYTES];
  make_shm_name(shm_name, sizeof(shm_name), "live");
  CHECK(mkdtemp(root) != NULL);
  CHECK(create_test_shm(shm_name) == 0);

  AgoraShmManagerState first;
  AgoraShmManagerState second;
  CHECK(agora_shm_manager_state_open_at(&first, root) == 0);
  CHECK(agora_shm_manager_state_add(&first, shm_name) == 0);
  CHECK(agora_shm_manager_state_open_at(&second, root) == 0);
  CHECK(shm_exists(shm_name) == 1);

  agora_shm_manager_state_close(&second);
  CHECK(agora_shm_manager_state_remove(&first, shm_name) == 0);
  agora_shm_manager_state_close(&first);
  CHECK(agora_shm_ipc_unlink(shm_name) == 0);
  CHECK(rmdir(root) == 0);
  return 0;
}
```

Add fork-based stale recovery and malformed-manifest tests:

```c
static int test_dead_owner_is_recovered(void) {
  char root[] = "/tmp/agora_shm_state_test_XXXXXX";
  char shm_name[AGORA_SHM_IPC_SHM_NAME_BYTES];
  make_shm_name(shm_name, sizeof(shm_name), "stale");
  CHECK(mkdtemp(root) != NULL);
  CHECK(create_test_shm(shm_name) == 0);

  pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    AgoraShmManagerState abandoned;
    if (agora_shm_manager_state_open_at(&abandoned, root) != 0 ||
        agora_shm_manager_state_add(&abandoned, shm_name) != 0) {
      _exit(2);
    }
    _exit(0); /* Deliberately skip close. */
  }
  CHECK(wait_child_ok(child) == 0);

  AgoraShmManagerState recovery;
  CHECK(agora_shm_manager_state_open_at(&recovery, root) == 0);
  CHECK(shm_exists(shm_name) == 0);
  agora_shm_manager_state_close(&recovery);
  CHECK(rmdir(root) == 0);
  return 0;
}
```

The malformed test constructs a published `manager-bad` directory with an
unlocked `owner.lock` and `shm.list` containing `not-a-posix-shm-name\n`, then
asserts `agora_shm_manager_state_open_at` fails with `EPROTO`, leaves the
directory in place, and does not unlink an unrelated test SHM.

- [ ] **Step 2: Add the test target and verify RED**

Add these Make variables and rules:

```make
AGORA_MANAGER_STATE_OBJ := $(BUILD_DIR)/agora_shm_manager_state.o
AGORA_MANAGER_STATE_TEST := $(BUILD_DIR)/test_agora_shm_manager_state

$(AGORA_MANAGER_STATE_OBJ): src/agora_shm_manager_state.c src/agora_shm_manager_state.h src/agora_shm_ipc.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c src/agora_shm_manager_state.c -o $@

$(AGORA_MANAGER_STATE_TEST): tests/test_agora_shm_manager_state.c $(AGORA_OBJ) $(AGORA_MANAGER_STATE_OBJ) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ tests/test_agora_shm_manager_state.c $(AGORA_OBJ) $(AGORA_MANAGER_STATE_OBJ) $(LIBS)
```

Run:

```bash
make build/test_agora_shm_manager_state
```

Expected: compilation fails because `agora_shm_manager_state.h` and its
functions do not exist.

- [ ] **Step 3: Implement the internal state interface**

Create `src/agora_shm_manager_state.h` with a fixed-size, allocation-free state:

```c
#ifndef AGORA_SHM_MANAGER_STATE_H
#define AGORA_SHM_MANAGER_STATE_H

#include "agora_shm_ipc.h"

#include <limits.h>
#include <stddef.h>

#define AGORA_SHM_MANAGER_STATE_ROOT "/tmp/agora_shm_manager"
#define AGORA_SHM_MANAGER_STATE_MAX_NAMES 256u

typedef struct AgoraShmManagerState {
  int lock_fd;
  char dir_path[PATH_MAX];
  char names[AGORA_SHM_MANAGER_STATE_MAX_NAMES]
            [AGORA_SHM_IPC_SHM_NAME_BYTES];
  size_t name_count;
} AgoraShmManagerState;

int agora_shm_manager_state_open(AgoraShmManagerState *state);
int agora_shm_manager_state_open_at(AgoraShmManagerState *state,
                                    const char *root);
int agora_shm_manager_state_add(AgoraShmManagerState *state,
                               const char *shm_name);
int agora_shm_manager_state_remove(AgoraShmManagerState *state,
                                  const char *shm_name);
void agora_shm_manager_state_close(AgoraShmManagerState *state);

#endif
```

Implement `src/agora_shm_manager_state.c` around these private helpers:

```c
static int validate_shm_name(const char *name);
static int recover_published_dir(int root_fd, const char *entry_name);
static int recover_all(int root_fd);
static int write_all(int fd, const void *data, size_t len);
static int persist_manifest(AgoraShmManagerState *state);
static int remove_owned_dir(AgoraShmManagerState *state);
```

Required mechanics:

```c
/* Publish only after the owner lock is held and the empty manifest is durable. */
CHECK_SYSCALL(mkdtemp(creating_template));
lock_fd = open(lock_path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW,
               0600);
CHECK_SYSCALL(lock_fd);
CHECK_SYSCALL(flock(lock_fd, LOCK_EX | LOCK_NB));
CHECK_SYSCALL(persist_manifest(&candidate));
CHECK_SYSCALL(rename(creating_path, published_path));
CHECK_SYSCALL(fsync(root_fd));
```

Do not introduce a production `CHECK_SYSCALL` macro that aborts. Each failure
path must save `errno`, close descriptors, remove only files created by the
current attempt, zero the output state, restore `errno`, and return `-1`.

Recovery must use directory-relative operations and reject symlinks:

```c
int dir_fd = openat(root_fd, entry_name,
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
int lock_fd = openat(dir_fd, "owner.lock",
                     O_RDWR | O_CLOEXEC | O_NOFOLLOW);
if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0 &&
    (errno == EWOULDBLOCK || errno == EAGAIN)) {
  /* Live manager: skip without touching its manifest. */
}
```

Parse at most 256 newline-terminated names, reject embedded NULs, overlong
records, missing final newlines, names not beginning with `/`, and unexpected
files other than the known `shm.list.tmp` crash residue. Remove that temporary
file only after successfully parsing the committed `shm.list`. For stale
cleanup, ignore `ENOENT` from `agora_shm_ipc_unlink`; preserve the directory and
return the first other error.

Persist updates by creating `shm.list.tmp` with `O_EXCL|O_NOFOLLOW`, writing the
complete in-memory name set, calling `fsync`, then `renameat` to `shm.list` and
`fsync` on the manager directory. Always remove a leftover temp file after an
update failure.

- [ ] **Step 4: Run the focused state test and verify GREEN**

Run:

```bash
make build/test_agora_shm_manager_state && ./build/test_agora_shm_manager_state
```

Expected: `PASS test_agora_shm_manager_state` and exit status `0`.

- [ ] **Step 5: Commit the state module**

```bash
git add Makefile src/agora_shm_manager_state.c src/agora_shm_manager_state.h tests/test_agora_shm_manager_state.c
git commit -m "feat: persist shm manager ownership"
```

### Task 2: REMOVECMD Wire Handling

**Files:**
- Modify: `src/agora_localsock.h:27-30`
- Modify: `src/agora_localsock.c:50-56,100-112,138-151`
- Modify: `src/agora_shm_manager.c:343-416,774-801`
- Modify: `tests/test_agora_shm_manager_poll.c`

- [ ] **Step 1: Write failing REMOVECMD integration tests**

Add a wait helper that treats `ENOENT` as successful detachment:

```c
static int wait_for_missing_read_entry(AgoraShmManager *manager,
                                       const char *shm_name) {
  AgoraShmIpcHeader header;
  char byte;
  for (unsigned i = 0; i < 100u; ++i) {
    errno = 0;
    if (agora_shm_manager_poll(manager, shm_name, &header, &byte, 1u) == -1 &&
        errno == ENOENT) {
      return 0;
    }
    sleep_ms(10);
  }
  errno = ETIMEDOUT;
  return -1;
}
```

Test writer remove notifying its receiver:

```c
static int test_writer_remove_detaches_peer_read_entry(void) {
  TestFixture f;
  CHECK(fixture_open(&f, 1024u) == 0);
  uint8_t data[16] = {0};
  CHECK(write_frame(&f, data, sizeof(data), AGORA_SHM_MEDIA_VIDEO) == 0);
  AgoraShmIpcHeader header;
  char out[sizeof(data)];
  CHECK(wait_for_poll(&f, &header, out, sizeof(out)) == (ssize_t)sizeof(out));

  CHECK(agora_shm_manager_remove(f.writer, f.shm_name) == 0);
  CHECK(wait_for_missing_read_entry(f.receiver, f.shm_name) == 0);
  f.shm_name[0] = '\0'; /* Fixture must not remove the already removed name. */
  fixture_close(&f);
  return 0;
}
```

Add a raw REMOVECMD test against a manager holding a same-name write entry. Send
the fixed 64-byte payload from a temporary localsock client, wait 100 ms, then
assert `agora_shm_manager_write` still succeeds. This proves receive dispatch
does not search or clear the write table.

- [ ] **Step 2: Run the focused manager test and verify RED**

Run:

```bash
make build/test_agora_shm_manager_poll
```

Expected: compilation fails because `AGORA_LOCALSOCK_MSG_REMOVECMD` is undefined.
After adding only the constant to permit compilation, run the binary and expect
`test_writer_remove_detaches_peer_read_entry` to time out because no remove
message is dispatched.

- [ ] **Step 3: Add the wire value and known-message classification**

In `src/agora_localsock.h`:

```c
#define AGORA_LOCALSOCK_MSG_WRITECMD 3u
#define AGORA_LOCALSOCK_MSG_REMOVECMD 4u
```

In `src/agora_localsock.c`, add `AGORA_LS_PARSE_REMOVECMD`, return it from
`parse_datagram`, and include it in the known message condition that inserts or
refreshes peers:

```c
if (msg_type == AGORA_LOCALSOCK_MSG_REMOVECMD) {
  return AGORA_LS_PARSE_REMOVECMD;
}

if (k == AGORA_LS_PARSE_KEEPALIVE || k == AGORA_LS_PARSE_APP ||
    k == AGORA_LS_PARSE_WRITECMD || k == AGORA_LS_PARSE_REMOVECMD) {
  /* Existing insert/refresh path. */
}
```

- [ ] **Step 4: Implement send and receive dispatch**

Add one send helper so remove and close use identical payload construction:

```c
static int manager_send_removecmd(AgoraShmManager *m, const char *shm_name) {
  char payload[AGORA_SHM_IPC_SHM_NAME_BYTES] = {0};
  (void)strncpy(payload, shm_name, sizeof(payload) - 1u);
  if (m->server_mode && m->srv != NULL) {
    return agora_localsock_server_send_datagram(
        m->srv, (uint16_t)AGORA_LOCALSOCK_MSG_REMOVECMD, payload,
        sizeof(payload));
  }
  if (!m->server_mode && m->cli != NULL) {
    return agora_localsock_client_send_datagram(
        m->cli, (uint16_t)AGORA_LOCALSOCK_MSG_REMOVECMD, payload,
        sizeof(payload));
  }
  errno = EINVAL;
  return -1;
}
```

Add a receive helper that only addresses the read table:

```c
static void manager_dispatch_removecmd(AgoraShmManager *m,
                                       const char *payload) {
  char shm_name[AGORA_SHM_IPC_SHM_NAME_BYTES];
  memcpy(shm_name, payload, sizeof(shm_name));
  if (memchr(shm_name, '\0', sizeof(shm_name)) == NULL || shm_name[0] == '\0') {
    return;
  }
  pthread_mutex_lock(&m->lock);
  int ridx = find_read_entry_by_name(m, shm_name);
  if (ridx >= 0) {
    clear_read_entry(&m->read_entries[ridx]);
  }
  pthread_mutex_unlock(&m->lock);
}
```

Extend `manager_on_udp_datagram` with an exact payload-size check and an
immediate return after dispatch. In the write-entry branch of
`agora_shm_manager_remove`, copy the name while locked, unlock, call
`manager_send_removecmd`, then relock, close and clear the write entry, unlock,
and call `agora_shm_ipc_unlink`. Preserve the first send or unlink error, but
always complete local cleanup. Add a `removing` flag to the write entry, set it
before unlocking for send, and make `find_write_entry_by_name` ignore removing
entries so concurrent write/remove calls cannot use the mapping. Do not send
anything when the remove operation matched only a read entry. Task 3 extends
this already-tested cleanup path with manifest retirement.

- [ ] **Step 5: Run the focused manager test and verify GREEN**

Run:

```bash
make build/test_agora_shm_manager_poll && ./build/test_agora_shm_manager_poll
```

Expected: `PASS test_agora_shm_manager_poll` and exit status `0`.

- [ ] **Step 6: Commit the protocol behavior**

```bash
git add src/agora_localsock.c src/agora_localsock.h src/agora_shm_manager.c tests/test_agora_shm_manager_poll.c
git commit -m "feat: detach readers with removecmd"
```

### Task 3: Couple Writer Lifecycle to Persistent State

**Files:**
- Modify: `src/agora_shm_manager.c:35-57,503-662,719-801`
- Modify: `src/agora_shm_manager.h:46-71`
- Modify: `tests/test_agora_shm_manager_poll.c`
- Modify: `Makefile`

- [ ] **Step 1: Write failing manager persistence tests**

Add a test-only manifest scanner that opens
`AGORA_SHM_MANAGER_STATE_ROOT/manager-*/shm.list` and searches for the fixture's
unique SHM name without modifying any state.

```c
static int test_add_and_remove_update_persistent_manifest(void) {
  TestFixture f;
  CHECK(fixture_open(&f, 1024u) == 0);
  CHECK(count_manifests_containing(f.shm_name) == 1);
  CHECK(agora_shm_manager_remove(f.writer, f.shm_name) == 0);
  CHECK(count_manifests_containing(f.shm_name) == 0);
  f.shm_name[0] = '\0';
  fixture_close(&f);
  return 0;
}
```

Add a fork integration test. The child starts a client manager, adds a unique
SHM, sends a one-byte readiness marker through a pipe, and calls `_exit(0)`
without close. After `waitpid`, the parent verifies the SHM exists, starts a new
client manager on the same reserved port, and verifies startup recovery makes
`shm_open(name, O_RDWR, 0)` fail with `ENOENT`.

- [ ] **Step 2: Link the state object and verify RED**

Add `$(AGORA_MANAGER_STATE_OBJ)` to both the manager demo and manager poll test
link prerequisites and command lines. Run:

```bash
make build/test_agora_shm_manager_poll && ./build/test_agora_shm_manager_poll
```

Expected: `test_add_and_remove_update_persistent_manifest` fails because manager
start/add/remove do not call the state module.

- [ ] **Step 3: Initialize state before sockets and worker creation**

Add the state include and member:

```c
#include "agora_shm_manager_state.h"

struct AgoraShmManager {
  /* Existing fields. */
  AgoraShmManagerState state;
};
```

Immediately after allocating and initializing the manager's scalar fields,
call:

```c
if (agora_shm_manager_state_open(&m->state) != 0) {
  int saved_errno = errno;
  free(m);
  errno = saved_errno;
  return -1;
}
```

Every later start failure path must call
`agora_shm_manager_state_close(&m->state)` after releasing socket/thread
resources and before freeing `m`. Preserve the original failure `errno`.

- [ ] **Step 4: Persist successful adds before publishing the write entry**

After `agora_shm_ipc_writer_session_begin` and before setting `e->in_use`, add:

```c
if (agora_shm_manager_state_add(&m->state, shm_name) != 0) {
  int saved_errno = errno;
  agora_shm_ipc_close(&ipc);
  (void)agora_shm_ipc_unlink(shm_name);
  pthread_mutex_unlock(&m->lock);
  errno = saved_errno;
  return -1;
}
```

Also fix the existing writer-session failure path to unlink the creator object;
closing its mapping alone leaves an unreachable SHM object.

- [ ] **Step 5: Make write removal complete all cleanup and preserve errors**

Replace the current user-modified write branch (including its explanatory
comment and direct unlink) with a helper that follows this ordering:

```c
static int manager_remove_write_entry(AgoraShmManager *m, int widx,
                                      int notify_peer) {
  char shm_name[AGORA_SHM_IPC_SHM_NAME_BYTES];
  int first_error = 0;
  int unlink_complete = 0;
  AgoraShmManagerWriteEntry *entry = &m->write_entries[widx];
  memcpy(shm_name, entry->shm_name, sizeof(shm_name));

  if (notify_peer != 0 && manager_send_removecmd(m, shm_name) != 0) {
    first_error = errno;
  }
  clear_write_entry(entry);
  if (agora_shm_ipc_unlink(shm_name) == 0 || errno == ENOENT) {
    unlink_complete = 1;
  } else if (first_error == 0) {
    first_error = errno;
  }
  if (unlink_complete != 0 &&
      agora_shm_manager_state_remove(&m->state, shm_name) != 0 &&
      first_error == 0) {
    first_error = errno;
  }
  if (first_error != 0) {
    errno = first_error;
    return -1;
  }
  return 0;
}
```

The implementation must capture `errno` immediately after a failed unlink
rather than evaluating stale `errno` after a successful call. The abbreviated
snippet above expresses the required branching; production code stores the
unlink result and saved error explicitly. A real unlink failure intentionally
leaves the name in the manifest for future recovery.

Do not call socket send routines while holding `m->lock`: mark/copy the selected
entry under the lock, unlock for notification, then relock and finish cleanup in
a way that prevents concurrent write/remove from using the entry. The minimal
implementation may add a `removing` flag to `AgoraShmManagerWriteEntry`; lookup
functions and `agora_shm_manager_write` must treat removing entries as absent.

- [ ] **Step 6: Update close ordering**

After worker join and before destroying localsocket objects, copy active writer
names, mark entries removing, and send best-effort REMOVECMD for each. Then,
under the manager lock, close and unlink every creator entry. Only after all SHM
cleanup call:

```c
agora_shm_manager_state_close(&m->state);
```

Because `close` returns `void`, retain manifest records when a non-`ENOENT`
unlink fails; `state_close` must support a preserve-on-error path. Add the
internal operation below and use it instead of unconditional deletion:

```c
void agora_shm_manager_state_abandon(AgoraShmManagerState *state);
```

`state_abandon` only closes the lock descriptor and zeros memory. A later start
will retry the preserved manifest. Use normal `state_close` only when the
in-memory manifest is empty; if any explicit remove, close unlink, or manifest
rewrite failed and left a record, use `state_abandon`.

- [ ] **Step 7: Run both focused tests and verify GREEN**

Run:

```bash
make build/test_agora_shm_manager_state build/test_agora_shm_manager_poll
./build/test_agora_shm_manager_state
./build/test_agora_shm_manager_poll
```

Expected: both print `PASS` and exit status `0`.

- [ ] **Step 8: Commit lifecycle integration**

```bash
git add Makefile src/agora_shm_manager.c src/agora_shm_manager.h src/agora_shm_manager_state.c src/agora_shm_manager_state.h tests/test_agora_shm_manager_poll.c
git commit -m "feat: recover abandoned manager shm"
```

### Task 4: Documentation and Protocol Contract

**Files:**
- Modify: `README.md`
- Modify: `PLAN_localsocket.md`
- Modify: `PLAN_shm_ipc_manager.md`
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Update public behavior documentation**

Record these exact contract points:

```text
REMOVECMD has msg_type 4 and a 64-byte SHM-name payload.
Only removal of creator-owned write entries sends REMOVECMD.
The receiver clears only a matching read entry and never unlinks it.
Successful add records the name under /tmp/agora_shm_manager.
Startup skips locked live managers and recovers unlocked stale managers.
Close removes writer SHM objects and the current manager state directory.
```

Add the approved Pod rule and manifest mount example to README:

```yaml
volumeMounts:
  - name: shm-manager-state
    mountPath: /tmp/agora_shm_manager
volumes:
  - name: shm-manager-state
    emptyDir: {}
```

State explicitly that every cooperating container in one Pod must mount the
same `emptyDir`, and that a read-only root filesystem requires the mount.

- [ ] **Step 2: Check documentation consistency**

Run:

```bash
rg -n "REMOVECMD|WRITECMD|agora_shm_manager|emptyDir|msg_type" README.md PLAN_localsocket.md PLAN_shm_ipc_manager.md CHANGELOG.md
git diff --check
```

Expected: REMOVECMD is consistently value `4`, WRITECMD remains value `3`, the
fixed path is identical everywhere, and `git diff --check` emits no errors.

- [ ] **Step 3: Commit documentation**

```bash
git add README.md PLAN_localsocket.md PLAN_shm_ipc_manager.md CHANGELOG.md
git commit -m "docs: document shm ownership recovery"
```

### Task 5: Full Verification

**Files:**
- Verify all modified production, test, build, and documentation files.

- [ ] **Step 1: Clean rebuild and run all repository tests**

Run:

```bash
make clean
make test
```

Expected: both `PASS test_agora_shm_manager_state` and
`PASS test_agora_shm_manager_poll`, with exit status `0`.

- [ ] **Step 2: Run sanitizer builds**

Compile and run each test with AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cc -std=c11 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Isrc \
  -o /tmp/test_agora_shm_manager_state_asan \
  tests/test_agora_shm_manager_state.c src/agora_shm_ipc.c \
  src/agora_shm_manager_state.c -pthread
ASAN_OPTIONS=detect_leaks=1 /tmp/test_agora_shm_manager_state_asan

cc -std=c11 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Isrc \
  -o /tmp/test_agora_shm_manager_poll_asan \
  tests/test_agora_shm_manager_poll.c src/agora_shm_ipc.c \
  src/agora_localsock.c src/agora_shm_manager_state.c \
  src/agora_shm_manager.c -pthread
ASAN_OPTIONS=detect_leaks=1 /tmp/test_agora_shm_manager_poll_asan
```

Expected: both tests print `PASS`; sanitizers report no memory errors, undefined
behavior, or leaks.

- [ ] **Step 3: Inspect final scope and formatting**

Run:

```bash
git diff --check HEAD~4..HEAD
git status --short
git log -5 --oneline
```

Expected: no whitespace errors; only the user's pre-existing unrelated files
remain uncommitted; the feature is represented by focused state, protocol,
lifecycle, and documentation commits.

- [ ] **Step 4: Review requirements against evidence**

Confirm all four requested behaviors against fresh test output:

```text
1. write_entry remove sends REMOVECMD: protocol integration test passes.
2. REMOVECMD clears peer read_slot only: detach and write-preservation tests pass.
3. add persists names and start recovers stale SHM: state and fork tests pass.
4. close deletes manager state: state lifecycle and manager close tests pass.
```
