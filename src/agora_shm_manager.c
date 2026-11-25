/**
 * created by:wei
 * copyright (c) 2026 Agora IO. All rights reserved.
 * date: 2026-04-15
 */

#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "agora_shm_manager.h"

#include "agora_localsock.h"
#include "agora_localsock_internal.h"
#include "agora_shm_ipc.h"
#include "agora_shm_manager_state.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define AGORA_SHM_MANAGER_MAX_ENTRIES 256
#define AGORA_SHM_MANAGER_DEFAULT_READ_CAP (256u * 1024u)
#define AGORA_SHM_MANAGER_UDP_CAP 2048u
#define AGORA_SHM_MANAGER_AUDIO_BUFFER_CAPACITY_IN_MS (200u)
#define AGORA_SHM_MANAGER_AUDIO_FLUSH_NS 10000000ull
#define AGORA_SHM_MANAGER_AUDIO_BATCH_FRAMES 4u
#define AGORA_SHM_MANAGER_AUDIO_QUEUE_FRAMES 64u
#define AGORA_SHM_MANAGER_ERROR_WAIT_MS 20u
#define AGORA_SHM_MANAGER_IDLE_WAIT_MS 10u
#define AGORA_SHM_MANAGER_REBUILD_ERRORS 3u

#define AGORA_SHM_IPC_MAGIC_EXPECT 0xA601C0DEu
#define AGORA_SHM_IPC_VER_EXPECT 3u

typedef struct AgoraShmManagerReadEntry {
  int in_use;
  int auto_read;
  char shm_name[AGORA_SHM_IPC_SHM_NAME_BYTES];
  AgoraShmIpc ipc;
  uint32_t media_type;
  int media_type_valid;
  uint8_t *buffer;
  size_t buffer_capacity;
  size_t data_len;
  unsigned latest_seq;
  int latest_seq_valid;
  AgoraShmIpcHeader latest_header;
} AgoraShmManagerReadEntry;

typedef struct AgoraShmManagerAudioFrame {
  struct AgoraShmManagerAudioFrame *next;
  size_t len;
  AgoraShmIpcFrameMeta meta;
  uint8_t data[];
} AgoraShmManagerAudioFrame;

typedef struct AgoraShmManagerWriteEntry {
  int in_use;
  int removing;
  char shm_name[AGORA_SHM_IPC_SHM_NAME_BYTES];
  AgoraShmIpc ipc;
  AgoraShmManagerAudioFrame *audio_head;
  AgoraShmManagerAudioFrame *audio_tail;
  size_t audio_bytes;
  unsigned audio_count;
  uint64_t audio_flush_ns;
  uint8_t *audio_scratch;
  size_t audio_scratch_capacity;
} AgoraShmManagerWriteEntry;

struct AgoraShmManager {
  pthread_mutex_t lock;
  pthread_mutex_t transport_lock;
  pthread_mutex_t wait_lock;
  pthread_cond_t wake;
  atomic_int stop;
  atomic_int recovering;
  int rebuild_needed; /* Protected by transport_lock. */
  pthread_t worker;
  int worker_started;
  bool server_mode;
  uint16_t port;
  size_t max_clients;
  uint32_t keepalive_ms;
  agora_localsock_server *srv;
  agora_localsock_client *cli;
  AgoraShmManagerReadEntry read_entries[AGORA_SHM_MANAGER_MAX_ENTRIES];
  AgoraShmManagerWriteEntry write_entries[AGORA_SHM_MANAGER_MAX_ENTRIES];
  agora_shm_manager_on_frame_fn on_frame;
  void *user;
  size_t read_cap;
  uint8_t *read_scratch;
  uint8_t *udp_recv;
  AgoraShmManagerState state;
};

static int mono_now_ns(uint64_t *out) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return -errno;
  }
  *out = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  return 0;
}

/* PCM capacity rounded down to whole interleaved samples, without int overflow. */
static int manager_audio_capacity(const AgoraShmIpcFrameMeta *meta,
                                   size_t *sample_bytes, size_t *capacity) {
  if (meta->sample_rate <= 0 || meta->channels <= 0 || meta->bits <= 0 ||
      meta->bits % 8 != 0) {
    return -EINVAL;
  }
  const uint64_t stride = (uint64_t)meta->channels * (uint64_t)(meta->bits / 8);
  const uint64_t samples = (uint64_t)meta->sample_rate *
                          AGORA_SHM_MANAGER_AUDIO_BUFFER_CAPACITY_IN_MS / 1000u;
  if (samples == 0) {
    return -EINVAL;
  }
  if (stride > SIZE_MAX || samples > SIZE_MAX / stride) {
    return -EOVERFLOW;
  }
  *sample_bytes = (size_t)stride;
  *capacity = (size_t)(samples * stride);
  return 0;
}

static int manager_audio_same_format(const AgoraShmIpcFrameMeta *a,
                                      const AgoraShmIpcFrameMeta *b) {
  return a->sample_rate == b->sample_rate && a->channels == b->channels &&
         a->bits == b->bits;
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
  free(e->buffer);
  memset(e, 0, sizeof(*e));
}

static void manager_copy_header(AgoraShmIpcHeader *dst,
                                const AgoraShmIpcHeader *src) {
  dst->magic = src->magic;
  dst->version = src->version;
  dst->payload_size = src->payload_size;
  dst->data_len = src->data_len;
  dst->frame_meta = src->frame_meta;
  atomic_store_explicit(&dst->seq,
                        atomic_load_explicit(&src->seq, memory_order_relaxed),
                        memory_order_relaxed);
}

