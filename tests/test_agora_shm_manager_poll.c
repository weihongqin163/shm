#define _POSIX_C_SOURCE 200809L

#include "agora_localsock.h"
#include "agora_shm_manager.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define AUDIO_BUFFER_CAPACITY (48u * 2u * 200u)
#define TEST_READ_CAP 32768u

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL %s:%d: %s (errno=%d)\n", __FILE__, __LINE__,       \
              #cond, errno);                                                   \
      return -1;                                                               \
    }                                                                          \
  } while (0)

static ssize_t (*poll_api)(AgoraShmManager *, const char *, AgoraShmIpcHeader *,
                           char *, size_t) = agora_shm_manager_poll;

typedef struct TestFixture {
  AgoraShmManager *receiver;
  AgoraShmManager *writer;
  uint16_t port;
  char shm_name[AGORA_SHM_IPC_SHM_NAME_BYTES];
} TestFixture;

typedef struct CallbackCapture {
  atomic_int called;
  size_t len;
  unsigned seq;
  uint8_t payload[256];
} CallbackCapture;

typedef struct StressWriter {
  TestFixture *fixture;
  uint8_t *data;
  size_t len;
  unsigned iterations;
  atomic_int failed;
  atomic_int done;
} StressWriter;

static unsigned g_name_counter = 0u;

static void sleep_ms(long ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

static uint16_t reserve_udp_port(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return 0u;
  }
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
    (void)close(fd);
    return 0u;
  }
  addr.sin_port = 0;
  if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
    (void)close(fd);
    return 0u;
  }
  socklen_t len = (socklen_t)sizeof(addr);
  if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
    (void)close(fd);
    return 0u;
  }
  uint16_t port = ntohs(addr.sin_port);
  (void)close(fd);
  return port;
}

static void noop_frame(const char *shm_name, const void *payload, size_t len,
                       const AgoraShmIpcHeader *header, void *user) {
  (void)shm_name;
  (void)payload;
  (void)len;
  (void)header;
  (void)user;
}

static void capture_frame(const char *shm_name, const void *payload, size_t len,
                          const AgoraShmIpcHeader *header, void *user) {
  (void)shm_name;
  CallbackCapture *capture = (CallbackCapture *)user;
  if (len > sizeof(capture->payload)) {
    return;
  }
  memcpy(capture->payload, payload, len);
  capture->len = len;
  capture->seq = atomic_load_explicit(&header->seq, memory_order_relaxed);
  atomic_store_explicit(&capture->called, 1, memory_order_release);
}

static void fixture_close(TestFixture *f) {
  if (f->writer != NULL) {
    agora_shm_manager_close(f->writer);
    f->writer = NULL;
  }
  if (f->receiver != NULL) {
    agora_shm_manager_close(f->receiver);
    f->receiver = NULL;
  }
  if (f->shm_name[0] != '\0') {
    (void)agora_shm_ipc_unlink(f->shm_name);
  }
}

static int fixture_open(TestFixture *f, size_t payload_size) {
  memset(f, 0, sizeof(*f));
  f->port = reserve_udp_port();
  CHECK(f->port != 0u);
  (void)snprintf(f->shm_name, sizeof(f->shm_name), "/apoll_%ld_%u",
                 (long)getpid(), ++g_name_counter);
  (void)agora_shm_ipc_unlink(f->shm_name);

  CHECK(agora_shm_manager_start(NULL, f->port, true, 4u, 1000u, NULL,
                                TEST_READ_CAP, &f->receiver) == 0);
  if (agora_shm_manager_start(noop_frame, f->port, false, 0u, 0u, NULL,
                              TEST_READ_CAP, &f->writer) != 0) {
    fixture_close(f);
    CHECK(0);
  }
  if (agora_shm_manager_add(f->writer, f->shm_name, payload_size) != 0) {
    fixture_close(f);
    CHECK(0);
  }
  return 0;
}

