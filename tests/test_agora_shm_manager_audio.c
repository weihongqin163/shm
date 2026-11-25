/* Deterministic worker deadlines; injection remains private to this test. */
#define _DARWIN_C_SOURCE
#define main original_poll_tests_main
#include "test_agora_shm_manager_poll.c"
#undef main

static int simulated_clock;
static uint64_t simulated_ns;
static uint64_t write_duration_ns;
static AgoraShmManager *single_tick_manager;

static int audio_clock_gettime(clockid_t clock, struct timespec *ts) {
  if (simulated_clock && clock == CLOCK_MONOTONIC) {
    ts->tv_sec = (time_t)(simulated_ns / 1000000000ull);
    ts->tv_nsec = (long)(simulated_ns % 1000000000ull);
    return 0;
  }
  return clock_gettime(clock, ts);
}

static int audio_server_poll(agora_localsock_server *server, int timeout,
                              void *buf, size_t cap, size_t *len);

static int audio_ipc_write(AgoraShmIpc *ipc, const void *data, size_t len,
                            const AgoraShmIpcFrameMeta *meta) {
  int rc = agora_shm_ipc_write(ipc, data, len, meta);
  if (simulated_clock) {
    simulated_ns += write_duration_ns;
  }
  return rc;
}

#define clock_gettime audio_clock_gettime
#define agora_localsock_server_poll audio_server_poll
#define agora_shm_ipc_write audio_ipc_write
#include "../src/agora_shm_manager.c"
#undef clock_gettime
#undef agora_localsock_server_poll
#undef agora_shm_ipc_write

static int audio_server_poll(agora_localsock_server *server, int timeout,
                              void *buf, size_t cap, size_t *len) {
  if (single_tick_manager != NULL) {
    atomic_store(&single_tick_manager->stop, 1);
    *len = 0;
    return 0;
  }
  return agora_localsock_server_poll(server, timeout, buf, cap, len);
}

static int audio_fixture_open(TestFixture *f, size_t payload_size) {
  memset(f, 0, sizeof(*f));
  f->port = reserve_udp_port();
  CHECK(f->port != 0);
  snprintf(f->shm_name, sizeof(f->shm_name), "/abuf_%ld_%u",
           (long)getpid(), ++g_name_counter);
  CHECK(agora_shm_manager_start(NULL, f->port, true, 4, 1000, NULL,
                                TEST_READ_CAP, &f->writer) == 0);
  atomic_store(&f->writer->stop, 1);
  manager_wake(f->writer);
  CHECK(pthread_join(f->writer->worker, NULL) == 0);
  f->writer->worker_started = 0;
  CHECK(agora_shm_manager_add(f->writer, f->shm_name, payload_size) == 0);
  simulated_ns = 1000000000ull;
  write_duration_ns = 0;
  simulated_clock = 1;
  return 0;
}

static void audio_fixture_close(TestFixture *f) {
  simulated_clock = 0;
  fixture_close(f);
}

static void audio_tick(TestFixture *f, uint64_t elapsed_ms) {
  simulated_ns += elapsed_ms * 1000000ull;
  single_tick_manager = f->writer;
  atomic_store(&f->writer->stop, 0);
  (void)manager_worker(f->writer);
  single_tick_manager = NULL;
}

static AgoraShmIpc *audio_ipc(TestFixture *f, const char *name) {
  int index = find_write_entry_by_name(f->writer, name);
  return index < 0 ? NULL : &f->writer->write_entries[index].ipc;
}

static int expect_audio(TestFixture *f, const char *name, const void *expected,
                         size_t expected_len, unsigned expected_seq) {
  uint8_t out[TEST_READ_CAP];
  void *dst = out;
  size_t len = 0;
  AgoraShmIpcHeader header;
  CHECK(agora_shm_ipc_read(audio_ipc(f, name), &dst, sizeof(out), &len,
                           &header) == 0);
  CHECK(len == expected_len);
  CHECK(memcmp(out, expected, len) == 0);
  CHECK(atomic_load(&header.seq) == expected_seq);
  CHECK(header.frame_meta.sample_rate == 48000);
  return 0;
}

