# Agora SHM Manager Poll Mode Design

## Goal

Extend `AgoraShmManager` with a polling receive mode while preserving the
existing callback mode. The mode is selected when the manager starts:

- `on_frame != NULL`: keep the existing zero-copy callback behavior.
- `on_frame == NULL`: retain received data in each read entry for synchronous
  consumption through `agora_shm_manager_poll`.

The two modes are mutually exclusive.

## Public API

Add this declaration to `agora_shm_manager.h`:

```c
ssize_t agora_shm_manager_poll(AgoraShmManager *m, const char *shm_name,
                               AgoraShmIpcHeader *header, char *out,
                               size_t max_payload_size);
```

The public header includes `<sys/types.h>` so `ssize_t` is available.

The caller owns and allocates `header` and `out`. `max_payload_size` is the
capacity of `out`.

Return values and errors:

- Positive value: number of bytes copied into `out`.
- `0`: the named read entry exists but currently has no buffered data. Neither
  `header` nor `out` is modified.
- `-1`, `EINVAL`: `m`, `header`, or `out` is NULL; `shm_name` is NULL or empty;
  or `max_payload_size` is zero.
- `-1`, `ENOTSUP`: the manager was started in callback mode.
- `-1`, `ENOENT`: `shm_name` is not present in the read table.
- `-1`, `ENOBUFS`: a complete buffered video frame does not fit in `out`. The
  video frame remains buffered for a later retry.

On a positive return, `header` is copied from the most recently accepted frame
and `header->data_len` is changed to the number of bytes returned by this poll.
For aggregated audio this means the returned header metadata and sequence are
from the newest appended frame, while `data_len` describes the aggregate chunk
copied to `out`.

## Read Entry State

Each `AgoraShmManagerReadEntry` gains private polling state:

```c
uint32_t media_type;
int media_type_valid;
uint8_t *buffer;
size_t buffer_capacity;
size_t data_len;
unsigned latest_seq;
int latest_seq_valid;
AgoraShmIpcHeader latest_header;
```

This state is used only in poll mode. Callback mode does not allocate an entry
buffer.

The media type and buffer are initialized lazily after the first successful
stable `agora_shm_ipc_read`. This is required because the APP signal carries a
full header, but the WRITECMD attach path currently constructs a synthetic
header that does not preserve `media_type`. The stable read snapshot is the
authoritative source in both paths.

The first accepted frame locks the entry to its media type. Only
`AGORA_SHM_MEDIA_AUDIO` and `AGORA_SHM_MEDIA_VIDEO` are accepted. A later frame
with a different or invalid media type is discarded without changing the
buffered data or entry type.

Buffer capacities are:

- Audio: `48 * 2 * 200`, or 19,200 bytes. This is a fixed 200 ms capacity for
  the agreed 48 kHz, mono, 16-bit PCM format; it is not recalculated from header
  metadata.
- Video: `manager->read_cap` bytes.

If the first allocation fails, that frame is discarded and its sequence is not
marked as accepted. The read entry remains attached, and a later read opportunity
(including a repeated signal for the same sequence) may retry allocation. The
worker thread remains running.

`clear_read_entry` frees `buffer` in addition to closing the SHM mapping and
clearing the structure. This covers both `agora_shm_manager_remove` and
`agora_shm_manager_close`.

## Receive Path

`agora_shm_manager_start` no longer rejects a NULL `on_frame`. All other start
validation remains unchanged.

After a stable SHM read:

1. In callback mode, preserve the current behavior: release the manager mutex,
   then invoke `on_frame` with the SHM mapping view and header snapshot.
2. In poll mode, keep the mutex and update the matching read entry's private
   buffer.

The stable snapshot's `seq` is compared for equality with `latest_seq`. If it
matches an already accepted sequence, the frame is discarded. This prevents a
repeated signal for one stable SHM frame from appending audio twice or replacing
video unnecessarily. Sequence ordering is not compared, so unsigned wraparound
does not require special handling.

Once a new frame has been accepted into the entry buffer, store its full header
as `latest_header`, store its sequence as `latest_seq`, and mark the sequence as
valid. Consuming all buffered bytes does not clear this sequence state, so a
duplicate signal cannot requeue an already consumed frame.

### Audio Append

Let the fixed capacity be `C`, existing buffered length be `N`, and incoming
length be `L`.

- If `N + L <= C`, append all `L` bytes.
- If `N + L > C`, discard `N + L - C` oldest bytes, move any surviving old
  bytes to the start, then append the incoming bytes.
- If `L >= C`, discard all old data and retain only the final `C` bytes of the
  incoming frame.

The result always contains the newest available audio and never exceeds 200 ms.

### Video Replace

Each accepted video frame replaces the previous buffered video frame in full.
The entry length becomes the new frame length. A defensive capacity check
discards an oversized incoming frame without damaging the previously buffered
complete frame, although normal dispatch validation already limits frames to
`manager->read_cap`.

## Poll Consumption

`agora_shm_manager_poll` holds the existing manager mutex while locating the
entry, validating state, copying data and header, and updating the buffer. This
serializes it with the worker receive path, `remove`, and `close`.

For audio:

1. Copy `min(entry->data_len, max_payload_size)` bytes from the front of the
   buffer.
2. Move remaining bytes to the front.
3. Decrease `entry->data_len` by the copied amount.
4. Return the copied amount.

This allows audio to be consumed in chunks smaller than the accumulated 200 ms.

For video:

1. If `max_payload_size < entry->data_len`, return `-1/ENOBUFS` without
   modifying caller outputs or the buffered frame.
2. Otherwise copy the complete frame, set `entry->data_len` to zero, and return
   the frame length.

On successful audio or video consumption, copy `latest_header` to the caller
and overwrite its `data_len` with the returned byte count.

## Compatibility

Existing public function signatures remain unchanged. Existing users that pass
a non-NULL callback retain the same SHM zero-copy callback path and payload
lifetime. They do not pay for per-entry polling buffers. Calling poll on such a
manager fails explicitly with `ENOTSUP`.

The only changed start behavior is that a NULL callback is now valid and selects
poll mode.

## Verification

Add focused tests for:

- Start accepts NULL callback while preserving existing non-NULL callback
  behavior.
- Poll argument validation, callback-mode `ENOTSUP`, missing entry `ENOENT`, and
  empty entry returning zero without changing outputs.
- First stable snapshot determines and locks the entry media type.
- Invalid media types and later media-type changes are discarded.
- Duplicate sequences do not append or replace data, including after the buffer
  has been fully consumed.
- Audio appends below capacity, rolls at 19,200 bytes, retains the newest bytes
  for an input at least as large as capacity, and supports partial poll reads
  with correct remainder movement.
- Audio poll returns the newest frame header and rewrites `header->data_len` to
  the returned aggregate length.
- Video replaces an older frame, consumes only as a complete frame, and remains
  intact after `ENOBUFS`.
- Removing entries and closing the manager release polling buffers.
- The project builds cleanly with the existing warning flags.
