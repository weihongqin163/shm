# Agora SHM Manager Remove and Crash Recovery Design

## Goals

This change makes the manager that creates a write-side SHM object responsible
for its complete lifecycle:

- Removing a write entry notifies the peer to detach its read entry.
- The creator unlinks its write-side SHM object on remove and close.
- Successfully added write-side SHM names are persisted so a later manager can
  unlink objects left by an abnormally terminated process.
- Multiple managers may run concurrently, including managers using the same UDP
  port and multiple managers in one process.

Read-side managers never unlink SHM objects. They only close and clear their
local read entries.

## Remove Protocol

Add `AGORA_LOCALSOCK_MSG_REMOVECMD` with wire value `4`. Its payload is exactly
`AGORA_SHM_IPC_SHM_NAME_BYTES` bytes containing a NUL-terminated SHM name.

When `agora_shm_manager_remove` finds a write entry, it:

1. Sends REMOVECMD through the existing server broadcast or client send path.
2. Closes the local write mapping.
3. Calls `agora_shm_ipc_unlink` for the SHM name.
4. Removes the name from the persistent manifest.
5. Clears the write-table entry.

Cleanup continues even if signaling fails. The function returns the first
meaningful error after completing all local cleanup, so the caller knows that
the remote detach notification may not have been delivered. `ENOENT` from
`shm_unlink` is treated as successful idempotent cleanup.

When REMOVECMD is received, the manager validates the exact payload size and a
non-empty NUL-terminated name, locks its table, and clears only the matching
read entry. A missing read entry is a no-op. REMOVECMD never clears a write
entry and never calls `shm_unlink` on the receiver.

Normal manager close sends REMOVECMD for every active write entry before the
localsocket is destroyed, then closes and unlinks those entries. This applies
the same creator-owned lifecycle rule to explicit remove and normal shutdown.
Because UDP has no acknowledgment, delivery remains best effort during close.

The localsocket parser treats REMOVECMD as a known message type. It may register
or refresh a server peer under the same rules as WRITECMD.

## Persistent State Layout

The fixed state root is:

```text
/tmp/agora_shm_manager
```

Each manager owns a unique directory below it:

```text
manager-<random>/
  owner.lock
  shm.list
```

The manager creates the directory under a temporary name, opens `owner.lock`
with close-on-exec enabled, acquires `flock(LOCK_EX | LOCK_NB)`, creates an empty
manifest, and atomically renames the directory to the `manager-*` form. Recovery
only scans published `manager-*` directories, so it cannot mistake a manager
that is still initializing for stale state.

`flock` is used instead of POSIX process-associated record locking. Separate
open file descriptions conflict even when multiple managers exist in the same
process. The kernel releases the lock after normal exit, crashes, and SIGKILL.

The state root and manager directories must not follow symbolic links. Manager
directories and files use owner-only permissions (`0700` and `0600`). Directory
entries that do not match the manager naming and expected file layout are
ignored.

## Manifest Format and Updates

`shm.list` contains one validated SHM name per line. Names must be non-empty,
start with `/`, fit in `AGORA_SHM_IPC_SHM_NAME_BYTES`, and contain no newline.
The maximum record count is the write-table capacity.

Updates use a temporary file in the same manager directory:

1. Write the complete new manifest.
2. `fsync` the temporary file.
3. Rename it over `shm.list`.
4. `fsync` the manager directory.

This prevents recovery from observing a partially rewritten manifest.

`agora_shm_manager_add` first creates and initializes the SHM object, then
persists the updated manifest before publishing the write-table entry and
returning success. If persistence fails, it closes and unlinks the new object
and returns failure. Therefore every successful add is recoverable.

There is an unavoidable small interval between successful `shm_open` and the
manifest update during which forced process termination can leave an untracked
object. Eliminating that interval would require an ownership transaction token
inside the SHM format. That protocol expansion is outside this change.

On remove, unlink happens before the manifest name is removed. A crash between
those operations leaves a stale manifest record, but the next recovery treats
an already absent SHM object as successfully cleaned.

## Startup Recovery

Before publishing a new manager, `agora_shm_manager_start` initializes the state
root and scans every published manager directory:

1. Open `owner.lock` without following symbolic links.
2. Try `flock(LOCK_EX | LOCK_NB)`.
3. Skip the directory when the lock is held by a live manager.
4. When the lock succeeds, read and validate `shm.list`.
5. Call `agora_shm_ipc_unlink` for each record, accepting `ENOENT`.
6. Remove the manifest, lock file, and manager directory after complete cleanup.

If a valid stale manifest contains an SHM object that cannot be unlinked, start
fails and preserves the state directory for a later retry. Invalid manifests
also fail safely rather than unlinking unvalidated names.

Concurrent recovery is safe: only one scanner can lock a stale owner file at a
time. Cleanup and unlink operations are idempotent.

## Close Behavior

After stopping and joining the worker, `agora_shm_manager_close`:

1. Sends best-effort REMOVECMD messages for active write entries.
2. Closes all read and write mappings.
3. Unlinks every creator-owned write SHM object.
4. Removes the manager manifest and lock file while retaining the lock.
5. Removes the manager state directory.
6. Closes the lock descriptor and frees the manager.

The state files are removed only after SHM cleanup. If the process terminates
during close, the remaining locked state becomes recoverable when the kernel
releases the lock.

## Pod Deployment Rule

On ordinary Linux, `/tmp/agora_shm_manager` is used directly. In Kubernetes,
production deployments must mount a shared `emptyDir` at that exact path:

```yaml
volumeMounts:
  - name: shm-manager-state
    mountPath: /tmp/agora_shm_manager

volumes:
  - name: shm-manager-state
    emptyDir: {}
```

All containers in a Pod that run cooperating managers must mount the same
`emptyDir`. This preserves manifests across container crashes and restarts
within the same Pod. The data is intentionally removed when the Pod itself is
deleted. A read-only root filesystem also requires this writable mount.

## Compatibility and Errors

- No public manager function signatures change.
- Existing WRITECMD behavior and wire value remain unchanged.
- Peers that do not understand message type `4` ignore it under the existing
  unknown-message behavior; they will retain a read entry until their own close.
- Failure to initialize or recover persistent state makes manager start fail.
- Failure to persist a newly added name makes add fail and rolls back its SHM.
- Remove always attempts complete local cleanup, even when notification or
  manifest operations fail.

## Tests

Tests will cover:

- Removing a write entry sends REMOVECMD and removes the peer read entry.
- A received REMOVECMD never removes a same-name write entry.
- Removing a read entry sends no REMOVECMD and never unlinks the SHM object.
- Add writes the name to the manager manifest; remove deletes it.
- Close removes the manifest and state directory.
- Startup recovers and removes SHM objects from an unlocked stale manifest.
- Startup skips a manifest whose owner lock is still held.
- Multiple managers, including managers using one port and managers in one
  process, do not clean each other's state.
- Malformed manifests and names are rejected without unsafe unlink attempts.
- Existing polling, callback, write signaling, and concurrent-frame tests still
  pass.