static int test_burst_is_copied_and_deferred(void) {
  TestFixture f;
  CHECK(audio_fixture_open(&f, 4096) == 0);
  AgoraShmIpcFrameMeta meta = make_meta(&f, AGORA_SHM_MEDIA_AUDIO);
  uint8_t input[960], expected[2880];
  for (unsigned i = 0; i < 3; ++i) {
    memset(input, 'A' + (int)i, sizeof(input));
    memcpy(expected + i * sizeof(input), input, sizeof(input));
    CHECK(agora_shm_manager_write(f.writer, f.shm_name, input, sizeof(input),
                                  &meta) == 0);
  }
  memset(input, 0, sizeof(input));
  meta.sample_rate = 8000;
  CHECK(audio_ipc(&f, f.shm_name)->header->data_len == 0);
  audio_tick(&f, 9);
  CHECK(audio_ipc(&f, f.shm_name)->header->data_len == 0);
  audio_tick(&f, 1);
  CHECK(expect_audio(&f, f.shm_name, expected, sizeof(expected), 2) == 0);
  audio_fixture_close(&f);
  return 0;
}

static int test_four_frame_limit_and_no_catchup(void) {
  TestFixture f;
  CHECK(audio_fixture_open(&f, 4096) == 0);
  AgoraShmIpcFrameMeta meta = make_meta(&f, AGORA_SHM_MEDIA_AUDIO);
  uint8_t frame[960], first[3840], last[960];
  for (unsigned i = 0; i < 5; ++i) {
    memset(frame, 'A' + (int)i, sizeof(frame));
    if (i < 4) {
      memcpy(first + i * sizeof(frame), frame, sizeof(frame));
    } else {
      memcpy(last, frame, sizeof(frame));
    }
    CHECK(agora_shm_manager_write(f.writer, f.shm_name, frame, sizeof(frame),
                                  &meta) == 0);
  }
  audio_tick(&f, 100);
  CHECK(expect_audio(&f, f.shm_name, first, sizeof(first), 2) == 0);
  audio_tick(&f, 0);
  audio_tick(&f, 9);
  CHECK(expect_audio(&f, f.shm_name, first, sizeof(first), 2) == 0);
  audio_tick(&f, 1);
  CHECK(expect_audio(&f, f.shm_name, last, sizeof(last), 4) == 0);
  audio_fixture_close(&f);
  return 0;
}

static int test_capacity_and_metadata_boundaries(void) {
  TestFixture f;
  CHECK(audio_fixture_open(&f, 8) == 0);
  AgoraShmIpcFrameMeta meta = make_meta(&f, AGORA_SHM_MEDIA_AUDIO);
  CHECK(agora_shm_manager_write(f.writer, f.shm_name, "AAAA", 4, &meta) == 0);
  CHECK(agora_shm_manager_write(f.writer, f.shm_name, "BBBB", 4, &meta) == 0);
  CHECK(agora_shm_manager_write(f.writer, f.shm_name, "CCCC", 4, &meta) == 0);
  meta.orientation = 180;
  CHECK(agora_shm_manager_write(f.writer, f.shm_name, "DDDD", 4, &meta) == 0);
  audio_tick(&f, 10);
  CHECK(expect_audio(&f, f.shm_name, "AAAABBBB", 8, 2) == 0);
  audio_tick(&f, 10);
  CHECK(expect_audio(&f, f.shm_name, "CCCC", 4, 4) == 0);
  audio_tick(&f, 10);
  CHECK(expect_audio(&f, f.shm_name, "DDDD", 4, 6) == 0);
  CHECK(audio_ipc(&f, f.shm_name)->header->frame_meta.orientation == 180);
  audio_fixture_close(&f);
  return 0;
}

static int test_independent_names_and_video(void) {
  TestFixture f;
  CHECK(audio_fixture_open(&f, 32) == 0);
  char other[AGORA_SHM_IPC_SHM_NAME_BYTES];
  snprintf(other, sizeof(other), "/abuf_other_%ld", (long)getpid());
  CHECK(agora_shm_manager_add(f.writer, other, 32) == 0);
  AgoraShmIpcFrameMeta meta = make_meta(&f, AGORA_SHM_MEDIA_AUDIO);
  CHECK(agora_shm_manager_write(f.writer, f.shm_name, "AAAA", 4, &meta) == 0);
  snprintf(meta.shm_name, sizeof(meta.shm_name), "%s", other);
  CHECK(agora_shm_manager_write(f.writer, other, "BBBB", 4, &meta) == 0);
  audio_tick(&f, 10);
  CHECK(expect_audio(&f, f.shm_name, "AAAA", 4, 2) == 0);
  CHECK(expect_audio(&f, other, "BBBB", 4, 2) == 0);
  meta.media_type = AGORA_SHM_MEDIA_VIDEO;
  CHECK(agora_shm_manager_write(f.writer, other, "V", 1, &meta) == 0);
  CHECK(audio_ipc(&f, other)->header->data_len == 1);
  CHECK(audio_ipc(&f, other)->payload[0] == 'V');
  audio_fixture_close(&f);
  return 0;
}