static void manager_buffer_frame(AgoraShmManager *m,
                                 AgoraShmManagerReadEntry *e,
                                 const void *payload, size_t len,
                                 const AgoraShmIpcHeader *header) {
  const uint32_t media_type = header->frame_meta.media_type;
  if (media_type != (uint32_t)AGORA_SHM_MEDIA_AUDIO &&
      media_type != (uint32_t)AGORA_SHM_MEDIA_VIDEO) {
    return;
  }
  if (e->media_type_valid != 0 && e->media_type != media_type) {
    return;
  }

  const unsigned seq = atomic_load_explicit(&header->seq, memory_order_relaxed);
  if (e->latest_seq_valid != 0 && e->latest_seq == seq) {
    return;
  }

  size_t capacity = m->read_cap;
  if (media_type == (uint32_t)AGORA_SHM_MEDIA_AUDIO) {
    size_t sample_bytes;
    if (manager_audio_capacity(&header->frame_meta, &sample_bytes,
                                &capacity) != 0) {
      return;
    }
  }
  if (e->media_type_valid == 0 ||
      (media_type == (uint32_t)AGORA_SHM_MEDIA_AUDIO &&
       !manager_audio_same_format(&e->latest_header.frame_meta,
                                   &header->frame_meta))) {
    uint8_t *buffer = (uint8_t *)malloc(capacity);
    if (buffer == NULL) {
      return;
    }
    free(e->buffer);
    e->buffer = buffer;
    e->buffer_capacity = capacity;
    e->data_len = 0;
    e->media_type = media_type;
    e->media_type_valid = 1;
  }

  if (media_type == (uint32_t)AGORA_SHM_MEDIA_AUDIO) {
    if (len >= e->buffer_capacity) {
      memcpy(e->buffer, (const uint8_t *)payload + len - e->buffer_capacity,
             e->buffer_capacity);
      e->data_len = e->buffer_capacity;
    } else {
      const size_t retained_capacity = e->buffer_capacity - len;
      if (e->data_len > retained_capacity) {
        const size_t discard = e->data_len - retained_capacity;
        memmove(e->buffer, e->buffer + discard, e->data_len - discard);
        e->data_len -= discard;
      }
      memcpy(e->buffer + e->data_len, payload, len);
      e->data_len += len;
    }
  } else {
    if (len > e->buffer_capacity) {
      return;
    }
    memcpy(e->buffer, payload, len);
    e->data_len = len;
  }

  manager_copy_header(&e->latest_header, header);
  e->latest_seq = seq;
  e->latest_seq_valid = 1;
}

