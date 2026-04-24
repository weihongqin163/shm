/**
 * created by:wei
 * copyright (c) 2026 Agora IO. All rights reserved.
 * date: 2026-04-15
 */

#include "agora_shm_manager.h"

#include "agora_localsock.h"
#include "agora_shm_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define AGORA_SHM_MANAGER_MAX_ENTRIES 64
#define AGORA_SHM_MANAGER_DEFAULT_READ_CAP (256u * 1024u)
#define AGORA_SHM_MANAGER_UDP_CAP 65536u

#define AGORA_SHM_IPC_MAGIC_EXPECT 0xA601C0DEu
#define AGORA_SHM_IPC_VER_EXPECT 2u

typedef struct AgoraShmManagerReadEntry {
  int in_use;
  int auto_read;
  char shm_name[AGORA_SHM_IPC_SHM_NAME_BYTES];
  AgoraShmIpc ipc;
} AgoraShmManagerReadEntry;

typedef struct AgoraShmManagerWriteEntry {
  int in_use;
  char shm_name[AGORA_SHM_IPC_SHM_NAME_BYTES];
  AgoraShmIpc ipc;
} AgoraShmManagerWriteEntry;

struct AgoraShmManager {
  pthread_mutex_t lock;
  atomic_int stop;
  pthread_t worker;
  int worker_started;
  bool server_mode;
  agora_localsock_server *srv;
  agora_localsock_client *cli;
  AgoraShmManagerReadEntry read_entries[AGORA_SHM_MANAGER_MAX_ENTRIES];
  AgoraShmManagerWriteEntry write_entries[AGORA_SHM_MANAGER_MAX_ENTRIES];
  agora_shm_manager_on_frame_fn on_frame;
  void *user;
  size_t read_cap;
  uint8_t *udp_recv;
};

static int mono_now_ns(uint64_t *out) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return -1;
  }
  *out = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  return 0;
}

static int find_read_entry_by_name(AgoraShmManager *m, const char *shm_name) {
  for (int i = 0; i < AGORA_SHM_MANAGER_MAX_ENTRIES; ++i) {
    if (m->read_entries[i].in_use != 0 &&
        strcmp(m->read_entries[i].shm_name, shm_name) == 0) {
      return i;
    }
  }
  return -1;
}

static int find_free_read_slot(AgoraShmManager *m) {
  for (int i = 0; i < AGORA_SHM_MANAGER_MAX_ENTRIES; ++i) {
    if (m->read_entries[i].in_use == 0) {
      return i;
    }
  }
  return -1;
}

static void clear_read_entry(AgoraShmManagerReadEntry *e) {
  if (e->in_use != 0) {
    agora_shm_ipc_close(&e->ipc);
  }
  memset(e, 0, sizeof(*e));
}

static int find_write_entry_by_name(AgoraShmManager *m, const char *shm_name) {
  for (int i = 0; i < AGORA_SHM_MANAGER_MAX_ENTRIES; ++i) {
    if (m->write_entries[i].in_use != 0 &&
        strcmp(m->write_entries[i].shm_name, shm_name) == 0) {
      return i;
    }
  }
  return -1;
}

static int find_free_write_slot(AgoraShmManager *m) {
  for (int i = 0; i < AGORA_SHM_MANAGER_MAX_ENTRIES; ++i) {
    if (m->write_entries[i].in_use == 0) {
      return i;
    }
  }
  return -1;
}

static void clear_write_entry(AgoraShmManagerWriteEntry *e) {
  if (e->in_use != 0) {
    agora_shm_ipc_close(&e->ipc);
  }
  memset(e, 0, sizeof(*e));
}

