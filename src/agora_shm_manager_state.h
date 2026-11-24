#ifndef AGORA_SHM_MANAGER_STATE_H
#define AGORA_SHM_MANAGER_STATE_H

#include "agora_shm_ipc.h"

#include <limits.h>
#include <stddef.h>

#ifndef AGORA_SHM_MANAGER_STATE_ROOT
#define AGORA_SHM_MANAGER_STATE_ROOT "/var/lib/agora/shm-manager"
#endif
#define AGORA_SHM_MANAGER_STATE_MAX_NAMES 256u

typedef struct AgoraShmManagerState {
  int lock_fd;
  char dir_path[PATH_MAX];
  char names[AGORA_SHM_MANAGER_STATE_MAX_NAMES][AGORA_SHM_IPC_SHM_NAME_BYTES];
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
void agora_shm_manager_state_abandon(AgoraShmManagerState *state);

#endif /* AGORA_SHM_MANAGER_STATE_H */