static AgoraShmIpcFrameMeta make_meta(const TestFixture *f,
                                      uint32_t media_type) {
  AgoraShmIpcFrameMeta meta;
  memset(&meta, 0, sizeof(meta));
  (void)strncpy(meta.user_id, "poll-test", sizeof(meta.user_id) - 1u);
  (void)strncpy(meta.shm_name, f->shm_name, sizeof(meta.shm_name) - 1u);
  meta.media_type = media_type;
  meta.stream_type = (uint32_t)AGORA_SHM_STREAM_MAIN;
  if (media_type == (uint32_t)AGORA_SHM_MEDIA_AUDIO) {
    meta.sample_rate = 48000;
    meta.channels = 1;
    meta.bits = 16;
  } else if (media_type == (uint32_t)AGORA_SHM_MEDIA_VIDEO) {
    meta.width = 16;
    meta.height = 16;
  }
  return meta;
}

static int write_frame(TestFixture *f, const void *data, size_t len,
                       uint32_t media_type) {
  AgoraShmIpcFrameMeta meta = make_meta(f, media_type);
  CHECK(agora_shm_manager_write(f->writer, f->shm_name, data, len, &meta) == 0);
  sleep_ms(100);
  return 0;
}

static ssize_t wait_for_poll(TestFixture *f, AgoraShmIpcHeader *header,
                             char *out, size_t cap) {
  for (unsigned i = 0u; i < 100u; ++i) {
    ssize_t n = poll_api(f->receiver, f->shm_name, header, out, cap);
    if (n >= 0) {
      return n;
    }
    if (errno != ENOENT) {
      return -1;
    }
    sleep_ms(10);
  }
  errno = ETIMEDOUT;
  return -1;
}

static int test_audio_append_and_header(void) {
  TestFixture f;
  CHECK(fixture_open(&f, 1024u) == 0);
  uint8_t first[128];
  uint8_t second[96];
  memset(first, 0x11, sizeof(first));
  memset(second, 0x22, sizeof(second));
  CHECK(write_frame(&f, first, sizeof(first), AGORA_SHM_MEDIA_AUDIO) == 0);
  CHECK(write_frame(&f, second, sizeof(second), AGORA_SHM_MEDIA_AUDIO) == 0);

  char out[sizeof(first) + sizeof(second)];
  AgoraShmIpcHeader header;
  ssize_t n = wait_for_poll(&f, &header, out, sizeof(out));
  CHECK(n == (ssize_t)sizeof(out));
  CHECK(memcmp(out, first, sizeof(first)) == 0);
  CHECK(memcmp(out + sizeof(first), second, sizeof(second)) == 0);
  CHECK(header.media_type == (uint32_t)AGORA_SHM_MEDIA_AUDIO);
  CHECK(header.data_len == (uint32_t)sizeof(out));
  CHECK(atomic_load_explicit(&header.seq, memory_order_relaxed) == 4u);
  fixture_close(&f);
  return 0;
}

static int test_audio_keeps_latest_200ms(void) {
  TestFixture f;
  CHECK(fixture_open(&f, 12000u) == 0);
  uint8_t *first = (uint8_t *)malloc(12000u);
  uint8_t *second = (uint8_t *)malloc(12000u);
  char *out = (char *)malloc(AUDIO_BUFFER_CAPACITY);
  CHECK(first != NULL && second != NULL && out != NULL);
  memset(first, 0x31, 12000u);
  memset(second, 0x42, 12000u);
  CHECK(write_frame(&f, first, 12000u, AGORA_SHM_MEDIA_AUDIO) == 0);
  CHECK(write_frame(&f, second, 12000u, AGORA_SHM_MEDIA_AUDIO) == 0);

  AgoraShmIpcHeader header;
  ssize_t n = wait_for_poll(&f, &header, out, AUDIO_BUFFER_CAPACITY);
  CHECK(n == (ssize_t)AUDIO_BUFFER_CAPACITY);
  for (size_t i = 0u; i < 7200u; ++i) {
    CHECK((unsigned char)out[i] == 0x31u);
  }
  for (size_t i = 7200u; i < AUDIO_BUFFER_CAPACITY; ++i) {
    CHECK((unsigned char)out[i] == 0x42u);
  }
  CHECK(header.data_len == AUDIO_BUFFER_CAPACITY);
  free(out);
  free(second);
  free(first);
  fixture_close(&f);
  return 0;
}