static int manager_probe_shm_payload_size(const char *shm_name, uint32_t *out_ps) {
  if (shm_name == NULL || out_ps == NULL) {
    errno = EINVAL;
    return -1;
  }
  int fd = shm_open(shm_name, O_RDONLY, 0);
  if (fd < 0) {
    return -1;
  }
  size_t map_len = sizeof(AgoraShmIpcHeader);
  void *p = mmap(NULL, map_len, PROT_READ, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    int e = errno;
    (void)close(fd);
    errno = e;
    return -1;
  }
  const AgoraShmIpcHeader *h = (const AgoraShmIpcHeader *)p;
  if (h->magic != AGORA_SHM_IPC_MAGIC_EXPECT || h->version != AGORA_SHM_IPC_VER_EXPECT) {
    (void)munmap(p, map_len);
    (void)close(fd);
    errno = EIO;
    return -1;
  }
  *out_ps = h->payload_size;
  (void)munmap(p, map_len);
  (void)close(fd);
  return 0;
}

static void manager_dispatch_ipc_header(AgoraShmManager *m,
                                        const AgoraShmIpcHeader *hdr) {
  if (hdr->magic != AGORA_SHM_IPC_MAGIC_EXPECT ||
      hdr->version != AGORA_SHM_IPC_VER_EXPECT) {
    return;
  }

  const uint32_t ps = hdr->payload_size;
  if (ps == 0u || ps > (uint32_t)m->read_cap) {
    return;
  }

  char shm_key[AGORA_SHM_IPC_SHM_NAME_BYTES];
  memcpy(shm_key, hdr->shm_name, sizeof(shm_key));
  shm_key[sizeof(shm_key) - 1u] = '\0';
  if (shm_key[0] == '\0') {
    return;
  }

  pthread_mutex_lock(&m->lock);

  int idx = find_read_entry_by_name(m, shm_key);
  if (idx < 0) {
    if (find_write_entry_by_name(m, shm_key) >= 0) {
      pthread_mutex_unlock(&m->lock);
      return;
    }
    int slot = find_free_read_slot(m);
    if (slot < 0) {
      pthread_mutex_unlock(&m->lock);
      return;
    }

    AgoraShmIpc ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (agora_shm_ipc_open(shm_key, (size_t)ps, 0, &ctx) != 0) {
      pthread_mutex_unlock(&m->lock);
      return;
    }

    AgoraShmManagerReadEntry *e = &m->read_entries[slot];
    memset(e, 0, sizeof(*e));
    e->in_use = 1;
    e->auto_read = 1;
    memcpy(e->shm_name, shm_key, sizeof(e->shm_name));
    e->ipc = ctx;
    idx = slot;
  } else {
    AgoraShmManagerReadEntry *e = &m->read_entries[idx];
    if ((uint32_t)e->ipc.payload_size != ps) {
      pthread_mutex_unlock(&m->lock);
      return;
    }
  }

  AgoraShmManagerReadEntry *e = &m->read_entries[idx];
  AgoraShmIpc *ipc = &e->ipc;

  size_t out_len = 0u;
  AgoraShmIpcHeader snap;
  void *payload_ref = NULL;
  int rr = agora_shm_ipc_read(ipc, &payload_ref, m->read_cap, &out_len, &snap);

  char shm_name_cb[AGORA_SHM_IPC_SHM_NAME_BYTES];
  memcpy(shm_name_cb, e->shm_name, sizeof(shm_name_cb));
  shm_name_cb[sizeof(shm_name_cb) - 1u] = '\0';

  agora_shm_manager_on_frame_fn cb = m->on_frame;
  void *user = m->user;

  pthread_mutex_unlock(&m->lock);

  if (rr == 0 && cb != NULL) {
    cb(shm_name_cb, payload_ref, out_len, &snap, user);
  }
}

static void manager_dispatch_with_ps(AgoraShmManager *m, const char *shm_key,
                                     uint32_t ps) {
  if (ps == 0u || ps > (uint32_t)m->read_cap) {
    return;
  }
  AgoraShmIpcHeader syn;
  memset(&syn, 0, sizeof(syn));
  syn.magic = AGORA_SHM_IPC_MAGIC_EXPECT;
  syn.version = AGORA_SHM_IPC_VER_EXPECT;
  syn.payload_size = ps;
  (void)strncpy(syn.shm_name, shm_key, sizeof(syn.shm_name) - 1u);
  syn.shm_name[sizeof(syn.shm_name) - 1u] = '\0';
  manager_dispatch_ipc_header(m, &syn);
}