static int find_write_entry_by_name(AgoraShmManager *m, const char *shm_name) {
  for (int i = 0; i < AGORA_SHM_MANAGER_MAX_ENTRIES; ++i) {
    if (m->write_entries[i].in_use != 0 && m->write_entries[i].removing == 0 &&
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

static void manager_audio_pop(AgoraShmManagerWriteEntry *e) {
  AgoraShmManagerAudioFrame *frame = e->audio_head;
  e->audio_head = frame->next;
  e->audio_bytes -= frame->len;
  --e->audio_count;
  if (e->audio_head == NULL) {
    e->audio_tail = NULL;
  }
  free(frame);
}

static void manager_audio_clear(AgoraShmManagerWriteEntry *e) {
  while (e->audio_head != NULL) {
    manager_audio_pop(e);
  }
  e->audio_flush_ns = 0;
}

static void clear_write_entry(AgoraShmManagerWriteEntry *e) {
  manager_audio_clear(e);
  free(e->audio_scratch);
  if (e->in_use != 0) {
    agora_shm_ipc_close(&e->ipc);
  }
  memset(e, 0, sizeof(*e));
}

static int manager_audio_same_meta(const AgoraShmIpcFrameMeta *a,
                                    const AgoraShmIpcFrameMeta *b) {
  return manager_audio_same_format(a, b) &&
         a->media_type == b->media_type && a->stream_type == b->stream_type &&
         a->width == b->width && a->height == b->height &&
         a->orientation == b->orientation &&
         strncmp(a->user_id, b->user_id, sizeof(a->user_id)) == 0 &&
         strncmp(a->shm_name, b->shm_name, sizeof(a->shm_name)) == 0;
}

/* Called with m->lock held. Ownership transfers only on a zero return. */
static int manager_audio_enqueue(AgoraShmManagerWriteEntry *e,
                                  const void *data, size_t len,
                                  const AgoraShmIpcFrameMeta *meta) {
  size_t sample_bytes, capacity;
  int rc = manager_audio_capacity(meta, &sample_bytes, &capacity);
  if (rc != 0) {
    return rc;
  }
  if (len == 0 || len % sample_bytes != 0) {
    return -EINVAL;
  }
  if (len > SIZE_MAX - sizeof(AgoraShmManagerAudioFrame)) {
    return -EOVERFLOW;
  }
  uint64_t now_ns;
  rc = mono_now_ns(&now_ns);
  if (rc != 0) {
    return rc;
  }
  AgoraShmManagerAudioFrame *frame =
      (AgoraShmManagerAudioFrame *)malloc(sizeof(*frame) + len);
  if (frame == NULL) {
    return -ENOMEM;
  }
  frame->next = NULL;
  frame->len = len;
  frame->meta = *meta;
  memcpy(frame->data, data, len);

  if (e->audio_head != NULL &&
      !manager_audio_same_format(&e->audio_head->meta, meta)) {
    manager_audio_clear(e);
  }
  if (e->audio_head == NULL) {
    e->audio_flush_ns = now_ns + AGORA_SHM_MANAGER_AUDIO_FLUSH_NS;
  }
  if (capacity < len) {
    capacity = len; /* Preserve a single valid input even if longer than 200 ms. */
  }
  while (e->audio_head != NULL &&
         (e->audio_bytes > capacity - len ||
          e->audio_count >= AGORA_SHM_MANAGER_AUDIO_QUEUE_FRAMES)) {
    manager_audio_pop(e);
  }
  if (e->audio_tail != NULL) {
    e->audio_tail->next = frame;
  } else {
    e->audio_head = frame;
  }
  e->audio_tail = frame;
  e->audio_bytes += len;
  ++e->audio_count;
  return 0;
}

static int manager_probe_shm_payload_size(const char *shm_name,
                                          uint32_t *out_ps) {
  if (shm_name == NULL || out_ps == NULL) {
    errno = EINVAL;
    return -errno;
  }
  int fd = shm_open(shm_name, O_RDONLY, 0);
  if (fd < 0) {
    return -errno;
  }
  size_t map_len = sizeof(AgoraShmIpcHeader);
  void *p = mmap(NULL, map_len, PROT_READ, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    int e = errno;
    (void)close(fd);
    errno = e;
    return -errno;
  }
  const AgoraShmIpcHeader *h = (const AgoraShmIpcHeader *)p;
  if (h->magic != AGORA_SHM_IPC_MAGIC_EXPECT ||
      h->version != AGORA_SHM_IPC_VER_EXPECT) {
    (void)munmap(p, map_len);
    (void)close(fd);
    errno = EIO;
    return -errno;
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
  memcpy(shm_key, hdr->frame_meta.shm_name, sizeof(shm_key));
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
  agora_shm_manager_on_frame_fn cb = m->on_frame;
  void *payload_ref = cb == NULL ? m->read_scratch : NULL;
  int rr = agora_shm_ipc_read(ipc, &payload_ref, m->read_cap, &out_len, &snap);

  char shm_name_cb[AGORA_SHM_IPC_SHM_NAME_BYTES];
  memcpy(shm_name_cb, e->shm_name, sizeof(shm_name_cb));
  shm_name_cb[sizeof(shm_name_cb) - 1u] = '\0';

  void *user = m->user;

  if (rr == 0 && cb == NULL) {
    manager_buffer_frame(m, e, payload_ref, out_len, &snap);
  }

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
  memcpy(syn.frame_meta.shm_name, shm_key,
         sizeof(syn.frame_meta.shm_name) - 1u);
  syn.frame_meta.shm_name[sizeof(syn.frame_meta.shm_name) - 1u] = '\0';
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

static int manager_send_datagram(AgoraShmManager *m, uint16_t type,
                                  const void *payload, size_t size);

static int manager_send_removecmd(AgoraShmManager *m, const char *shm_name) {
  char payload[AGORA_SHM_IPC_SHM_NAME_BYTES] = {0};
  (void)strncpy(payload, shm_name, sizeof(payload) - 1u);
  return manager_send_datagram(m, AGORA_LOCALSOCK_MSG_REMOVECMD,
                               payload, sizeof(payload));
}

static void manager_dispatch_removecmd(AgoraShmManager *m,
                                       const char *payload) {
  char shm_name[AGORA_SHM_IPC_SHM_NAME_BYTES];
  memcpy(shm_name, payload, sizeof(shm_name));
  if (shm_name[0] == '\0' || memchr(shm_name, '\0', sizeof(shm_name)) == NULL) {
    return;
  }

  (void)pthread_mutex_lock(&m->lock);
  int ridx = find_read_entry_by_name(m, shm_name);
  if (ridx >= 0) {
    clear_read_entry(&m->read_entries[ridx]);
  }
  (void)pthread_mutex_unlock(&m->lock);
}

static void manager_on_udp_datagram(AgoraShmManager *m, const uint8_t *data,
                                    size_t len) {
  if (len < AGORA_LOCALSOCK_HEADER_BYTES) {
    return;
  }
  agora_localsock_header lh;
  memcpy(&lh, data, sizeof(lh));
  if (lh.magic != AGORA_LOCALSOCK_MAGIC ||
      lh.ver != (uint16_t)AGORA_LOCALSOCK_VER) {
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
    alignas(
        AgoraShmIpcHeader) unsigned char hdr_storage[sizeof(AgoraShmIpcHeader)];
    memcpy(hdr_storage, data + AGORA_LOCALSOCK_HEADER_BYTES,
           sizeof(hdr_storage));
    const AgoraShmIpcHeader *hdr = (const AgoraShmIpcHeader *)hdr_storage;
    manager_dispatch_ipc_header(m, hdr);
    return;
  }

  if (lh.msg_type == (uint16_t)AGORA_LOCALSOCK_MSG_WRITECMD) {
    if (lh.payload_len != (uint32_t)sizeof(AgoraShmIpcFrameMeta)) {
      return;
    }
    alignas(AgoraShmIpcFrameMeta) unsigned char
        meta_storage[sizeof(AgoraShmIpcFrameMeta)];
    memcpy(meta_storage, data + AGORA_LOCALSOCK_HEADER_BYTES,
           sizeof(meta_storage));
    const AgoraShmIpcFrameMeta *fm = (const AgoraShmIpcFrameMeta *)meta_storage;
    manager_dispatch_writecmd(m, fm);
    return;
  }

  if (lh.msg_type == (uint16_t)AGORA_LOCALSOCK_MSG_REMOVECMD) {
    if (lh.payload_len != (uint32_t)AGORA_SHM_IPC_SHM_NAME_BYTES) {
      return;
    }
    manager_dispatch_removecmd(
        m, (const char *)(data + AGORA_LOCALSOCK_HEADER_BYTES));
  }
}

static int manager_stopped(AgoraShmManager *m) {
  return atomic_load_explicit(&m->stop, memory_order_acquire) != 0;
}

static void manager_wake(AgoraShmManager *m) {
  (void)pthread_mutex_lock(&m->wait_lock);
  (void)pthread_cond_broadcast(&m->wake);
  (void)pthread_mutex_unlock(&m->wait_lock);
}

/* No entry/transport locks may be held while waiting or logging. */
static void manager_wait(AgoraShmManager *m, unsigned ms) {
  struct timespec until;
  if (clock_gettime(CLOCK_MONOTONIC, &until) != 0) {
    struct timespec delay = {0, (long)ms * 1000000L};
    while (!manager_stopped(m) && nanosleep(&delay, &delay) != 0 &&
           errno == EINTR) {
    }
    return;
  }
  until.tv_nsec += (long)ms * 1000000L;
  if (until.tv_nsec >= 1000000000L) {
    ++until.tv_sec;
    until.tv_nsec -= 1000000000L;
  }
  (void)pthread_mutex_lock(&m->wait_lock);
  while (!manager_stopped(m)) {
#if defined(__APPLE__)
    struct timespec now, remaining;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
      break;
    }
    remaining.tv_sec = until.tv_sec - now.tv_sec;
    remaining.tv_nsec = until.tv_nsec - now.tv_nsec;
    if (remaining.tv_nsec < 0) {
      --remaining.tv_sec;
      remaining.tv_nsec += 1000000000L;
    }
    if (remaining.tv_sec < 0) {
      break;
    }
    int rc = pthread_cond_timedwait_relative_np(&m->wake, &m->wait_lock,
                                                &remaining);
#else
    int rc = pthread_cond_timedwait(&m->wake, &m->wait_lock, &until);
#endif
    if (rc == ETIMEDOUT) {
      break;
    }
    if (rc != 0) {
      break;
    }
    /* Spurious wakeups must not shorten the error delay. */
  }
  (void)pthread_mutex_unlock(&m->wait_lock);
}

static int manager_condition_init(pthread_cond_t *condition) {
#if defined(__APPLE__)
  return pthread_cond_init(condition, NULL);
#else
  pthread_condattr_t attr;
  int rc = pthread_condattr_init(&attr);
  if (rc != 0) {
    return rc;
  }
  rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  if (rc == 0) {
    rc = pthread_cond_init(condition, &attr);
  }
  (void)pthread_condattr_destroy(&attr);
  return rc;
#endif
}

static void manager_log(AgoraShmManager *m, const char *operation,
                        int error, short events, unsigned failures) {
  (void)fprintf(stderr,
                "[agora_shm_manager] mode=%s port=%u op=%s errno=%d "
                "revents=0x%x failures=%u\n",
                m->server_mode ? "server" : "client", (unsigned)m->port,
                operation, error, (unsigned short)events, failures);
}

static void manager_log_state(AgoraShmManager *m, const char *operation,
                              const char *shm_name, int error) {
  (void)fprintf(stderr,
                "[agora_shm_manager] mode=%s port=%u op=%s errno=%d "
                "path=%s shm_name=%s; ownership persistence failed\n",
                m->server_mode ? "server" : "client", (unsigned)m->port,
                operation, error,
                m->state.dir_path[0] != '\0' ? m->state.dir_path :
                                               AGORA_SHM_MANAGER_STATE_ROOT,
                shm_name != NULL ? shm_name : "-");
}

static int manager_invalid_socket(int error) {
  return error == EBADF || error == ENOTSOCK;
}

static int manager_temporary_error(int error) {
  return error == EINTR || error == EAGAIN || error == EWOULDBLOCK;
}

static int manager_send_datagram(AgoraShmManager *m, uint16_t type,
                                  const void *payload, size_t size) {
  (void)pthread_mutex_lock(&m->transport_lock);
  if (atomic_load_explicit(&m->recovering, memory_order_acquire) ||
      (m->server_mode ? m->srv == NULL : m->cli == NULL)) {
    (void)pthread_mutex_unlock(&m->transport_lock);
    errno = EAGAIN;
    return -errno;
  }
  int rc = m->server_mode ?
      agora_localsock_server_send_datagram(m->srv, type, payload, size) :
      agora_localsock_client_send_datagram(m->cli, type, payload, size);
  int error = errno;
  if (rc != 0 && manager_invalid_socket(error)) {
    m->rebuild_needed = 1;
    atomic_store_explicit(&m->recovering, 1, memory_order_release);
  }
  (void)pthread_mutex_unlock(&m->transport_lock);
  if (rc != 0) {
    manager_log(m, "send", error, 0, 0);
    manager_wake(m);
    errno = error;
    return -errno;
  }
  return 0;
}

/* One commit per entry per deadline; never catch up by overwriting repeatedly. */
static void manager_flush_audio(AgoraShmManager *m, uint64_t now_ns) {
  for (int i = 0; i < AGORA_SHM_MANAGER_MAX_ENTRIES; ++i) {
    (void)pthread_mutex_lock(&m->lock);
    AgoraShmManagerWriteEntry *e = &m->write_entries[i];
    if (e->in_use == 0 || e->removing != 0 || e->audio_head == NULL ||
        now_ns < e->audio_flush_ns) {
      (void)pthread_mutex_unlock(&m->lock);
      continue;
    }
    e->audio_flush_ns = now_ns + AGORA_SHM_MANAGER_AUDIO_FLUSH_NS;
    AgoraShmIpcFrameMeta meta = e->audio_head->meta;
    size_t len = 0;
    unsigned count = 0;
    for (AgoraShmManagerAudioFrame *frame = e->audio_head;
         frame != NULL && count < AGORA_SHM_MANAGER_AUDIO_BATCH_FRAMES;
         frame = frame->next) {
      if (!manager_audio_same_meta(&meta, &frame->meta) ||
          frame->len > e->ipc.payload_size - len) {
        break;
      }
      len += frame->len;
      ++count;
    }
    if (len > e->audio_scratch_capacity) {
      uint8_t *scratch = (uint8_t *)realloc(e->audio_scratch, len);
      if (scratch == NULL) {
        (void)pthread_mutex_unlock(&m->lock);
        manager_log(m, "audio_buffer", ENOMEM, 0, 0);
        continue;
      }
      e->audio_scratch = scratch;
      e->audio_scratch_capacity = len;
    }
    size_t offset = 0;
    AgoraShmManagerAudioFrame *frame = e->audio_head;
    for (unsigned n = 0; n < count; ++n) {
      memcpy(e->audio_scratch + offset, frame->data, frame->len);
      offset += frame->len;
      frame = frame->next;
    }
    int rc = agora_shm_ipc_write(&e->ipc, e->audio_scratch, len, &meta);
    int error = errno;
    uint64_t completed_ns;
    if (mono_now_ns(&completed_ns) == 0) {
      e->audio_flush_ns = completed_ns + AGORA_SHM_MANAGER_AUDIO_FLUSH_NS;
    }
    if (rc == 0) {
      for (unsigned n = 0; n < count; ++n) {
        manager_audio_pop(e);
      }
    }
    (void)pthread_mutex_unlock(&m->lock);
    if (rc != 0) {
      manager_log(m, "audio_write", error, 0, 0);
    } else if (manager_send_datagram(m, AGORA_LOCALSOCK_MSG_WRITECMD,
                                     &meta, sizeof(meta)) != 0) {
      /* Already committed: retrying the audio batch would duplicate samples. */
      manager_log(m, "audio_notify", errno, 0, 0);
    }
  }
}

static void manager_worker_error(AgoraShmManager *m, const char *operation,
                                 int error, short events, unsigned *failures,
                                 int socket_error) {
  if (!manager_temporary_error(error)) {
    atomic_store_explicit(&m->recovering, 1, memory_order_release);
    if (*failures < AGORA_SHM_MANAGER_REBUILD_ERRORS) {
      ++*failures;
    }
    if (socket_error &&
        (manager_invalid_socket(error) ||
         *failures >= AGORA_SHM_MANAGER_REBUILD_ERRORS)) {
      (void)pthread_mutex_lock(&m->transport_lock);
      m->rebuild_needed = 1;
      (void)pthread_mutex_unlock(&m->transport_lock);
    }
  }
  manager_log(m, operation, error, events, *failures);
  manager_wait(m, AGORA_SHM_MANAGER_ERROR_WAIT_MS);
}

static int manager_rebuild_transport(AgoraShmManager *m, unsigned failures) {
  (void)pthread_mutex_lock(&m->transport_lock);
  if (!m->rebuild_needed) {
    (void)pthread_mutex_unlock(&m->transport_lock);
    return 0;
  }
  atomic_store_explicit(&m->recovering, 1, memory_order_release);
  agora_localsock_server *old_srv = m->srv;
  agora_localsock_client *old_cli = m->cli;
  m->srv = NULL;
  m->cli = NULL;
  (void)pthread_mutex_unlock(&m->transport_lock);

  /* Only the worker replaces sockets; detached objects have no users. */
  if (old_srv != NULL || old_cli != NULL) {
    manager_log(m, "rebuild_begin", 0, 0, failures);
  }
  agora_localsock_server_destroy(old_srv);
  agora_localsock_client_destroy(old_cli);
  if (manager_stopped(m)) {
    return -ECANCELED;
  }
  agora_localsock_server *new_srv = NULL;
  agora_localsock_client *new_cli = NULL;
  int rc;
  if (m->server_mode) {
    rc = agora_localsock_server_create(m->port, m->keepalive_ms,
                                       m->max_clients, &new_srv);
  } else {
    rc = agora_localsock_client_create(m->port, &new_cli);
    if (rc == 0) {
      rc = agora_localsock_client_send_keepalive(new_cli);
    }
  }
  int error = errno;
  if (rc != 0 || manager_stopped(m)) {
    agora_localsock_server_destroy(new_srv);
    agora_localsock_client_destroy(new_cli);
    if (rc != 0) {
      manager_log(m, "rebuild", error, 0, failures);
      manager_wait(m, AGORA_SHM_MANAGER_ERROR_WAIT_MS);
    }
    return rc != 0 ? -error : -ECANCELED;
  }

  (void)pthread_mutex_lock(&m->transport_lock);
  m->srv = new_srv;
  m->cli = new_cli;
  m->rebuild_needed = 0;
  atomic_store_explicit(&m->recovering, 0, memory_order_release);
  (void)pthread_mutex_unlock(&m->transport_lock);
  manager_log(m, "recovered", 0, 0, 0);
  return 1;
}

static void *manager_worker(AgoraShmManager *m) {
  unsigned poll_failures = 0;
  unsigned keepalive_failures = 0;
  unsigned clock_failures = 0;
  uint64_t last_ka_ns = 0;

  while (!manager_stopped(m)) {
    uint64_t now_ns = 0;
    if (mono_now_ns(&now_ns) != 0) {
      manager_worker_error(m, "clock", errno, 0, &clock_failures, 0);
      continue;
    }
    clock_failures = 0;
    manager_flush_audio(m, now_ns);
    int rebuilt = manager_rebuild_transport(
        m, poll_failures > keepalive_failures ? poll_failures : keepalive_failures);
    if (rebuilt < 0) {
      continue;
    }
    if (rebuilt > 0) {
      poll_failures = keepalive_failures = 0;
      last_ka_ns = 0;
    }
    if (!m->server_mode) {
      if (last_ka_ns == 0 || now_ns - last_ka_ns >= 500000000ull) {
        (void)pthread_mutex_lock(&m->transport_lock);
        int rc = agora_localsock_client_send_keepalive(m->cli);
        int error = errno;
        (void)pthread_mutex_unlock(&m->transport_lock);
        if (rc != 0) {
          manager_worker_error(m, "keepalive", error, 0,
                                &keepalive_failures, 1);
          continue;
        }
        keepalive_failures = 0;
        last_ka_ns = now_ns;
      }
    }

    size_t len = 0;
    short events;
    (void)pthread_mutex_lock(&m->transport_lock);
    int rc;
    if (m->server_mode) {
      rc = agora_localsock_server_poll(m->srv, 0, m->udp_recv,
                                       AGORA_SHM_MANAGER_UDP_CAP, &len);
      events = agora_localsock_server_poll_events(m->srv);
    } else {
      rc = agora_localsock_client_poll(m->cli, 0, m->udp_recv,
                                       AGORA_SHM_MANAGER_UDP_CAP, &len);
      events = agora_localsock_client_poll_events(m->cli);
    }
    int error = errno;
    int healthy = rc == 0 || error == EAGAIN || error == EWOULDBLOCK;
    int recovered = 0;
    if (healthy && !m->rebuild_needed) {
      poll_failures = 0;
      recovered = atomic_exchange_explicit(&m->recovering, 0,
                                            memory_order_acq_rel);
    }
    (void)pthread_mutex_unlock(&m->transport_lock);
    if (recovered) {
      manager_log(m, "recovered", 0, 0, 0);
    }
    if (!healthy) {
      manager_worker_error(m, m->server_mode ? "server_poll" : "client_poll",
                            error, events, &poll_failures, 1);
      continue;
    }
    if (len != 0) {
      manager_on_udp_datagram(m, m->udp_recv, len);
    } else {
      manager_wait(m, AGORA_SHM_MANAGER_IDLE_WAIT_MS);
    }
  }
  return NULL;
}

static void *agora_shm_manager_worker_server(void *arg) {
  return manager_worker((AgoraShmManager *)arg);
}

static void *agora_shm_manager_worker_client(void *arg) {
  return manager_worker((AgoraShmManager *)arg);
}

int agora_shm_manager_start(agora_shm_manager_on_frame_fn on_frame,
                            uint16_t port, bool server_mode,
                            size_t localsock_max_clients,
                            uint32_t localsock_keepalive_ms, void *user,
                            size_t max_read_cap, AgoraShmManager **out) {
  if (out == NULL || port == 0u ||
      (server_mode && (localsock_max_clients == 0u ||
                       localsock_keepalive_ms == 0u))) {
    errno = EINVAL;
    return -errno;
  }
  AgoraShmManager *m = (AgoraShmManager *)calloc(1, sizeof(*m));
  if (m == NULL) {
    return -errno;
  }
  m->read_cap = max_read_cap != 0 ? max_read_cap :
                                  AGORA_SHM_MANAGER_DEFAULT_READ_CAP;
  m->on_frame = on_frame;
  m->user = user;
  m->server_mode = server_mode;
  m->port = port;
  m->max_clients = localsock_max_clients;
  m->keepalive_ms = localsock_keepalive_ms;
  atomic_init(&m->stop, 0);
  atomic_init(&m->recovering, 0);

  m->state.lock_fd = -1;
  if (agora_shm_manager_state_open(&m->state) != 0) {
    manager_log_state(m, "state_open", NULL, errno);
  }
  int sync_initialized = 0;
  int error = pthread_mutex_init(&m->lock, NULL);
  if (error != 0) {
    goto fail;
  }
  sync_initialized = 1;
  error = pthread_mutex_init(&m->transport_lock, NULL);
  if (error != 0) {
    goto fail;
  }
  sync_initialized = 2;
  error = pthread_mutex_init(&m->wait_lock, NULL);
  if (error != 0) {
    goto fail;
  }
  sync_initialized = 3;
  error = manager_condition_init(&m->wake);
  if (error != 0) {
    goto fail;
  }
  sync_initialized = 4;

  if (on_frame == NULL) {
    m->read_scratch = (uint8_t *)malloc(m->read_cap);
    if (m->read_scratch == NULL) {
      error = errno;
      goto fail;
    }
  }
  m->udp_recv = (uint8_t *)malloc(AGORA_SHM_MANAGER_UDP_CAP);
  if (m->udp_recv == NULL) {
    error = errno;
    goto fail;
  }
  int rc = server_mode ?
      agora_localsock_server_create(port, localsock_keepalive_ms,
                                     localsock_max_clients, &m->srv) :
      agora_localsock_client_create(port, &m->cli);
  if (rc != 0) {
    error = errno;
    goto fail;
  }
  error = pthread_create(&m->worker, NULL,
                          server_mode ? agora_shm_manager_worker_server :
                                        agora_shm_manager_worker_client, m);
  if (error != 0) {
    goto fail;
  }
  m->worker_started = 1;
  *out = m;
  return 0;

fail:
  agora_localsock_server_destroy(m->srv);
  agora_localsock_client_destroy(m->cli);
  free(m->udp_recv);
  free(m->read_scratch);
  if (sync_initialized >= 4) {
    (void)pthread_cond_destroy(&m->wake);
  }
  if (sync_initialized >= 3) {
    (void)pthread_mutex_destroy(&m->wait_lock);
  }
  if (sync_initialized >= 2) {
    (void)pthread_mutex_destroy(&m->transport_lock);
  }
  if (sync_initialized >= 1) {
    (void)pthread_mutex_destroy(&m->lock);
  }
  agora_shm_manager_state_close(&m->state);
  free(m);
  errno = error;
  return -errno;
}

void agora_shm_manager_close(AgoraShmManager *m) {
  if (m == NULL) {
    return;
  }

  atomic_store_explicit(&m->stop, 1, memory_order_release);
  manager_wake(m);

  if (m->worker_started != 0) {
    (void)pthread_join(m->worker, NULL);
    m->worker_started = 0;
  }

  char remove_names[AGORA_SHM_MANAGER_MAX_ENTRIES]
                   [AGORA_SHM_IPC_SHM_NAME_BYTES];
  size_t remove_count = 0u;

  (void)pthread_mutex_lock(&m->lock);
  for (int i = 0; i < AGORA_SHM_MANAGER_MAX_ENTRIES; ++i) {
    clear_read_entry(&m->read_entries[i]);
  }
  for (int i = 0; i < AGORA_SHM_MANAGER_MAX_ENTRIES; ++i) {
    AgoraShmManagerWriteEntry *e = &m->write_entries[i];
    if (e->in_use == 0) {
      continue;
    }
    e->removing = 1;
    memcpy(remove_names[remove_count], e->shm_name,
           sizeof(remove_names[remove_count]));
    ++remove_count;
  }
  (void)pthread_mutex_unlock(&m->lock);

  for (size_t i = 0u; i < remove_count; ++i) {
    (void)manager_send_removecmd(m, remove_names[i]);
  }

  int preserve_state = 0;
  (void)pthread_mutex_lock(&m->lock);
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
      int unlink_rc = agora_shm_ipc_unlink(unlink_name);
      int unlink_errno = errno;
      if (unlink_rc == 0 || unlink_errno == ENOENT) {
        if (m->state.lock_fd >= 0 &&
            agora_shm_manager_state_remove(&m->state, unlink_name) != 0) {
          manager_log_state(m, "state_remove", unlink_name, errno);
          preserve_state = 1;
        }
      } else {
        preserve_state = 1;
      }
    }
    clear_write_entry(e);
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

  if (preserve_state == 0 && m->state.name_count == 0u) {
    agora_shm_manager_state_close(&m->state);
  } else {
    agora_shm_manager_state_abandon(&m->state);
  }

  (void)pthread_mutex_destroy(&m->lock);

  (void)pthread_mutex_destroy(&m->transport_lock);
  (void)pthread_cond_destroy(&m->wake);
  (void)pthread_mutex_destroy(&m->wait_lock);

  free(m->udp_recv);
  m->udp_recv = NULL;
  free(m->read_scratch);
  m->read_scratch = NULL;
  free(m);
}

/**
 * Internal: attach an existing SHM for read and insert into read_entries.
 * Same duplicate rules as agora_shm_manager_add (EEXIST if name in write or
 * read table). Does not call writer_session_begin.
 */
static int __attribute__((unused))
agora_shm_manager_addreader_inner(AgoraShmManager *m, const char *shm_name,
                                  size_t max_payload_size) {
  if (m == NULL || shm_name == NULL || shm_name[0] == '\0' ||
      max_payload_size == 0u) {
    errno = EINVAL;
    return -errno;
  }

  (void)pthread_mutex_lock(&m->lock);

  if (find_write_entry_by_name(m, shm_name) >= 0) {
    (void)pthread_mutex_unlock(&m->lock);
    errno = EEXIST;
    return -errno;
  }
  if (find_read_entry_by_name(m, shm_name) >= 0) {
    (void)pthread_mutex_unlock(&m->lock);
    errno = EEXIST;
    return -errno;
  }

  int slot = find_free_read_slot(m);
  if (slot < 0) {
    (void)pthread_mutex_unlock(&m->lock);
    errno = ENOMEM;
    return -errno;
  }

  AgoraShmIpc ipc;
  memset(&ipc, 0, sizeof(ipc));
  if (agora_shm_ipc_open(shm_name, max_payload_size, 0, &ipc) != 0) {
    int e = errno;
    (void)pthread_mutex_unlock(&m->lock);
    errno = e;
    return -errno;
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
    return -errno;
  }

  (void)pthread_mutex_lock(&m->lock);

  if (find_write_entry_by_name(m, shm_name) >= 0) {
    (void)pthread_mutex_unlock(&m->lock);
    errno = EEXIST;
    return -errno;
  }
  if (find_read_entry_by_name(m, shm_name) >= 0) {
    (void)pthread_mutex_unlock(&m->lock);
    errno = EEXIST;
    return -errno;
  }

  int slot = find_free_write_slot(m);
  if (slot < 0) {
    (void)pthread_mutex_unlock(&m->lock);
    errno = ENOMEM;
    return -errno;
  }

  AgoraShmIpc ipc;
  memset(&ipc, 0, sizeof(ipc));
  if (agora_shm_ipc_open(shm_name, max_payload_size, 1, &ipc) != 0) {
    int e = errno;
    (void)pthread_mutex_unlock(&m->lock);
    errno = e;
    return -errno;
  }
  if (agora_shm_ipc_writer_session_begin(&ipc) != 0) {
    int e = errno;
    agora_shm_ipc_close(&ipc);
    (void)agora_shm_ipc_unlink(shm_name);
    (void)pthread_mutex_unlock(&m->lock);
    errno = e;
    return -errno;
  }
  if (m->state.lock_fd >= 0 &&
      agora_shm_manager_state_add(&m->state, shm_name) != 0) {
    manager_log_state(m, "state_add", shm_name, errno);
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
    return -errno;
  }

  (void)pthread_mutex_lock(&m->lock);

  int widx = find_write_entry_by_name(m, shm_name);
  if (widx >= 0) {
    AgoraShmManagerWriteEntry *entry = &m->write_entries[widx];
    entry->removing = 1;
    char remove_name[AGORA_SHM_IPC_SHM_NAME_BYTES];
    memcpy(remove_name, entry->shm_name, sizeof(remove_name));
    (void)pthread_mutex_unlock(&m->lock);

    int first_error = 0;
    if (manager_send_removecmd(m, remove_name) != 0) {
      first_error = errno;
    }

    (void)pthread_mutex_lock(&m->lock);
    clear_write_entry(entry);
    (void)pthread_mutex_unlock(&m->lock);

    int unlink_rc = agora_shm_ipc_unlink(remove_name);
    int unlink_errno = errno;
    if (unlink_rc == 0 || unlink_errno == ENOENT) {
      (void)pthread_mutex_lock(&m->lock);
      if (m->state.lock_fd >= 0 &&
          agora_shm_manager_state_remove(&m->state, remove_name) != 0) {
        manager_log_state(m, "state_remove", remove_name, errno);
      }
      (void)pthread_mutex_unlock(&m->lock);
    } else if (first_error == 0) {
      first_error = unlink_errno;
    }
    if (first_error != 0) {
      errno = first_error;
      return -errno;
    }
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
  return -errno;
}

int agora_shm_manager_write(AgoraShmManager *m, const char *shm_name,
                            const void *data, size_t len,
                            const AgoraShmIpcFrameMeta *meta) {
  if (m == NULL || shm_name == NULL || shm_name[0] == '\0' || meta == NULL) {
    errno = EINVAL;
    return -errno;
  }

  (void)pthread_mutex_lock(&m->lock);

  int widx = find_write_entry_by_name(m, shm_name);
  if (widx < 0) {
    (void)pthread_mutex_unlock(&m->lock);
    errno = ENOENT;
    return -errno;
  }

  AgoraShmManagerWriteEntry *e = &m->write_entries[widx];
  if (data == NULL || len > e->ipc.payload_size) {
    int error = data == NULL ? EINVAL : EMSGSIZE;
    (void)pthread_mutex_unlock(&m->lock);
    errno = error;
    return -errno;
  }
  if (meta->media_type == (uint32_t)AGORA_SHM_MEDIA_AUDIO) {
    int rc = manager_audio_enqueue(e, data, len, meta);
    (void)pthread_mutex_unlock(&m->lock);
    if (rc != 0) {
      errno = -rc;
    }
    return rc;
  }
  manager_audio_clear(e);
  if (agora_shm_ipc_write(&e->ipc, data, len, meta) != 0) {
    int err = errno;
    (void)pthread_mutex_unlock(&m->lock);
    errno = err;
    return -errno;
  }

  AgoraShmIpcFrameMeta meta_copy = *meta;
  (void)pthread_mutex_unlock(&m->lock);

  return manager_send_datagram(m, AGORA_LOCALSOCK_MSG_WRITECMD,
                               &meta_copy, sizeof(meta_copy));
}

ssize_t agora_shm_manager_poll(AgoraShmManager *m, const char *shm_name,
                               AgoraShmIpcHeader *header, char *out,
                               size_t max_payload_size) {
  if (m == NULL || shm_name == NULL || shm_name[0] == '\0' || header == NULL ||
      out == NULL || max_payload_size == 0u) {
    errno = EINVAL;
    return -errno;
  }

  (void)pthread_mutex_lock(&m->lock);

  if (m->on_frame != NULL) {
    (void)pthread_mutex_unlock(&m->lock);
    errno = ENOTSUP;
    return -errno;
  }

  int ridx = find_read_entry_by_name(m, shm_name);
  if (ridx < 0) {
    int error = atomic_load_explicit(&m->recovering, memory_order_acquire) ?
                    EAGAIN : ENOENT;
    (void)pthread_mutex_unlock(&m->lock);
    errno = error;
    return -errno;
  }

  AgoraShmManagerReadEntry *e = &m->read_entries[ridx];
  if (e->data_len == 0u) {
    int recovering = atomic_load_explicit(&m->recovering, memory_order_acquire);
    (void)pthread_mutex_unlock(&m->lock);
    if (recovering) {
      errno = EAGAIN;
      return -errno;
    }
    return 0;
  }

  size_t copy_len = e->data_len;
  if (e->media_type == (uint32_t)AGORA_SHM_MEDIA_AUDIO) {
    if (copy_len > max_payload_size) {
      copy_len = max_payload_size;
    }
  } else if (copy_len > max_payload_size) {
    (void)pthread_mutex_unlock(&m->lock);
    errno = ENOBUFS;
    return -errno;
  }

  if (copy_len > (size_t)SSIZE_MAX) {
    (void)pthread_mutex_unlock(&m->lock);
    errno = EOVERFLOW;
    return -errno;
  }

  memcpy(out, e->buffer, copy_len);
  manager_copy_header(header, &e->latest_header);
  header->data_len = (uint32_t)copy_len;

  if (e->media_type == (uint32_t)AGORA_SHM_MEDIA_AUDIO) {
    const size_t remaining = e->data_len - copy_len;
    if (remaining > 0u) {
      memmove(e->buffer, e->buffer + copy_len, remaining);
    }
    e->data_len = remaining;
  } else {
    e->data_len = 0u;
  }

  (void)pthread_mutex_unlock(&m->lock);
  return (ssize_t)copy_len;
}
