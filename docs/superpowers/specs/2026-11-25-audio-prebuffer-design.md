# Audio burst pre-buffer

Audio manager writes copy payload and metadata into a FIFO owned by each write
entry. Video writes remain synchronous latest-frame commits. The existing worker
uses monotonic deadlines to flush audio every 10 ms, including while transport
recovery is in progress. Each deadline commits at most four consecutive frames
whose complete metadata matches and whose combined size fits the registered SHM
payload. Remaining frames wait for the next deadline. A delayed worker sends only
one batch per entry and schedules its next deadline from commit completion time.

The first frame waits for a 10 ms batching window. Queued payload and metadata are
owned copies; successful audio write means accepted into the queue, not delivered.
Notification errors occur asynchronously and are logged, without replaying an
already committed batch. SHM layout and signaling formats remain unchanged.
Both peers must allow a payload large enough for the desired aggregate; smaller
SHM objects simply aggregate fewer frames.

For live speech, the sender keeps at most 200 ms of queued PCM bytes and 64 input
frames, discarding oldest complete frames on overflow. A single input larger than
200 ms is accepted if it fits SHM, and replaces older queued data. Audio format
changes clear the old pending format before accepting the new format; otherwise
different metadata starts a separate batch. Positive sample rate and channels,
positive byte-aligned bit depth, and nonempty sample-aligned payload are required;
capacity arithmetic is checked. Remove/close discard and free pending audio.

Poll-mode receive audio capacity becomes 200 ms of PCM, still retaining the newest
bytes on overflow and allowing partial reads. Video caching is unchanged. This
smooths bursts; the existing single-slot SHM and best-effort notifications do not
provide an acknowledgement or a strict delivery guarantee.

Verification covers deferred copies, 3/4-frame aggregation, the fifth-frame
deadline, capacity-limited batches, metadata boundaries, queue overflow, invalid
audio input, multiple SHM names, recovery, removal/close, actual worker delivery,
and receive retention above 60 ms and at the 200 ms boundary. Existing video,
callback, state and recovery suites must continue passing.