static void manager_dispatch_writecmd(AgoraShmManager *m,
                                      const AgoraShmIpcFrameMeta *meta) {
  char shm_key[AGORA_SHM_IPC_SHM_NAME_BYTES];
  memcpy(shm_key, meta->shm_name, sizeof(shm_key));
  shm_key[sizeof(shm_key) - 1u] = '\0';
  if (shm_key[0] == '\0') {
    return;
  }

  uint32_t ps = 0u;

  pthread_mutex_lock(&m->lock);
  if (find_write_entry_by_name(m, shm_key) >= 0) {
    pthread_mutex_unlock(&m->lock);
    return;
  }
  int ridx = find_read_entry_by_name(m, shm_key);
  if (ridx >= 0) {
    ps = (uint32_t)m->read_entries[ridx].ipc.payload_size;
  }
  pthread_mutex_unlock(&m->lock);

  if (ridx < 0) {
    if (manager_probe_shm_payload_size(shm_key, &ps) != 0) {
      return;
    }
  }

  manager_dispatch_with_ps(m, shm_key, ps);
}

static void manager_on_udp_datagram(AgoraShmManager *m, const uint8_t *data,
                                    size_t len) {
  if (len < AGORA_LOCALSOCK_HEADER_BYTES) {
    return;
  }
  agora_localsock_header lh;
  memcpy(&lh, data, sizeof(lh));
  if (lh.magic != AGORA_LOCALSOCK_MAGIC || lh.ver != (uint16_t)AGORA_LOCALSOCK_VER) {
    return;
  }
  size_t total = (size_t)AGORA_LOCALSOCK_HEADER_BYTES + (size_t)lh.payload_len;
  if (total != len) {
    return;
  }

  if (lh.msg_type == (uint16_t)AGORA_LOCALSOCK_MSG_APP) {
    if (lh.payload_len != (uint32_t)sizeof(AgoraShmIpcHeader)) {
      return;
    }
    alignas(AgoraShmIpcHeader) unsigned char hdr_storage[sizeof(AgoraShmIpcHeader)];
    memcpy(hdr_storage, data + AGORA_LOCALSOCK_HEADER_BYTES, sizeof(hdr_storage));
    const AgoraShmIpcHeader *hdr = (const AgoraShmIpcHeader *)hdr_storage;
    manager_dispatch_ipc_header(m, hdr);
    return;
  }

  if (lh.msg_type == (uint16_t)AGORA_LOCALSOCK_MSG_WRITECMD) {
    if (lh.payload_len != (uint32_t)sizeof(AgoraShmIpcFrameMeta)) {
      return;
    }
    alignas(AgoraShmIpcFrameMeta) unsigned char meta_storage[sizeof(AgoraShmIpcFrameMeta)];
    memcpy(meta_storage, data + AGORA_LOCALSOCK_HEADER_BYTES, sizeof(meta_storage));
    const AgoraShmIpcFrameMeta *fm = (const AgoraShmIpcFrameMeta *)meta_storage;
    manager_dispatch_writecmd(m, fm);
  }
}