static int test_single_audio_block_keeps_latest_200ms(void) {
  TestFixture f;
  CHECK(fixture_open(&f, 24000u) == 0);
  uint8_t *data = (uint8_t *)malloc(24000u);
  char *out = (char *)malloc(AUDIO_BUFFER_CAPACITY);
  CHECK(data != NULL && out != NULL);
  for (size_t i = 0u; i < 24000u; ++i) {
    data[i] = (uint8_t)(i & 0xffu);
  }
  CHECK(write_frame(&f, data, 24000u, AGORA_SHM_MEDIA_AUDIO) == 0);

  AgoraShmIpcHeader header;
  ssize_t n = wait_for_poll(&f, &header, out, AUDIO_BUFFER_CAPACITY);
  CHECK(n == (ssize_t)AUDIO_BUFFER_CAPACITY);
  CHECK(memcmp(out, data + 24000u - AUDIO_BUFFER_CAPACITY,
               AUDIO_BUFFER_CAPACITY) == 0);
  CHECK(header.data_len == AUDIO_BUFFER_CAPACITY);
  free(out);
  free(data);
  fixture_close(&f);
  return 0;
}

static int test_video_replaces_previous_frame(void) {
  TestFixture f;
  CHECK(fixture_open(&f, 1024u) == 0);
  uint8_t first[100];
  uint8_t second[80];
  memset(first, 0x51, sizeof(first));
  memset(second, 0x62, sizeof(second));
  CHECK(write_frame(&f, first, sizeof(first), AGORA_SHM_MEDIA_VIDEO) == 0);
  CHECK(write_frame(&f, second, sizeof(second), AGORA_SHM_MEDIA_VIDEO) == 0);

  char out[128];
  AgoraShmIpcHeader header;
  ssize_t n = wait_for_poll(&f, &header, out, sizeof(out));
  CHECK(n == (ssize_t)sizeof(second));
  CHECK(memcmp(out, second, sizeof(second)) == 0);
  CHECK(header.data_len == (uint32_t)sizeof(second));
  CHECK(atomic_load_explicit(&header.seq, memory_order_relaxed) == 4u);
  fixture_close(&f);
  return 0;
}

static int test_duplicate_seq_is_ignored(void) {
  TestFixture f;
  CHECK(fixture_open(&f, 1024u) == 0);
  uint8_t data[64];
  memset(data, 0x73, sizeof(data));
  CHECK(write_frame(&f, data, sizeof(data), AGORA_SHM_MEDIA_AUDIO) == 0);

  char out[128];
  AgoraShmIpcHeader header;
  ssize_t n = wait_for_poll(&f, &header, out, sizeof(out));
  CHECK(n == (ssize_t)sizeof(data));

  agora_localsock_client *signal_client = NULL;
  CHECK(agora_localsock_client_create(f.port, &signal_client) == 0);
  AgoraShmIpcFrameMeta meta = make_meta(&f, AGORA_SHM_MEDIA_AUDIO);
  CHECK(agora_localsock_client_send_datagram(
            signal_client, (uint16_t)AGORA_LOCALSOCK_MSG_WRITECMD, &meta,
            sizeof(meta)) == 0);
  sleep_ms(100);

  memset(out, 0x7f, sizeof(out));
  memset(&header, 0x7f, sizeof(header));
  CHECK(poll_api(f.receiver, f.shm_name, &header, out, sizeof(out)) == 0);
  agora_localsock_client_destroy(signal_client);
  fixture_close(&f);
  return 0;
}

static int test_media_type_is_locked(void) {
  TestFixture f;
  CHECK(fixture_open(&f, 1024u) == 0);
  uint8_t audio[32];
  uint8_t video[16];
  memset(audio, 0x14, sizeof(audio));
  memset(video, 0x25, sizeof(video));
  CHECK(write_frame(&f, audio, sizeof(audio), AGORA_SHM_MEDIA_AUDIO) == 0);
  CHECK(write_frame(&f, video, sizeof(video), AGORA_SHM_MEDIA_VIDEO) == 0);

  char out[64];
  AgoraShmIpcHeader header;
  ssize_t n = wait_for_poll(&f, &header, out, sizeof(out));
  CHECK(n == (ssize_t)sizeof(audio));
  CHECK(memcmp(out, audio, sizeof(audio)) == 0);
  CHECK(header.media_type == (uint32_t)AGORA_SHM_MEDIA_AUDIO);
  CHECK(atomic_load_explicit(&header.seq, memory_order_relaxed) == 2u);
  fixture_close(&f);
  return 0;
}

