/**
 * created by:wei
 * copyright (c) 2026 Agora IO. All rights reserved.
 * date: 2026-04-13
 */

#include "agora_shm_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define AGORA_SHM_IPC_MAGIC 0xA601C0DEu
#define AGORA_SHM_IPC_VER 2u

_Static_assert(sizeof(AgoraShmIpcHeader) <= 256, "header size bound");

static void agora_shm_ipc_header_apply_meta(AgoraShmIpcHeader *h,
                                           const AgoraShmIpcFrameMeta *meta) {
  if (meta != NULL) {
    memcpy(h->user_id, meta->user_id, sizeof(h->user_id));
    h->media_type = meta->media_type;
    h->stream_type = meta->stream_type;
    h->width = meta->width;
    h->height = meta->height;
    h->sample_rate = meta->sample_rate;
    h->channels = meta->channels;
    h->bits = meta->bits;
  } else {
    memset(h->user_id, 0, sizeof(h->user_id));
    h->media_type = 0u;
    h->stream_type = 0u;
    h->width = 0;
    h->height = 0;
    h->sample_rate = 0;
    h->channels = 0;
    h->bits = 0;
  }
}

static void agora_shm_ipc_copy_meta_out(AgoraShmIpcFrameMeta *dst,
                                        const AgoraShmIpcHeader *h) {
  memcpy(dst->user_id, h->user_id, sizeof(dst->user_id));
  dst->media_type = h->media_type;
  dst->stream_type = h->stream_type;
  dst->width = h->width;
  dst->height = h->height;
  dst->sample_rate = h->sample_rate;
  dst->channels = h->channels;
  dst->bits = h->bits;
}

static size_t agora_shm_ipc_total_size(size_t payload_size) {
  return sizeof(AgoraShmIpcHeader) + payload_size;
}

static socklen_t agora_sockaddr_un_len(const struct sockaddr_un *sa) {
  return (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                     strlen(sa->sun_path));
}

int agora_shm_ipc_open(const char *shm_name, size_t payload_size, int is_creator,
                       AgoraShmIpc *out) {
  if (!shm_name || !out || payload_size == 0u) {
    errno = EINVAL;
    return -1;
  }
  if (payload_size > (size_t)UINT32_MAX) {
    errno = EINVAL;
    return -1;
  }

  memset(out, 0, sizeof(*out));
  out->fd = -1;

  const size_t total = agora_shm_ipc_total_size(payload_size);
  if (total < sizeof(AgoraShmIpcHeader)) {
    errno = EINVAL;
    return -1;
  }

  int fd = -1;
  int created = 0;

  if (is_creator) {
    fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0660);
    if (fd < 0) {
      return -1;
    }
    created = 1;
    if (ftruncate(fd, (off_t)total) != 0) {
      int e = errno;
      (void)close(fd);
      (void)shm_unlink(shm_name);
      errno = e;
      return -1;
    }
  } else {
    fd = shm_open(shm_name, O_RDWR, 0660);
    if (fd < 0) {
      return -1;
    }
  }

  struct stat st;
  if (fstat(fd, &st) != 0) {
    int e = errno;
    (void)close(fd);
    if (created) {
      (void)shm_unlink(shm_name);
    }
    errno = e;
    return -1;
  }
  if ((size_t)st.st_size < total) {
    (void)close(fd);
    if (created) {
      (void)shm_unlink(shm_name);
    }
    errno = EINVAL;
    return -1;
  }

  void *p = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    int e = errno;
    (void)close(fd);
    if (created) {
      (void)shm_unlink(shm_name);
    }
    errno = e;
    return -1;
  }

  AgoraShmIpcHeader *h = (AgoraShmIpcHeader *)p;
  uint8_t *pl = (uint8_t *)p + sizeof(AgoraShmIpcHeader);

  if (created) {
    memset(p, 0, total);
    h->magic = AGORA_SHM_IPC_MAGIC;
    h->version = AGORA_SHM_IPC_VER;
    h->payload_size = (uint32_t)payload_size;
    h->data_len = 0u;
    atomic_init(&h->seq, 0u);
  } else {
    if (h->magic != AGORA_SHM_IPC_MAGIC || h->version != AGORA_SHM_IPC_VER) {
      (void)munmap(p, total);
      (void)close(fd);
      errno = EPROTO;
      return -1;
    }
    if ((size_t)h->payload_size != payload_size) {
      (void)munmap(p, total);
      (void)close(fd);
      errno = EINVAL;
      return -1;
    }
  }

  out->region_base = p;
  out->header = h;
  out->payload = pl;
  out->map_size = total;
  out->payload_size = payload_size;
  out->fd = fd;
  out->creator = created;
  return 0;
}

