/**
 * created by:wei
 * copyright (c) 2026 Agora IO. All rights reserved.
 * date: 2026-04-15
 */

#include "agora_shm_manager.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AGORA_SHM_MANAGER_MAX_ENTRIES 64
#define AGORA_SHM_MANAGER_DEFAULT_READ_CAP (256u * 1024u)

/* Must match agora_shm_ipc.c */
#define AGORA_SHM_IPC_MAGIC_EXPECT 0xA601C0DEu
#define AGORA_SHM_IPC_VER_EXPECT 2u

typedef struct AgoraShmManagerEntry {
  int in_use;
  int auto_read;
  AgoraShmManagerRole role;
  char shm_name[AGORA_SHM_IPC_SHM_NAME_BYTES];
  AgoraShmIpc ipc;
} AgoraShmManagerEntry;

struct AgoraShmManager {
  pthread_mutex_t lock;
  atomic_int stop;
  pthread_t worker;
  int worker_started;
  AgoraShmIpcNotify notify;
  AgoraShmManagerEntry entries[AGORA_SHM_MANAGER_MAX_ENTRIES];
  agora_shm_manager_on_frame_fn on_frame;
  void *user;
  size_t read_cap;
  uint8_t *read_scratch;
};

static int find_entry_by_name(AgoraShmManager *m, const char *shm_name) {
  for (int i = 0; i < AGORA_SHM_MANAGER_MAX_ENTRIES; ++i) {
    if (m->entries[i].in_use != 0 &&
        strcmp(m->entries[i].shm_name, shm_name) == 0) {
      return i;
    }
  }
  return -1;
}

static int find_free_slot(AgoraShmManager *m) {
  for (int i = 0; i < AGORA_SHM_MANAGER_MAX_ENTRIES; ++i) {
    if (m->entries[i].in_use == 0) {
      return i;
    }
  }
  return -1;
}

static void clear_entry(AgoraShmManagerEntry *e) {
  if (e->in_use != 0) {
    agora_shm_ipc_close(&e->ipc);
  }
  memset(e, 0, sizeof(*e));
}