static int test_poll_errors_and_empty_entry(void) {
  uint16_t port = reserve_udp_port();
  CHECK(port != 0u);
  AgoraShmManager *callback_manager = NULL;
  CHECK(agora_shm_manager_start(noop_frame, port, true, 1u, 1000u, NULL,
                                TEST_READ_CAP, &callback_manager) == 0);
  AgoraShmIpcHeader header;
  char out[8];
  errno = 0;
  CHECK(poll_api(NULL, "/missing", &header, out, sizeof(out)) == -1);
  CHECK(errno == EINVAL);
  errno = 0;
  CHECK(poll_api(callback_manager, NULL, &header, out, sizeof(out)) == -1);
  CHECK(errno == EINVAL);
  errno = 0;
  CHECK(poll_api(callback_manager, "", &header, out, sizeof(out)) == -1);
  CHECK(errno == EINVAL);
  errno = 0;
  CHECK(poll_api(callback_manager, "/missing", NULL, out, sizeof(out)) == -1);
  CHECK(errno == EINVAL);
  errno = 0;
  CHECK(poll_api(callback_manager, "/missing", &header, NULL, sizeof(out)) ==
        -1);
  CHECK(errno == EINVAL);
  errno = 0;
  CHECK(poll_api(callback_manager, "/missing", &header, out, 0u) == -1);
  CHECK(errno == EINVAL);
  errno = 0;
  CHECK(poll_api(callback_manager, "/missing", &header, out, sizeof(out)) ==
        -1);
  CHECK(errno == ENOTSUP);
  agora_shm_manager_close(callback_manager);

  TestFixture f;
  CHECK(fixture_open(&f, 1024u) == 0);
  errno = 0;
  CHECK(poll_api(f.receiver, "/unknown", &header, out, sizeof(out)) == -1);
  CHECK(errno == ENOENT);

  uint8_t invalid[4] = {1u, 2u, 3u, 4u};
  CHECK(write_frame(&f, invalid, sizeof(invalid), 99u) == 0);
  memset(&header, 0x5a, sizeof(header));
  memset(out, 0x6b, sizeof(out));
  AgoraShmIpcHeader expected_header;
  memcpy(&expected_header, &header, sizeof(header));
  char expected_out[sizeof(out)];
  memcpy(expected_out, out, sizeof(out));
  CHECK(wait_for_poll(&f, &header, out, sizeof(out)) == 0);
  CHECK(memcmp(&header, &expected_header, sizeof(header)) == 0);
  CHECK(memcmp(out, expected_out, sizeof(out)) == 0);
  fixture_close(&f);
  return 0;
}

static int test_partial_audio_poll_moves_remainder(void) {
  TestFixture f;
  CHECK(fixture_open(&f, 1024u) == 0);
  uint8_t data[100];
  for (size_t i = 0u; i < sizeof(data); ++i) {
    data[i] = (uint8_t)i;
  }
  CHECK(write_frame(&f, data, sizeof(data), AGORA_SHM_MEDIA_AUDIO) == 0);

  char first[40];
  char second[60];
  AgoraShmIpcHeader header;
  CHECK(wait_for_poll(&f, &header, first, sizeof(first)) ==
        (ssize_t)sizeof(first));
  CHECK(memcmp(first, data, sizeof(first)) == 0);
  CHECK(header.data_len == (uint32_t)sizeof(first));
  CHECK(poll_api(f.receiver, f.shm_name, &header, second, sizeof(second)) ==
        (ssize_t)sizeof(second));
  CHECK(memcmp(second, data + sizeof(first), sizeof(second)) == 0);
  CHECK(header.data_len == (uint32_t)sizeof(second));
  fixture_close(&f);
  return 0;
}

