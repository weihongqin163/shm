# SHM worker recovery

## Agreed behavior

- Scope: SHM library, tests, documentation. No MCU code or new public manager APIs.
- After successful start, server/client workers exit only when close sets stop.
- Socket failures wait 20 ms, without holding business or transport locks. Close
  wakes waits. EINTR and no-data/timeout do not count toward reconstruction.
- EBADF/ENOTSOCK (including POLLNVAL mapped to EBADF) require reconstruction;
  other socket failures require three consecutive failures. Normal polling
  resets the counter. Failed reconstruction retries every 20 ms until close.
- Preserve manager, SHM mappings, read/write entries, buffered data and persistent
  ownership. Recreate only localsock, retaining server configuration and port.
  Recreated clients send keepalive immediately; server peers register again.
- Poll consumes buffered data first. With no buffered data, recovery returns
  -EAGAIN and errno=EAGAIN; healthy missing/empty entries remain -ENOENT/0.
- Write always attempts SHM first. During recovery, signaling returns -EAGAIN;
  already-written data is not rolled back. Other failures return -errno.
- Errors and recovery transitions are written to stderr without rate limiting,
  outside locks. Logs identify mode, operation, errno, poll events and count.
- Callers stop and join business calls before close. Close may race the worker's
  recovery, and must reclaim both old and newly-created transports safely.
- No notification replay, registration synchronization or historical frame
  recovery. A lost final/only notification requires no compensation.

## Internal design

A transport mutex serializes socket access and replacement, including server
peer-table access. Poll under this mutex is nonblocking; an interruptible
condition wait of 10 ms handles healthy idle periods, and 20 ms handles errors.
This keeps business calls independent of blocking socket waits. Worker dispatch
and callbacks happen after releasing the transport mutex. The existing entry
mutex protects SHM and buffered data. Neither mutex is held while waiting,
allocating a replacement transport, destroying a detached transport, or logging.

Recovery state and rebuild requests remain internal. Poll errors retain native
errno; POLLNVAL becomes EBADF, POLLERR uses SO_ERROR with EIO fallback, and
POLLHUP becomes EIO. Poll event bits are retained for diagnostics. Client sockets
are nonblocking so a stale readiness event cannot block under the transport lock.

Worker receive storage is allocated before thread creation, so allocation failure
is reported by start instead of killing an already-started worker.

## Verification

Fault-injected tests cover server blocking/drain-equivalent receive paths,
client receive/keepalive, isolated/consecutive errors and counter reset, immediate
invalid-socket reconstruction, failed reconstruction, cached reads and SHM writes
during recovery, bidirectional SHM ownership, concurrent writes and close during
recovery. Existing functional tests remain required.
