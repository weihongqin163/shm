/**
 * created by:wei
 * copyright (c) 2026 Agora IO. All rights reserved.
 * date: 2026-04-15
 */

#include "agora_shm_ipc_notify.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static socklen_t agora_sockaddr_un_len(const struct sockaddr_un *sa) {
  return (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                     strlen(sa->sun_path));
}

static int notify_bind_unix_dgram(int fd, const char *path, char *out_path,
                                  size_t out_path_cap) {
  if (!path || path[0] == '\0') {
    errno = EINVAL;
    return -1;
  }
  (void)unlink(path);

  struct sockaddr_un sa;
  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(sa.sun_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(sa.sun_path, path, strlen(path) + 1u);

  socklen_t slen = agora_sockaddr_un_len(&sa);
  if (bind(fd, (struct sockaddr *)&sa, slen) != 0) {
    return -1;
  }

  if (out_path != NULL && out_path_cap > 0u) {
    (void)strncpy(out_path, path, out_path_cap - 1u);
    out_path[out_path_cap - 1u] = '\0';
  }
  return 0;
}

int agora_shm_ipc_notify_writer_init(AgoraShmIpcNotify *n,
                                     const char *writer_bind_path,
                                     const char *reader_recv_path) {
  if (!n || !writer_bind_path || !reader_recv_path ||
      writer_bind_path[0] == '\0' || reader_recv_path[0] == '\0') {
    errno = EINVAL;
    return -1;
  }

  memset(n, 0, sizeof(*n));
  n->fd = -1;

  int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) {
    return -1;
  }

  if (notify_bind_unix_dgram(fd, writer_bind_path, n->bind_path,
                             sizeof(n->bind_path)) != 0) {
    int e = errno;
    (void)close(fd);
    errno = e;
    return -1;
  }

  memset(&n->peer, 0, sizeof(n->peer));
  n->peer.sun_family = AF_UNIX;
  if (strlen(reader_recv_path) >= sizeof(n->peer.sun_path)) {
    (void)close(fd);
    (void)unlink(writer_bind_path);
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(n->peer.sun_path, reader_recv_path, strlen(reader_recv_path) + 1u);
  n->peer_len = agora_sockaddr_un_len(&n->peer);

  n->has_bind_path = 1;
  n->is_writer = 1;
  n->fd = fd;
  return 0;
}

int agora_shm_ipc_notify_reader_init(AgoraShmIpcNotify *n,
                                     const char *reader_recv_path) {
  if (!n || !reader_recv_path || reader_recv_path[0] == '\0') {
    errno = EINVAL;
    return -1;
  }

  memset(n, 0, sizeof(*n));
  n->fd = -1;

  int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) {
    return -1;
  }

  if (notify_bind_unix_dgram(fd, reader_recv_path, n->bind_path,
                             sizeof(n->bind_path)) != 0) {
    int e = errno;
    (void)close(fd);
    errno = e;
    return -1;
  }

  n->has_bind_path = 1;
  n->is_writer = 0;
  n->fd = fd;
  return 0;
}

int agora_shm_ipc_notify_fd(const AgoraShmIpcNotify *n) {
  if (!n || n->fd < 0) {
    errno = EINVAL;
    return -1;
  }
  return n->fd;
}

void agora_shm_ipc_notify_fini(AgoraShmIpcNotify *n) {
  if (!n) {
    return;
  }
  if (n->fd >= 0) {
    (void)close(n->fd);
    n->fd = -1;
  }
  if (n->has_bind_path != 0 && n->bind_path[0] != '\0') {
    (void)unlink(n->bind_path);
    n->bind_path[0] = '\0';
    n->has_bind_path = 0;
  }
}

int agora_shm_ipc_notify_post_write(const AgoraShmIpc *ipc,
                                    const AgoraShmIpcNotify *n) {
  if (!ipc || !ipc->header || !n) {
    errno = EINVAL;
    return -1;
  }
  if (n->fd < 0 || n->is_writer == 0) {
    errno = EINVAL;
    return -1;
  }
  (void)sendto(n->fd, ipc->header, sizeof(AgoraShmIpcHeader), 0,
               (struct sockaddr *)&n->peer, n->peer_len);
  return 0;
}