static int test_video_enobufs_preserves_frame(void) {
  TestFixture f;
  CHECK(fixture_open(&f, 1024u) == 0);
  uint8_t data[100];
  memset(data, 0x3c, sizeof(data));
  CHECK(write_frame(&f, data, sizeof(data), AGORA_SHM_MEDIA_VIDEO) == 0);

  char small[50];
  AgoraShmIpcHeader header;
  memset(&header, 0x2d, sizeof(header));
  memset(small, 0x4e, sizeof(small));
  AgoraShmIpcHeader expected_header;
  memcpy(&expected_header, &header, sizeof(header));
  char expected_small[sizeof(small)];
  memcpy(expected_small, small, sizeof(small));
  ssize_t n = wait_for_poll(&f, &header, small, sizeof(small));
  CHECK(n == -1);
  CHECK(errno == ENOBUFS);
  CHECK(memcmp(&header, &expected_header, sizeof(header)) == 0);
  CHECK(memcmp(small, expected_small, sizeof(small)) == 0);

  char full[sizeof(data)];
  CHECK(poll_api(f.receiver, f.shm_name, &header, full, sizeof(full)) ==
        (ssize_t)sizeof(full));
  CHECK(memcmp(full, data, sizeof(data)) == 0);
  CHECK(header.data_len == (uint32_t)sizeof(data));
  fixture_close(&f);
  return 0;
}

static int test_callback_mode_remains_compatible(void) {
  TestFixture f;
  memset(&f, 0, sizeof(f));
  f.port = reserve_udp_port();
  CHECK(f.port != 0u);
  (void)snprintf(f.shm_name, sizeof(f.shm_name), "/acb_%ld_%u", (long)getpid(),
                 ++g_name_counter);
  (void)agora_shm_ipc_unlink(f.shm_name);

  CallbackCapture capture;
  memset(&capture, 0, sizeof(capture));
  atomic_init(&capture.called, 0);
  CHECK(agora_shm_manager_start(capture_frame, f.port, true, 4u, 1000u,
                                &capture, TEST_READ_CAP, &f.receiver) == 0);
  CHECK(agora_shm_manager_start(noop_frame, f.port, false, 0u, 0u, NULL,
                                TEST_READ_CAP, &f.writer) == 0);
  CHECK(agora_shm_manager_add(f.writer, f.shm_name, 1024u) == 0);

  uint8_t data[72];
  memset(data, 0x6d, sizeof(data));
  CHECK(write_frame(&f, data, sizeof(data), AGORA_SHM_MEDIA_VIDEO) == 0);
  for (unsigned i = 0u;
       i < 100u &&
       atomic_load_explicit(&capture.called, memory_order_acquire) == 0;
       ++i) {
    sleep_ms(10);
  }
  CHECK(atomic_load_explicit(&capture.called, memory_order_acquire) == 1);
  CHECK(capture.len == sizeof(data));
  CHECK(capture.seq == 2u);
  CHECK(memcmp(capture.payload, data, sizeof(data)) == 0);

  AgoraShmIpcHeader header;
  char out[sizeof(data)];
  errno = 0;
  CHECK(poll_api(f.receiver, f.shm_name, &header, out, sizeof(out)) == -1);
  CHECK(errno == ENOTSUP);
  fixture_close(&f);
  return 0;
}

static int test_remove_discards_poll_entry(void) {
  TestFixture f;
  CHECK(fixture_open(&f, 1024u) == 0);
  uint8_t data[32];
  memset(data, 0x7a, sizeof(data));
  CHECK(write_frame(&f, data, sizeof(data), AGORA_SHM_MEDIA_AUDIO) == 0);

  char out[sizeof(data)];
  AgoraShmIpcHeader header;
  CHECK(wait_for_poll(&f, &header, out, sizeof(out)) == (ssize_t)sizeof(out));
  CHECK(agora_shm_manager_remove(f.receiver, f.shm_name) == 0);
  errno = 0;
  CHECK(poll_api(f.receiver, f.shm_name, &header, out, sizeof(out)) == -1);
  CHECK(errno == ENOENT);
  fixture_close(&f);
  return 0;
}

