/**
 * created by:wei
 * copyright (c) 2026 Agora IO. All rights reserved.
 * date: 2026-04-15
 *
 * Unix datagram notify path for Agora SHM IPC. Kept separate from
 * agora_shm_ipc so the SHM layer does not depend on AgoraShmIpcNotify.
 */

#ifndef AGORA_SHM_IPC_NOTIFY_H
#define AGORA_SHM_IPC_NOTIFY_H

#include "agora_shm_ipc.h"

#include <sys/socket.h>
#include <sys/un.h>

typedef struct AgoraShmIpcNotify {
  int fd;
  int is_writer;
  struct sockaddr_un peer;
  socklen_t peer_len;
  char bind_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
  int has_bind_path;
} AgoraShmIpcNotify;

/** Writer: bind writer_bind_path; peer is reader_recv_path for sendto. */
int agora_shm_ipc_notify_writer_init(AgoraShmIpcNotify *n,
                                     const char *writer_bind_path,
                                     const char *reader_recv_path);

/** Reader: bind reader_recv_path; use returned fd with poll/recv. */
int agora_shm_ipc_notify_reader_init(AgoraShmIpcNotify *n,
                                     const char *reader_recv_path);

/** Socket fd for poll(2); valid after successful *_notify_*_init. */
int agora_shm_ipc_notify_fd(const AgoraShmIpcNotify *n);

/** Closes fd and unlinks the bound path if this side called bind. */
void agora_shm_ipc_notify_fini(AgoraShmIpcNotify *n);

/**
 * Upper layer: after a successful agora_shm_ipc_write, send a header snapshot
 * to the reader (best-effort; sendto errors are ignored, errno may be set).
 */
int agora_shm_ipc_notify_post_write(const AgoraShmIpc *ipc,
                                    const AgoraShmIpcNotify *n);

#endif /* AGORA_SHM_IPC_NOTIFY_H */