static void *agora_shm_manager_worker(void *arg) {
  AgoraShmManager *m = (AgoraShmManager *)arg;
  const int nfd = agora_shm_ipc_notify_fd(&m->notify);

  for (;;) {
    if (atomic_load_explicit(&m->stop, memory_order_acquire) != 0) {
      break;
    }

    struct pollfd pfd;
    pfd.fd = nfd;
    pfd.events = POLLIN;
    int pr = poll(&pfd, 1, 200);
    if (pr < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (pr == 0) {
      continue;
    }
    if ((pfd.revents & POLLIN) == 0) {
      continue;
    }

    alignas(AgoraShmIpcHeader) unsigned char notify_buf[sizeof(AgoraShmIpcHeader)];
    ssize_t nr = recv(nfd, notify_buf, sizeof(notify_buf), 0);
    if (nr < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if ((size_t)nr < sizeof(AgoraShmIpcHeader)) {
      continue;
    }

    const AgoraShmIpcHeader *hdr = (const AgoraShmIpcHeader *)notify_buf;
    if (hdr->magic != AGORA_SHM_IPC_MAGIC_EXPECT ||
        hdr->version != AGORA_SHM_IPC_VER_EXPECT) {
      continue;
    }

    const uint32_t ps = hdr->payload_size;
    if (ps == 0u || ps > (uint32_t)m->read_cap) {
      continue;
    }

    char shm_key[AGORA_SHM_IPC_SHM_NAME_BYTES];
    memcpy(shm_key, hdr->shm_name, sizeof(shm_key));
    shm_key[sizeof(shm_key) - 1u] = '\0';
    if (shm_key[0] == '\0') {
      continue;
    }

    pthread_mutex_lock(&m->lock);

    int idx = find_entry_by_name(m, shm_key);
    if (idx < 0) {
      int slot = find_free_slot(m);
      if (slot < 0) {
        pthread_mutex_unlock(&m->lock);
        continue;
      }

      AgoraShmIpc ctx;
      memset(&ctx, 0, sizeof(ctx));
      if (agora_shm_ipc_open(shm_key, (size_t)ps, 0, &ctx) != 0) {
        pthread_mutex_unlock(&m->lock);
        continue;
      }

      AgoraShmManagerEntry *e = &m->entries[slot];
      memset(e, 0, sizeof(*e));
      e->in_use = 1;
      e->auto_read = 1;
      e->role = AGORA_SHM_MANAGER_ROLE_READ;
      memcpy(e->shm_name, shm_key, sizeof(e->shm_name));
      e->ipc = ctx;
      idx = slot;
    } else {
      AgoraShmManagerEntry *e = &m->entries[idx];
      if ((uint32_t)e->ipc.payload_size != ps) {
        pthread_mutex_unlock(&m->lock);
        continue;
      }
    }

    AgoraShmManagerEntry *e = &m->entries[idx];
    AgoraShmIpc *ipc = &e->ipc;

    size_t out_len = 0u;
    AgoraShmIpcFrameMeta meta;
    memset(&meta, 0, sizeof(meta));
    int rr = agora_shm_ipc_read(ipc, m->read_scratch, m->read_cap, &out_len,
                                &meta);

    char shm_name_cb[AGORA_SHM_IPC_SHM_NAME_BYTES];
    memcpy(shm_name_cb, e->shm_name, sizeof(shm_name_cb));
    shm_name_cb[sizeof(shm_name_cb) - 1u] = '\0';

    agora_shm_manager_on_frame_fn cb = m->on_frame;
    void *user = m->user;

    pthread_mutex_unlock(&m->lock);

    if (rr == 0 && cb != NULL) {
      cb(shm_name_cb, m->read_scratch, out_len, &meta, user);
    }
  }

  return NULL;
}

int agora_shm_manager_start(const char *reader_recv_path,
                            agora_shm_manager_on_frame_fn on_frame, void *user,
                            size_t max_payload_size, AgoraShmManager **out) {
  if (!reader_recv_path || reader_recv_path[0] == '\0' || !out) {
    errno = EINVAL;
    return -1;
  }

  size_t cap = max_payload_size;
  if (cap == 0u) {
    cap = AGORA_SHM_MANAGER_DEFAULT_READ_CAP;
  }

  AgoraShmManager *m = (AgoraShmManager *)calloc(1, sizeof(*m));
  if (m == NULL) {
    return -1;
  }

  m->read_scratch = (uint8_t *)malloc(cap);
  if (m->read_scratch == NULL) {
    free(m);
    return -1;
  }
  m->read_cap = cap;
  m->on_frame = on_frame;
  m->user = user;
  atomic_init(&m->stop, 0);

  if (pthread_mutex_init(&m->lock, NULL) != 0) {
    int e = errno;
    free(m->read_scratch);
    free(m);
    errno = e;
    return -1;
  }

  memset(&m->notify, 0, sizeof(m->notify));
  if (agora_shm_ipc_notify_reader_init(&m->notify, reader_recv_path) != 0) {
    int e = errno;
    (void)pthread_mutex_destroy(&m->lock);
    free(m->read_scratch);
    free(m);
    errno = e;
    return -1;
  }

  if (pthread_create(&m->worker, NULL, agora_shm_manager_worker, m) != 0) {
    int e = errno;
    agora_shm_ipc_notify_fini(&m->notify);
    (void)pthread_mutex_destroy(&m->lock);
    free(m->read_scratch);
    free(m);
    errno = e;
    return -1;
  }
  m->worker_started = 1;

  *out = m;
  return 0;
}

void agora_shm_manager_close(AgoraShmManager *m) {
  if (m == NULL) {
    return;
  }

  atomic_store_explicit(&m->stop, 1, memory_order_release);

  if (m->worker_started != 0) {
    (void)pthread_join(m->worker, NULL);
    m->worker_started = 0;
  }

  (void)pthread_mutex_lock(&m->lock);
  for (int i = 0; i < AGORA_SHM_MANAGER_MAX_ENTRIES; ++i) {
    clear_entry(&m->entries[i]);
  }
  (void)pthread_mutex_unlock(&m->lock);

  agora_shm_ipc_notify_fini(&m->notify);
  (void)pthread_mutex_destroy(&m->lock);

  free(m->read_scratch);
  m->read_scratch = NULL;
  free(m);
}

int agora_shm_manager_add(AgoraShmManager *m, const char *shm_name,
                          AgoraShmManagerRole role, const AgoraShmIpc *ipc) {
  if (!m || !shm_name || shm_name[0] == '\0' || !ipc) {
    errno = EINVAL;
    return -1;
  }

  (void)pthread_mutex_lock(&m->lock);

  if (find_entry_by_name(m, shm_name) >= 0) {
    (void)pthread_mutex_unlock(&m->lock);
    errno = EEXIST;
    return -1;
  }

  int slot = find_free_slot(m);
  if (slot < 0) {
    (void)pthread_mutex_unlock(&m->lock);
    errno = ENOMEM;
    return -1;
  }

  AgoraShmManagerEntry *e = &m->entries[slot];
  memset(e, 0, sizeof(*e));
  e->in_use = 1;
  e->auto_read = 0;
  e->role = role;
  (void)strncpy(e->shm_name, shm_name, sizeof(e->shm_name) - 1u);
  e->shm_name[sizeof(e->shm_name) - 1u] = '\0';
  memcpy(&e->ipc, ipc, sizeof(e->ipc));

  (void)pthread_mutex_unlock(&m->lock);
  return 0;
}

int agora_shm_manager_remove(AgoraShmManager *m, const char *shm_name) {
  if (!m || !shm_name || shm_name[0] == '\0') {
    errno = EINVAL;
    return -1;
  }

  (void)pthread_mutex_lock(&m->lock);

  int idx = find_entry_by_name(m, shm_name);
  if (idx < 0) {
    (void)pthread_mutex_unlock(&m->lock);
    errno = ENOENT;
    return -1;
  }

  clear_entry(&m->entries[idx]);

  (void)pthread_mutex_unlock(&m->lock);
  return 0;
}
