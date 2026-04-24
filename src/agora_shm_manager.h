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

typedef struct AgoraShmManager AgoraShmManager;

typedef void (*agora_shm_manager_on_frame_fn)(const char *shm_name,
                                              const void *payload, size_t len,
                                              const AgoraShmIpcHeader *hdr,
                                              void *user);

/**
 * UDP on 127.0.0.1:port (localsocket). port must be non-zero (EINVAL otherwise).
 *
 * server_mode: true = bind UDP server on port; false = UDP client connect to
 * port.
 *
 * localsock_max_clients / localsock_keepalive_ms are used only when
 * server_mode is true (passed to agora_localsock_server_create).
 *
 * max_read_cap: maximum accepted SHM frame payload length for attach/dispatch;
 *               0 selects an implementation default.
 *
 * @note In on_frame, hdr and payload are valid only until the callback returns;
 *       payload points into the SHM mmap.
 */
int agora_shm_manager_start(agora_shm_manager_on_frame_fn on_frame, uint16_t port,
                            bool server_mode, size_t localsock_max_clients,
                            uint32_t localsock_keepalive_ms, void *user,
                            size_t max_read_cap, AgoraShmManager **out);

/**
 * Stops the worker thread, agora_shm_ipc_close on all read/write entries; for
 * each write-table slot created via agora_shm_ipc_open(is_creator=1),
 * agora_shm_ipc_unlink after close; then destroys localsock and frees the manager.
 */
void agora_shm_manager_close(AgoraShmManager *m);

/**
 * Registers a writer-side SHM IPC: creates the object with
 * agora_shm_ipc_open(shm_name, max_payload_size, is_creator=1) and
 * agora_shm_ipc_writer_session_begin. max_payload_size must be non-zero
 * (EINVAL). Returns -1 with errno EEXIST if shm_name is already registered in
 * the write table or already present in the read (auto-attach) table.
 */
int agora_shm_manager_add(AgoraShmManager *m, const char *shm_name,
                          size_t max_payload_size);

/**
 * Removes shm_name from the write table first; if not found, removes from the
 * read (auto-attach) table. Returns -1 with errno ENOENT if absent in both.
 */
int agora_shm_manager_remove(AgoraShmManager *m, const char *shm_name);

/**
 * Writes to a SHM IPC registered in the write table, then sends a WRITECMD
 * localsocket datagram (header + meta). meta must be non-NULL (EINVAL).
 * ENOENT if shm_name is not in the write table. If server mode has no UDP
 * peers yet, signaling succeeds with return 0.
 */
int agora_shm_manager_write(AgoraShmManager *m, const char *shm_name,
                            const void *data, size_t len,
                            const AgoraShmIpcFrameMeta *meta);

#endif /* AGORA_SHM_MANAGER_H */