void agora_shm_ipc_close(AgoraShmIpc *ctx) {
  if (!ctx || !ctx->region_base) {
    return;
  }
  (void)munmap(ctx->region_base, ctx->map_size);
  ctx->region_base = NULL;
  ctx->header = NULL;
  ctx->payload = NULL;
  ctx->map_size = 0;
  ctx->payload_size = 0;
  if (ctx->fd >= 0) {
    (void)close(ctx->fd);
  }
  ctx->fd = -1;
}

int agora_shm_ipc_unlink(const char *shm_name) {
  if (!shm_name) {
    errno = EINVAL;
    return -1;
  }
  return shm_unlink(shm_name);
}

int agora_shm_ipc_writer_session_begin(AgoraShmIpc *ctx) {
  if (!ctx || !ctx->header) {
    errno = EINVAL;
    return -1;
  }
  AgoraShmIpcHeader *h = ctx->header;
  h->data_len = 0u;
  agora_shm_ipc_header_apply_meta(h, NULL);
  atomic_store_explicit(&h->seq, 0u, memory_order_release);
  return 0;
}

int agora_shm_ipc_write(AgoraShmIpc *ctx, const void *data, size_t len,
                        const AgoraShmIpcFrameMeta *meta,
                        const AgoraShmIpcNotify *notify) {
  if (!ctx || !ctx->header || !ctx->payload || !data) {
    errno = EINVAL;
    return -1;
  }
  if (len > ctx->payload_size) {
    errno = EMSGSIZE;
    return -1;
  }

  AgoraShmIpcHeader *h = ctx->header;
  (void)atomic_fetch_add_explicit(&h->seq, 1u, memory_order_acq_rel);
  agora_shm_ipc_header_apply_meta(h, meta);
  memcpy(ctx->payload, data, len);
  h->data_len = (uint32_t)len;
  atomic_thread_fence(memory_order_release);
  (void)atomic_fetch_add_explicit(&h->seq, 1u, memory_order_release);

  if (notify != NULL && notify->fd >= 0 && notify->is_writer != 0) {
    char b = 1;
    (void)sendto(notify->fd, &b, 1, 0, (struct sockaddr *)&notify->peer,
                 notify->peer_len);
  }
  return 0;
}

int agora_shm_ipc_read(AgoraShmIpc *ctx, void *buf, size_t cap, size_t *out_len,
                       AgoraShmIpcFrameMeta *out_meta) {
  if (!ctx || !ctx->header || !ctx->payload || !buf || !out_len) {
    errno = EINVAL;
    return -1;
  }

  AgoraShmIpcHeader *h = ctx->header;

  const unsigned k_max_seqlock_spins = 65536u;
  for (unsigned spin = 0u; spin < k_max_seqlock_spins; ++spin) {
    unsigned s1 =
        atomic_load_explicit(&h->seq, memory_order_acquire);
    if ((s1 & 1u) != 0u) {
      continue;
    }

    uint32_t len = h->data_len;
    if (len == 0u) {
      errno = EAGAIN;
      return -1;
    }
    if (len > h->payload_size) {
      errno = EIO;
      return -1;
    }
    if (cap < (size_t)len) {
      errno = ENOBUFS;
      return -1;
    }

    memcpy(buf, ctx->payload, (size_t)len);
    if (out_meta != NULL) {
      agora_shm_ipc_copy_meta_out(out_meta, h);
    }

    unsigned s2 =
        atomic_load_explicit(&h->seq, memory_order_acquire);
    if (s1 == s2 && (s2 & 1u) == 0u) {
      *out_len = (size_t)len;
      return 0;
    }
    /* Concurrent write or writer restarted; retry for a stable snapshot. */
  }

  errno = EAGAIN;
  return -1;
}

static int notify_bind_unix_dgram(int fd, const char *path,
                                  char *out_path, size_t out_path_cap) {
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
