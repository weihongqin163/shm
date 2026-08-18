/**
 * created by:wei
 * copyright (c) 2026 Agora IO. All rights reserved.
 * date: 2026-04-15
 */

#define _POSIX_C_SOURCE 200809L

#include "agora_shm_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define AGORA_SHM_IPC_MAGIC 0xA601C0DEu
#define AGORA_SHM_IPC_VER 3u

_Static_assert(sizeof(AgoraShmIpcHeader) <= 256, "header size bound");

static void agora_shm_ipc_header_set_shm_name(AgoraShmIpcHeader *h,
                                             const char *shm_name) {
  if (h == NULL) {
    return;
  }
  memset(h->frame_meta.shm_name, 0, sizeof(h->frame_meta.shm_name));
  if (shm_name != NULL) {
    (void)strncpy(h->frame_meta.shm_name, shm_name,
                  sizeof(h->frame_meta.shm_name) - 1u);
  }
}

static void agora_shm_ipc_header_apply_meta(AgoraShmIpcHeader *h,
                                           const AgoraShmIpcFrameMeta *meta) {
  if (meta != NULL) {
    h->frame_meta = *meta;
  } else {
    memset(&h->frame_meta, 0, sizeof(h->frame_meta));
  }
}

static void agora_shm_ipc_copy_header_snapshot(AgoraShmIpcHeader *dst,
                                              const AgoraShmIpcHeader *src) {
  dst->magic = src->magic;
  dst->version = src->version;
  dst->payload_size = src->payload_size;
  dst->data_len = src->data_len;
  dst->frame_meta = src->frame_meta;
  atomic_store_explicit(
      &dst->seq,
      atomic_load_explicit(&src->seq, memory_order_relaxed),
      memory_order_relaxed);
}

static size_t agora_shm_ipc_total_size(size_t payload_size) {
  return sizeof(AgoraShmIpcHeader) + payload_size;
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

  agora_shm_ipc_header_set_shm_name(h, shm_name);

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
                        const AgoraShmIpcFrameMeta *meta) {
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
  return 0;
}

int agora_shm_ipc_read(AgoraShmIpc *ctx, void **buf, size_t cap, size_t *out_len,
                       AgoraShmIpcHeader *out_hdr) {
  if (!ctx || !ctx->header || !ctx->payload || !buf || !out_len) {
    errno = EINVAL;
    return -1;
  }

  AgoraShmIpcHeader *h = ctx->header;
  const int copy_mode = (*buf != NULL);
  void *const copy_dst = copy_mode ? *buf : NULL;

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

    if (copy_mode != 0) {
      memcpy(copy_dst, ctx->payload, (size_t)len);
    }
    if (out_hdr != NULL) {
      agora_shm_ipc_copy_header_snapshot(out_hdr, h);
    }

    unsigned s2 =
        atomic_load_explicit(&h->seq, memory_order_acquire);
    if (s1 == s2 && (s2 & 1u) == 0u) {
      *out_len = (size_t)len;
      if (copy_mode == 0) {
        *buf = ctx->payload;
      }
      return 0;
    }
    /* Concurrent write or writer restarted; retry for a stable snapshot. */
  }

  errno = EAGAIN;
  return -1;
}