static int test_backlog_keeps_latest_200ms(void) {
  TestFixture f;
  CHECK(audio_fixture_open(&f, 4096) == 0);
  AgoraShmIpcFrameMeta meta = make_meta(&f, AGORA_SHM_MEDIA_AUDIO);
  uint8_t frame[960], expected[3840];
  for (unsigned i = 0; i < 24; ++i) {
    memset(frame, (int)i, sizeof(frame));
    CHECK(agora_shm_manager_write(f.writer, f.shm_name, frame, sizeof(frame),
                                  &meta) == 0);
    if (i >= 4 && i < 8) {
      memcpy(expected + (i - 4) * sizeof(frame), frame, sizeof(frame));
    }
  }
  audio_tick(&f, 10);
  CHECK(expect_audio(&f, f.shm_name, expected, sizeof(expected), 2) == 0);
  /* Closing with pending frames must release their owned buffers. */
  audio_fixture_close(&f);
  return 0;
}

static int test_invalid_audio_and_remove(void) {
  TestFixture f;
  CHECK(audio_fixture_open(&f, 64) == 0);
  AgoraShmIpcFrameMeta valid = make_meta(&f, AGORA_SHM_MEDIA_AUDIO);
  AgoraShmIpcFrameMeta meta = valid;
  meta.sample_rate = 0;
  CHECK(agora_shm_manager_write(f.writer, f.shm_name, "AA", 2, &meta) == -EINVAL);
  meta = valid;
  meta.channels = -1;
  CHECK(agora_shm_manager_write(f.writer, f.shm_name, "AA", 2, &meta) == -EINVAL);
  meta = valid;
  meta.bits = 7;
  CHECK(agora_shm_manager_write(f.writer, f.shm_name, "AA", 2, &meta) == -EINVAL);
  CHECK(agora_shm_manager_write(f.writer, f.shm_name, "A", 1, &valid) == -EINVAL);
  CHECK(agora_shm_manager_write(f.writer, f.shm_name, "", 0, &valid) == -EINVAL);
  CHECK(agora_shm_manager_write(f.writer, f.shm_name, NULL, 2, &valid) == -EINVAL);
  CHECK(agora_shm_manager_write(f.writer, f.shm_name, "AA", 2, &valid) == 0);
  CHECK(agora_shm_manager_remove(f.writer, f.shm_name) == 0);
  CHECK(agora_shm_manager_add(f.writer, f.shm_name, 64) == 0);
  audio_tick(&f, 10);
  CHECK(audio_ipc(&f, f.shm_name)->header->data_len == 0);
  audio_fixture_close(&f);
  return 0;
}

static int test_audio_commits_during_transport_recovery(void) {
  TestFixture f;
  CHECK(audio_fixture_open(&f, 32) == 0);
  AgoraShmIpcFrameMeta meta = make_meta(&f, AGORA_SHM_MEDIA_AUDIO);
  atomic_store(&f.writer->recovering, 1);
  CHECK(agora_shm_manager_write(f.writer, f.shm_name, "AAAA", 4, &meta) == 0);
  CHECK(agora_shm_manager_write(f.writer, f.shm_name, "BBBB", 4, &meta) == 0);
  audio_tick(&f, 10);
  CHECK(expect_audio(&f, f.shm_name, "AAAABBBB", 8, 2) == 0);
  audio_tick(&f, 10);
  CHECK(expect_audio(&f, f.shm_name, "AAAABBBB", 8, 2) == 0);
  audio_fixture_close(&f);
  return 0;
}

static int test_overflow_does_not_postpone_deadline(void) {
  TestFixture f;
  CHECK(audio_fixture_open(&f, 19200) == 0);
  AgoraShmIpcFrameMeta meta = make_meta(&f, AGORA_SHM_MEDIA_AUDIO);
  uint8_t frame[19200];
  memset(frame, 'A', sizeof(frame));
  CHECK(agora_shm_manager_write(f.writer, f.shm_name, frame, sizeof(frame),
                                &meta) == 0);
  simulated_ns += 5000000ull;
  memset(frame, 'B', sizeof(frame));
  CHECK(agora_shm_manager_write(f.writer, f.shm_name, frame, sizeof(frame),
                                &meta) == 0);
  audio_tick(&f, 5);
  CHECK(expect_audio(&f, f.shm_name, frame, sizeof(frame), 2) == 0);
  audio_fixture_close(&f);
  return 0;
}