static void *stress_writer_main(void *arg) {
  StressWriter *stress = (StressWriter *)arg;
  AgoraShmIpcFrameMeta meta = make_meta(stress->fixture, AGORA_SHM_MEDIA_VIDEO);
  for (unsigned i = 1u; i <= stress->iterations; ++i) {
    memset(stress->data, (int)(i & 0xffu), stress->len);
    if (agora_shm_manager_write(stress->fixture->writer,
                                stress->fixture->shm_name, stress->data,
                                stress->len, &meta) != 0) {
      atomic_store_explicit(&stress->failed, 1, memory_order_release);
      break;
    }
  }
  atomic_store_explicit(&stress->done, 1, memory_order_release);
  return NULL;
}

static int test_poll_frames_are_stable_under_concurrent_writes(void) {
  const size_t frame_size = 4u * 1024u * 1024u;
  TestFixture f;
  memset(&f, 0, sizeof(f));
  f.port = reserve_udp_port();
  CHECK(f.port != 0u);
  (void)snprintf(f.shm_name, sizeof(f.shm_name), "/astress_%ld_%u",
                 (long)getpid(), ++g_name_counter);
  (void)agora_shm_ipc_unlink(f.shm_name);
  CHECK(agora_shm_manager_start(NULL, f.port, true, 4u, 1000u, NULL, frame_size,
                                &f.receiver) == 0);
  CHECK(agora_shm_manager_start(noop_frame, f.port, false, 0u, 0u, NULL,
                                frame_size, &f.writer) == 0);
  CHECK(agora_shm_manager_add(f.writer, f.shm_name, frame_size) == 0);

  StressWriter stress;
  memset(&stress, 0, sizeof(stress));
  stress.fixture = &f;
  stress.data = (uint8_t *)malloc(frame_size);
  stress.len = frame_size;
  stress.iterations = 128u;
  CHECK(stress.data != NULL);
  atomic_init(&stress.failed, 0);
  atomic_init(&stress.done, 0);

  char *out = (char *)malloc(frame_size);
  CHECK(out != NULL);
  pthread_t thread;
  CHECK(pthread_create(&thread, NULL, stress_writer_main, &stress) == 0);

  unsigned frames_read = 0u;
  int torn = 0;
  for (unsigned attempt = 0u; attempt < 2000u; ++attempt) {
    AgoraShmIpcHeader header;
    ssize_t n = poll_api(f.receiver, f.shm_name, &header, out, frame_size);
    if (n == (ssize_t)frame_size) {
      ++frames_read;
      const unsigned char expected = (unsigned char)out[0];
      for (size_t i = 1u; i < frame_size; ++i) {
        if ((unsigned char)out[i] != expected) {
          torn = 1;
          break;
        }
      }
      if (torn != 0) {
        break;
      }
    } else if (n < 0 && errno != ENOENT) {
      CHECK(0);
    }
    if (atomic_load_explicit(&stress.done, memory_order_acquire) != 0 &&
        frames_read > 0u) {
      break;
    }
    sleep_ms(1);
  }

  CHECK(pthread_join(thread, NULL) == 0);
  CHECK(atomic_load_explicit(&stress.failed, memory_order_acquire) == 0);
  CHECK(frames_read > 0u);
  CHECK(torn == 0);
  free(out);
  free(stress.data);
  fixture_close(&f);
  return 0;
}

int main(void) {
  CHECK(test_poll_errors_and_empty_entry() == 0);
  CHECK(test_audio_append_and_header() == 0);
  CHECK(test_audio_keeps_latest_200ms() == 0);
  CHECK(test_single_audio_block_keeps_latest_200ms() == 0);
  CHECK(test_partial_audio_poll_moves_remainder() == 0);
  CHECK(test_video_replaces_previous_frame() == 0);
  CHECK(test_video_enobufs_preserves_frame() == 0);
  CHECK(test_duplicate_seq_is_ignored() == 0);
  CHECK(test_media_type_is_locked() == 0);
  CHECK(test_callback_mode_remains_compatible() == 0);
  CHECK(test_remove_discards_poll_entry() == 0);
  CHECK(test_poll_frames_are_stable_under_concurrent_writes() == 0);
  puts("PASS test_agora_shm_manager_poll");
  return 0;
}
