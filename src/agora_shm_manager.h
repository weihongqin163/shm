/**
 * created by:wei
 * copyright (c) 2026 Agora IO. All rights reserved.
 * date: 2026-04-15
 */

#ifndef AGORA_SHM_MANAGER_H
#define AGORA_SHM_MANAGER_H

#include "agora_shm_ipc.h"

#include <stddef.h>

typedef struct AgoraShmManager AgoraShmManager;

typedef enum AgoraShmManagerRole {
  AGORA_SHM_MANAGER_ROLE_READ = 0,
  AGORA_SHM_MANAGER_ROLE_WRITE = 1,
} AgoraShmManagerRole;

typedef void (*agora_shm_manager_on_frame_fn)(const char *shm_name,
                                              const void *payload, size_t len,
                                              const AgoraShmIpcFrameMeta *meta,
                                              void *user);

/**
 * Starts one manager: allocates state, binds reader notify, spawns worker
 * thread. All writers must sendto the same reader_recv_path.
 *
 * @param max_payload_size  Worker read buffer size; if 0, a default is used.
 * @param out               Set to new manager; undefined on failure.
 * @return 0 on success, -1 on error (errno set).
 */
int agora_shm_manager_start(const char *reader_recv_path,
                            agora_shm_manager_on_frame_fn on_frame, void *user,
                            size_t max_payload_size, AgoraShmManager **out);

/** Stops worker, closes all registered IPC, fini notify, frees manager. */
void agora_shm_manager_close(AgoraShmManager *m);

/**
 * Registers an opened AgoraShmIpc under shm_name. Same key must not exist.
 * The struct is copied; do not agora_shm_ipc_close the caller's copy until
 * agora_shm_manager_remove or agora_shm_manager_close (manager owns the slot).
 *
 * @return 0 on success, -1 with errno EEXIST if shm_name already registered,
 *         ENOMEM if table full.
 */
int agora_shm_manager_add(AgoraShmManager *m, const char *shm_name,
                          AgoraShmManagerRole role, const AgoraShmIpc *ipc);

/** Removes entry and calls agora_shm_ipc_close on its ctx. */
int agora_shm_manager_remove(AgoraShmManager *m, const char *shm_name);

#endif /* AGORA_SHM_MANAGER_H */
