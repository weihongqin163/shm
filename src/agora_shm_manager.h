/**
 * created by:wei
 * copyright (c) 2026 Agora IO. All rights reserved.
 * date: 2026-04-15
 */

#ifndef AGORA_SHM_MANAGER_H
#define AGORA_SHM_MANAGER_H

#include "agora_shm_ipc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct AgoraShmManager AgoraShmManager;

/*
 * All fallible manager APIs return -errno on failure and also set errno to the
 * positive error code. Test for a negative return, rather than exactly -1.
 * start/add/remove/write return 0 on success; poll returns bytes or 0.
 */

typedef void (*agora_shm_manager_on_frame_fn)(const char *shm_name,
                                              const void *payload, size_t len,
                                              const AgoraShmIpcHeader *hdr,
                                              void *user);

/**
 * UDP on 127.0.0.1:port (localsocket). port must be non-zero (EINVAL
 * otherwise).
 *
 * server_mode: true = bind UDP server on port; false = UDP client connect to
 * port.
 *
 * localsock_max_clients / localsock_keepalive_ms are used only when
 * server_mode is true (passed to agora_localsock_server_create).
 *
 * on_frame: non-NULL selects callback mode; NULL selects poll mode.
 *
 * max_read_cap: maximum accepted SHM frame payload length for attach/dispatch;
 *               0 selects an implementation default.
 *
 * @note In on_frame, hdr and payload are valid only until the callback returns;
 *       payload points into the SHM mmap.
 *
 * Before creating the worker, start attempts to recover creator-owned SHM
 * objects left in unlocked state directories by terminated managers. State
 * initialization/recovery errors are logged and do not prevent startup; that
 * manager then runs without persistent ownership tracking. Live managers with
 * an intact state-file lock are skipped by recovery.
 *
 * Once started, the worker exits only on close. Receive/keepalive errors retry
 * after 20 ms; an invalid socket or three consecutive other socket errors
 * rebuild only the transport, retaining all SHM entries and ownership. Failed
 * rebuilds retry until close. Errors/recovery are logged to stderr without
 * rate limiting. Lost notifications and historical frames are not replayed.
 */
int agora_shm_manager_start(agora_shm_manager_on_frame_fn on_frame,
                            uint16_t port, bool server_mode,
                            size_t localsock_max_clients,
                            uint32_t localsock_keepalive_ms, void *user,
                            size_t max_read_cap, AgoraShmManager **out);

/**
 * Stops the worker thread, agora_shm_ipc_close on all read/write entries; for
 * each write-table slot created via agora_shm_ipc_open(is_creator=1), sends a
 * best-effort REMOVECMD and calls agora_shm_ipc_unlink after close; removes the
 * manager's persistent state files after successful cleanup; destroys
 * localsock and frees the manager.
 * Callers must stop new business API calls and wait for in-flight calls to
 * finish before close; never call close from on_frame. Close interrupts worker
 * retry/idle waits and joins recovery before releasing resources.
 * Pending audio pre-buffer data is discarded, not drained, on close.
 */
void agora_shm_manager_close(AgoraShmManager *m);

/**
 * Registers a writer-side SHM IPC: creates the object with
 * agora_shm_ipc_open(shm_name, max_payload_size, is_creator=1) and
 * agora_shm_ipc_writer_session_begin. max_payload_size must be non-zero
 * (EINVAL). Returns -EEXIST and sets errno if shm_name is already registered in
 * the write table or already present in the read (auto-attach) table.
 * Persistent ownership registration is best effort: a state error is logged,
 * but the newly created SHM remains registered in the live write table and add
 * succeeds. Such objects may not be recoverable after abnormal process exit.
 * For audio batching, allocate enough payload for the desired combined frames
 * (up to four input frames); the receiver's max_read_cap must also fit the SHM.
 */
int agora_shm_manager_add(AgoraShmManager *m, const char *shm_name,
                          size_t max_payload_size);

/**
 * Removes shm_name from the write table first. A write removal sends REMOVECMD,
 * closes and unlinks the creator-owned SHM, then removes its persistent state
 * record. If not found in the write table, removes only the local read entry;
 * read removal never sends REMOVECMD or unlinks SHM. Returns -ENOENT and sets
 * errno if absent in both. Local write cleanup still runs if notification
 * fails, and the first notification or SHM cleanup error is returned.
 * Persistent state-update errors are logged and do not fail removal.
 * Pending audio for a removed writer is discarded and freed.
 */
int agora_shm_manager_remove(AgoraShmManager *m, const char *shm_name);

/**
 * Submits data for a registered writer. data and meta must be non-NULL;
 * ENOENT if shm_name is missing, EMSGSIZE if len exceeds the SHM payload size.
 *
 * Audio: copies data and metadata into a per-name pre-buffer. Zero means queued,
 * not delivered; caller storage may be reused immediately. PCM metadata must
 * have positive sample_rate/channels and positive bits divisible by 8; len must
 * be nonzero and contain whole interleaved samples (otherwise EINVAL).
 * The worker flushes on 10 ms monotonic deadlines, merging at most four matching
 * input frames that fit the SHM payload into one commit and WRITECMD. Delayed
 * ticks do not cause repeated catch-up commits. The queue retains at most
 * 200 ms of PCM and 64 frames, dropping oldest whole frames on overflow. One
 * oversized-duration input is retained if it fits SHM. Format changes discard
 * queued data in the old format. Allocation/clock errors fail enqueue; later
 * write/notification errors are logged asynchronously. During recovery, audio
 * is still queued/flushed, without replaying already committed batches.
 *
 * Video/other media: immediately writes SHM, then sends WRITECMD. If server mode
 * has no UDP peers, signaling returns 0. Notification failure returns -errno
 * even though data has already been written; during recovery this is -EAGAIN.
 * There is no rollback or automatic replay. Switching to non-audio discards any
 * pending audio for that name. Single-slot SHM offers no delivery acknowledgement.
 */
int agora_shm_manager_write(AgoraShmManager *m, const char *shm_name,
                            const void *data, size_t len,
                            const AgoraShmIpcFrameMeta *meta);

/**
 * Copies buffered receive data for shm_name into caller-owned storage.
 *
 * Available only when the manager was started with on_frame == NULL. Audio may
 * be consumed partially; video is copied only when the complete frame fits.
 * Audio retains the latest 200 ms of PCM; a format change clears the old cache
 * and recalculates its capacity. Video retains only the latest complete frame.
 * On a positive return, header is the latest accepted frame header with
 * header->data_len set to the returned byte count.
 *
 * Buffered data remains readable during transport recovery. When no data can
 * be returned during recovery (including a missing entry), returns -EAGAIN
 * with errno=EAGAIN. In healthy operation a missing entry returns -ENOENT and
 * an empty entry returns 0. This API reads the cache, not SHM directly.
 *
 * Other failures return -errno with errno set to EINVAL, ENOTSUP, ENOBUFS, or
 * EOVERFLOW as applicable. These errors do not consume the buffered data.
 */
ssize_t agora_shm_manager_poll(AgoraShmManager *m, const char *shm_name,
                               AgoraShmIpcHeader *header, char *out,
                               size_t max_payload_size);

#endif /* AGORA_SHM_MANAGER_H */