static int test_slow_commit_does_not_catch_up(void) {
  TestFixture f;
  CHECK(audio_fixture_open(&f, 32) == 0);
  AgoraShmIpcFrameMeta meta = make_meta(&f, AGORA_SHM_MEDIA_AUDIO);
  for (int i = 0; i < 5; ++i) {
    CHECK(agora_shm_manager_write(f.writer, f.shm_name, "AA", 2, &meta) == 0);
  }
  write_duration_ns = 20000000ull;
  audio_tick(&f, 10);
  CHECK(expect_audio(&f, f.shm_name, "AAAAAAAA", 8, 2) == 0);
  write_duration_ns = 0;
  audio_tick(&f, 0);
  audio_tick(&f, 9);
  CHECK(expect_audio(&f, f.shm_name, "AAAAAAAA", 8, 2) == 0);
  audio_tick(&f, 1);
  CHECK(expect_audio(&f, f.shm_name, "AA", 2, 4) == 0);
  audio_fixture_close(&f);
  return 0;
}

static int test_format_change_reconfigures_receive_cache(void) {
  TestFixture f;
  CHECK(audio_fixture_open(&f, 20000) == 0);
  AgoraShmIpcFrameMeta meta = make_meta(&f, AGORA_SHM_MEDIA_AUDIO);
  uint8_t old[6400], current[19200], out[19200];
  memset(old, 'A', sizeof(old));
  memset(current, 'B', sizeof(current));
  AgoraShmManagerReadEntry receiver = {0};
  AgoraShmIpcHeader header = {0};
  atomic_init(&header.seq, 2);
  header.frame_meta = meta;
  header.frame_meta.sample_rate = 16000;
  manager_buffer_frame(f.writer, &receiver, old, sizeof(old), &header);
  atomic_store(&header.seq, 4);
  header.frame_meta = meta;
  manager_buffer_frame(f.writer, &receiver, current, sizeof(current), &header);
  CHECK(receiver.data_len == sizeof(current));
  CHECK(receiver.buffer_capacity == sizeof(current));
  memcpy(out, receiver.buffer, sizeof(out));
  CHECK(memcmp(out, current, sizeof(out)) == 0);
  clear_read_entry(&receiver);
  audio_fixture_close(&f);
  return 0;
}

static int test_real_worker_delivers_burst(void) {
  TestFixture f;
  CHECK(fixture_open(&f, 4096) == 0);
  AgoraShmIpcFrameMeta meta = make_meta(&f, AGORA_SHM_MEDIA_AUDIO);
  uint8_t frame[960], expected[2880];
  for (unsigned i = 0; i < 3; ++i) {
    memset(frame, 'A' + (int)i, sizeof(frame));
    memcpy(expected + i * sizeof(frame), frame, sizeof(frame));
    CHECK(agora_shm_manager_write(f.writer, f.shm_name, frame, sizeof(frame),
                                  &meta) == 0);
  }
  char out[sizeof(expected)];
  size_t received = 0;
  for (unsigned attempt = 0; attempt < 200 && received < sizeof(out); ++attempt) {
    AgoraShmIpcHeader header;
    ssize_t n = agora_shm_manager_poll(f.receiver, f.shm_name, &header,
                                       out + received, sizeof(out) - received);
    CHECK(n >= 0 || errno == ENOENT);
    if (n > 0) {
      received += (size_t)n;
    }
    sleep_ms(5);
  }
  CHECK(received == sizeof(expected));
  CHECK(memcmp(out, expected, sizeof(out)) == 0);
  fixture_close(&f);
  return 0;
}

int main(int argc, char **argv) {
  const char *scenario = argc > 1 ? argv[1] : "all";
  if (strcmp(scenario, "real") == 0) {
    CHECK(test_real_worker_delivers_burst() == 0);
  } else {
    CHECK(test_burst_is_copied_and_deferred() == 0);
    CHECK(test_four_frame_limit_and_no_catchup() == 0);
    CHECK(test_capacity_and_metadata_boundaries() == 0);
    CHECK(test_independent_names_and_video() == 0);
    CHECK(test_backlog_keeps_latest_200ms() == 0);
    CHECK(test_invalid_audio_and_remove() == 0);
    CHECK(test_audio_commits_during_transport_recovery() == 0);
    CHECK(test_overflow_does_not_postpone_deadline() == 0);
    CHECK(test_slow_commit_does_not_catch_up() == 0);
    CHECK(test_format_change_reconfigures_receive_cache() == 0);
    CHECK(test_real_worker_delivers_burst() == 0);
  }
  puts("PASS test_agora_shm_manager_audio");
  return 0;
}