static void *agora_shm_manager_worker_server(void *arg) {
  AgoraShmManager *m = (AgoraShmManager *)arg;

  for (;;) {
    if (atomic_load_explicit(&m->stop, memory_order_acquire) != 0) {
      break;
    }
    uint8_t srv_poll_buf[AGORA_SHM_MANAGER_UDP_CAP];
    size_t srv_poll_len = 0u;
    if (agora_localsock_server_poll(m->srv, 200, srv_poll_buf,
                                    sizeof(srv_poll_buf), &srv_poll_len) != 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    int drain_poll_failed = 0;
    for (;;) {
      if (srv_poll_len > 0u) {
        manager_on_udp_datagram(m, srv_poll_buf, srv_poll_len);
      }
      srv_poll_len = 0u;
      if (agora_localsock_server_poll(m->srv, 0, srv_poll_buf,
                                      sizeof(srv_poll_buf), &srv_poll_len) != 0) {
        if (errno == EINTR) {
          continue;
        }
        drain_poll_failed = 1;
        break;
      }
      if (srv_poll_len == 0u) {
        break;
      }
    }
    if (drain_poll_failed != 0) {
      break;
    }
  }

  return NULL;
}

static void *agora_shm_manager_worker_client(void *arg) {
  AgoraShmManager *m = (AgoraShmManager *)arg;
  uint8_t *udp = m->udp_recv;
  uint64_t last_ka_ns = 0;

  for (;;) {
    if (atomic_load_explicit(&m->stop, memory_order_acquire) != 0) {
      break;
    }

    uint64_t now_ns = 0;
    if (mono_now_ns(&now_ns) != 0) {
      break;
    }
    if (last_ka_ns == 0) {
      last_ka_ns = now_ns;
    } else if (now_ns - last_ka_ns >= 500000000ull) {
      if (agora_localsock_client_send_keepalive(m->cli) != 0) {
        break;
      }
      last_ka_ns = now_ns;
    }

    size_t len = 0u;
    if (agora_localsock_client_poll(m->cli, 100, udp, AGORA_SHM_MANAGER_UDP_CAP,
                                      &len) != 0) {
      if (errno == EAGAIN || errno == EINTR) {
        continue;
      }
      break;
    }

    manager_on_udp_datagram(m, udp, len);
  }

  return NULL;
}

int agora_shm_manager_start(agora_shm_manager_on_frame_fn on_frame, uint16_t port,
                            bool server_mode, size_t localsock_max_clients,
                            uint32_t localsock_keepalive_ms, void *user,
                            size_t max_read_cap, AgoraShmManager **out) {
  if (on_frame == NULL || out == NULL || port == 0u) {
    errno = EINVAL;
    return -1;
  }
  if (server_mode) {
    if (localsock_max_clients == 0u || localsock_keepalive_ms == 0u) {
      errno = EINVAL;
      return -1;
    }
  }

  size_t cap = max_read_cap;
  if (cap == 0u) {
    cap = AGORA_SHM_MANAGER_DEFAULT_READ_CAP;
  }

  AgoraShmManager *m = (AgoraShmManager *)calloc(1, sizeof(*m));
  if (m == NULL) {
    return -1;
  }

  m->read_cap = cap;
  m->on_frame = on_frame;
  m->user = user;
  m->server_mode = server_mode;
  atomic_init(&m->stop, 0);

  if (!server_mode) {
    m->udp_recv = (uint8_t *)malloc(AGORA_SHM_MANAGER_UDP_CAP);
    if (m->udp_recv == NULL) {
      free(m);
      return -1;
    }
  }

  if (pthread_mutex_init(&m->lock, NULL) != 0) {
    int e = errno;
    free(m->udp_recv);
    free(m);
    errno = e;
    return -1;
  }

  if (server_mode) {
    if (agora_localsock_server_create(port, localsock_keepalive_ms,
                                      localsock_max_clients, &m->srv) != 0) {
      int e = errno;
      (void)pthread_mutex_destroy(&m->lock);
      free(m->udp_recv);
      free(m);
      errno = e;
      return -1;
    }
    if (pthread_create(&m->worker, NULL, agora_shm_manager_worker_server, m) !=
        0) {
      int e = errno;
      agora_localsock_server_destroy(m->srv);
      m->srv = NULL;
      (void)pthread_mutex_destroy(&m->lock);
      free(m->udp_recv);
      free(m);
      errno = e;
      return -1;
    }
  } else {
    if (agora_localsock_client_create(port, &m->cli) != 0) {
      int e = errno;
      (void)pthread_mutex_destroy(&m->lock);
      free(m->udp_recv);
      free(m);
      errno = e;
      return -1;
    }
    if (pthread_create(&m->worker, NULL, agora_shm_manager_worker_client, m) !=
        0) {
      int e = errno;
      agora_localsock_client_destroy(m->cli);
      m->cli = NULL;
      (void)pthread_mutex_destroy(&m->lock);
      free(m->udp_recv);
      free(m);
      errno = e;
      return -1;
    }
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
    clear_read_entry(&m->read_entries[i]);
  }
  for (int i = 0; i < AGORA_SHM_MANAGER_MAX_ENTRIES; ++i) {
    AgoraShmManagerWriteEntry *e = &m->write_entries[i];
    if (e->in_use == 0) {
      continue;
    }
    char unlink_name[AGORA_SHM_IPC_SHM_NAME_BYTES];
    memcpy(unlink_name, e->shm_name, sizeof(unlink_name));
    unlink_name[sizeof(unlink_name) - 1u] = '\0';
    const int was_creator = (e->ipc.creator != 0);
    agora_shm_ipc_close(&e->ipc);
    if (was_creator != 0) {
      (void)agora_shm_ipc_unlink(unlink_name);
    }
    memset(e, 0, sizeof(*e));
  }
  (void)pthread_mutex_unlock(&m->lock);

  if (m->srv != NULL) {
    agora_localsock_server_destroy(m->srv);
    m->srv = NULL;
  }
  if (m->cli != NULL) {
    agora_localsock_client_destroy(m->cli);
    m->cli = NULL;
  }

  (void)pthread_mutex_destroy(&m->lock);

  free(m->udp_recv);
  m->udp_recv = NULL;
  free(m);
}

/**
 * Internal: attach an existing SHM for read and insert into read_entries.
 * Same duplicate rules as agora_shm_manager_add (EEXIST if name in write or
 * read table). Does not call writer_session_begin.
 */
static int __attribute__((unused)) agora_shm_manager_addreader_inner(
    AgoraShmManager *m, const char *shm_name, size_t max_payload_size) {
  if (m == NULL || shm_name == NULL || shm_name[0] == '\0' ||
      max_payload_size == 0u) {
    errno = EINVAL;
    return -1;
  }

  (void)pthread_mutex_lock(&m->lock);

  if (find_write_entry_by_name(m, shm_name) >= 0) {
    (void)pthread_mutex_unlock(&m->lock);
    errno = EEXIST;
    return -1;
  }
  if (find_read_entry_by_name(m, shm_name) >= 0) {
    (void)pthread_mutex_unlock(&m->lock);
    errno = EEXIST;
    return -1;
  }

  int slot = find_free_read_slot(m);
  if (slot < 0) {
    (void)pthread_mutex_unlock(&m->lock);
    errno = ENOMEM;
    return -1;
  }

  AgoraShmIpc ipc;
  memset(&ipc, 0, sizeof(ipc));
  if (agora_shm_ipc_open(shm_name, max_payload_size, 0, &ipc) != 0) {
    int e = errno;
    (void)pthread_mutex_unlock(&m->lock);
    errno = e;
    return -1;
  }

  AgoraShmManagerReadEntry *e = &m->read_entries[slot];
  memset(e, 0, sizeof(*e));
  e->in_use = 1;
  e->auto_read = 0;
  (void)strncpy(e->shm_name, shm_name, sizeof(e->shm_name) - 1u);
  e->shm_name[sizeof(e->shm_name) - 1u] = '\0';
  e->ipc = ipc;

  (void)pthread_mutex_unlock(&m->lock);
  return 0;
}

int agora_shm_manager_add(AgoraShmManager *m, const char *shm_name,
                          size_t max_payload_size) {
  if (m == NULL || shm_name == NULL || shm_name[0] == '\0' ||
      max_payload_size == 0u) {
    errno = EINVAL;
    return -1;
  }

  (void)pthread_mutex_lock(&m->lock);

  if (find_write_entry_by_name(m, shm_name) >= 0) {
    (void)pthread_mutex_unlock(&m->lock);
    errno = EEXIST;
    return -1;
  }
  if (find_read_entry_by_name(m, shm_name) >= 0) {
    (void)pthread_mutex_unlock(&m->lock);
    errno = EEXIST;
    return -1;
  }

  int slot = find_free_write_slot(m);
  if (slot < 0) {
    (void)pthread_mutex_unlock(&m->lock);
    errno = ENOMEM;
    return -1;
  }

  AgoraShmIpc ipc;
  memset(&ipc, 0, sizeof(ipc));
  if (agora_shm_ipc_open(shm_name, max_payload_size, 1, &ipc) != 0) {
    int e = errno;
    (void)pthread_mutex_unlock(&m->lock);
    errno = e;
    return -1;
  }
  if (agora_shm_ipc_writer_session_begin(&ipc) != 0) {
    int e = errno;
    agora_shm_ipc_close(&ipc);
    (void)pthread_mutex_unlock(&m->lock);
    errno = e;
    return -1;
  }

  AgoraShmManagerWriteEntry *e = &m->write_entries[slot];
  memset(e, 0, sizeof(*e));
  e->in_use = 1;
  (void)strncpy(e->shm_name, shm_name, sizeof(e->shm_name) - 1u);
  e->shm_name[sizeof(e->shm_name) - 1u] = '\0';
  e->ipc = ipc;

  (void)pthread_mutex_unlock(&m->lock);
  return 0;
}

int agora_shm_manager_remove(AgoraShmManager *m, const char *shm_name) {
  if (m == NULL || shm_name == NULL || shm_name[0] == '\0') {
    errno = EINVAL;
    return -1;
  }

  (void)pthread_mutex_lock(&m->lock);

  int widx = find_write_entry_by_name(m, shm_name);
  if (widx >= 0) {
    clear_write_entry(&m->write_entries[widx]);
    (void)pthread_mutex_unlock(&m->lock);
    return 0;
  }

  int ridx = find_read_entry_by_name(m, shm_name);
  if (ridx >= 0) {
    clear_read_entry(&m->read_entries[ridx]);
    (void)pthread_mutex_unlock(&m->lock);
    return 0;
  }

  (void)pthread_mutex_unlock(&m->lock);
  errno = ENOENT;
  return -1;
}

int agora_shm_manager_write(AgoraShmManager *m, const char *shm_name,
                            const void *data, size_t len,
                            const AgoraShmIpcFrameMeta *meta) {
  if (m == NULL || shm_name == NULL || shm_name[0] == '\0' || meta == NULL) {
    errno = EINVAL;
    return -1;
  }

  (void)pthread_mutex_lock(&m->lock);

  int widx = find_write_entry_by_name(m, shm_name);
  if (widx < 0) {
    (void)pthread_mutex_unlock(&m->lock);
    errno = ENOENT;
    return -1;
  }

  AgoraShmManagerWriteEntry *e = &m->write_entries[widx];
  if (agora_shm_ipc_write(&e->ipc, data, len, meta) != 0) {
    int err = errno;
    (void)pthread_mutex_unlock(&m->lock);
    errno = err;
    return -1;
  }

  AgoraShmIpcFrameMeta meta_copy = *meta;
  (void)pthread_mutex_unlock(&m->lock);

  if (m->server_mode && m->srv != NULL) {
    return agora_localsock_server_send_datagram(
        m->srv, (uint16_t)AGORA_LOCALSOCK_MSG_WRITECMD, &meta_copy,
        sizeof(meta_copy));
  }
  if (!m->server_mode && m->cli != NULL) {
    return agora_localsock_client_send_datagram(
        m->cli, (uint16_t)AGORA_LOCALSOCK_MSG_WRITECMD, &meta_copy,
        sizeof(meta_copy));
  }

  errno = EINVAL;
  return -1;
}
