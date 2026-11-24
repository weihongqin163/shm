/* Fault injection stays in this test translation unit, outside the public API. */
#define _DARWIN_C_SOURCE
#define main original_poll_tests_main
#include "test_agora_shm_manager_poll.c"
#undef main

static atomic_int fault_mode;
static atomic_int poll_error;
static atomic_int poll_failures;
static atomic_int create_failures;
static atomic_int keepalive_failures;
static atomic_int send_failures;
static atomic_int injected_polls;
static atomic_int rebuild_attempts;
static atomic_int hold_create;
static atomic_int create_entered;
static int release_create;
static pthread_mutex_t create_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t create_condition = PTHREAD_COND_INITIALIZER;

static void create_barrier(void) {
  if (atomic_load(&hold_create)) {
    pthread_mutex_lock(&create_lock);
    atomic_store(&create_entered, 1);
    while (!release_create) {
      pthread_cond_wait(&create_condition, &create_lock);
    }
    pthread_mutex_unlock(&create_lock);
  }
}

static int consume_failure(atomic_int *remaining) {
  int n = atomic_load(remaining);
  while (n != 0) {
    if (n < 0 || atomic_compare_exchange_weak(remaining, &n, n - 1)) {
      return 1;
    }
  }
  return 0;
}

static int inject_poll(int server) {
  if (atomic_load(&fault_mode) == (server ? 1 : 2) &&
      consume_failure(&poll_failures)) {
    atomic_fetch_add(&injected_polls, 1);
    errno = atomic_load(&poll_error);
    return 1;
  }
  return 0;
}

static int recovery_server_poll(agora_localsock_server *s, int timeout,
                                 void *buf, size_t cap, size_t *len) {
  if (inject_poll(1)) {
    *len = 0;
    return -1;
  }
  return agora_localsock_server_poll(s, timeout, buf, cap, len);
}

static int recovery_client_poll(agora_localsock_client *c, int timeout,
                                 void *buf, size_t cap, size_t *len) {
  if (inject_poll(0)) {
    *len = 0;
    return -1;
  }
  return agora_localsock_client_poll(c, timeout, buf, cap, len);
}

static int inject_create(int server) {
  if (atomic_load(&fault_mode) != (server ? 1 : 2)) {
    return 0;
  }
  atomic_fetch_add(&rebuild_attempts, 1);
  if (consume_failure(&create_failures)) {
    errno = EMFILE;
    return 1;
  }
  return 0;
}

static int recovery_server_create(uint16_t port, uint32_t keepalive,
                                   size_t clients, agora_localsock_server **out) {
  if (inject_create(1)) {
    return -1;
  }
  int rc = agora_localsock_server_create(port, keepalive, clients, out);
  if (rc == 0) {
    create_barrier();
  }
  return rc;
}

static int recovery_client_create(uint16_t port, agora_localsock_client **out) {
  if (inject_create(0)) {
    return -1;
  }
  int rc = agora_localsock_client_create(port, out);
  if (rc == 0) {
    create_barrier();
  }
  return rc;
}

static int recovery_keepalive(agora_localsock_client *c) {
  if (atomic_load(&fault_mode) == 2 && consume_failure(&keepalive_failures)) {
    errno = EBADF;
    return -1;
  }
  return agora_localsock_client_send_keepalive(c);
}

static int recovery_server_send(agora_localsock_server *s, uint16_t type,
                                 const void *payload, size_t size) {
  if (atomic_load(&fault_mode) == 1 && consume_failure(&send_failures)) {
    errno = EBADF;
    return -1;
  }
  return agora_localsock_server_send_datagram(s, type, payload, size);
}

static int recovery_client_send(agora_localsock_client *c, uint16_t type,
                                 const void *payload, size_t size) {
  if (atomic_load(&fault_mode) == 2 && consume_failure(&send_failures)) {
    errno = EBADF;
    return -1;
  }
  return agora_localsock_client_send_datagram(c, type, payload, size);
}

#define agora_localsock_server_poll recovery_server_poll
#define agora_localsock_client_poll recovery_client_poll
#define agora_localsock_server_create recovery_server_create
#define agora_localsock_client_create recovery_client_create
#define agora_localsock_client_send_keepalive recovery_keepalive
#define agora_localsock_server_send_datagram recovery_server_send
#define agora_localsock_client_send_datagram recovery_client_send
#include "../src/agora_shm_manager.c"
#undef agora_localsock_server_poll
#undef agora_localsock_client_poll
#undef agora_localsock_server_create
#undef agora_localsock_client_create
#undef agora_localsock_client_send_keepalive
#undef agora_localsock_server_send_datagram
#undef agora_localsock_client_send_datagram

static void reset_faults(void) {
  atomic_store(&fault_mode, 0);
  atomic_store(&poll_error, EIO);
  atomic_store(&poll_failures, 0);
  atomic_store(&create_failures, 0);
  atomic_store(&keepalive_failures, 0);
  atomic_store(&send_failures, 0);
  atomic_store(&injected_polls, 0);
  atomic_store(&rebuild_attempts, 0);
  atomic_store(&hold_create, 0);
  atomic_store(&create_entered, 0);
  release_create = 0;
}

static int open_recovery_fixture(TestFixture *f, int server) {
  reset_faults();
  if (server) {
    return fixture_open(f, 256u);
  }
  memset(f, 0, sizeof(*f));
  f->port = reserve_udp_port();
  CHECK(f->port != 0);
  snprintf(f->shm_name, sizeof(f->shm_name), "/arec_%ld_%u",
           (long)getpid(), ++g_name_counter);
  CHECK(agora_shm_manager_start(NULL, f->port, true, 4u, 1000u, NULL,
                                TEST_READ_CAP, &f->writer) == 0);
  CHECK(agora_shm_manager_start(NULL, f->port, false, 0u, 0u, NULL,
                                TEST_READ_CAP, &f->receiver) == 0);
  CHECK(agora_shm_manager_add(f->writer, f->shm_name, 256u) == 0);
  /* Wait for the client's initial/periodic registration before server writes. */
  for (unsigned i = 0; i < 1000; ++i) {
    pthread_mutex_lock(&f->writer->transport_lock);
    size_t peers = agora_localsock_server_peer_count(f->writer->srv);
    pthread_mutex_unlock(&f->writer->transport_lock);
    if (peers != 0) {
      return 0;
    }
    sleep_ms(1);
  }
  CHECK(0);
}

static int wait_counter(atomic_int *counter, int minimum) {
  for (unsigned i = 0; i < 1500; ++i) {
    if (atomic_load(counter) >= minimum) {
      return 0;
    }
    sleep_ms(1);
  }
  CHECK(0);
}

static ssize_t wait_recovery_frame(TestFixture *f, char *out, size_t cap) {
  AgoraShmIpcHeader header;
  for (unsigned i = 0; i < 1500; ++i) {
    ssize_t n = agora_shm_manager_poll(f->receiver, f->shm_name, &header,
                                       out, cap);
    if (n > 0) {
      return n;
    }
    CHECK(n == 0 || n == -ENOENT || n == -EAGAIN);
    sleep_ms(1);
  }
  CHECK(0);
}

static int wait_recovery_state(AgoraShmManager *m, int recovering) {
  char byte;
  AgoraShmIpcHeader header;
  for (unsigned i = 0; i < 1500; ++i) {
    ssize_t n = agora_shm_manager_poll(m, "/unknown", &header, &byte, 1u);
    if (n == (recovering ? -EAGAIN : -ENOENT)) {
      return 0;
    }
    sleep_ms(1);
  }
  CHECK(0);
}

static int test_single_error_keeps_worker(int server, int error) {
  TestFixture f;
  CHECK(open_recovery_fixture(&f, server) == 0);
  atomic_store(&fault_mode, server ? 1 : 2);
  atomic_store(&poll_error, error);
  atomic_store(&poll_failures, 1);
  CHECK(wait_counter(&injected_polls, 1) == 0);
  CHECK(wait_recovery_state(f.receiver, 0) == 0);
  const char data[] = "after-one-error";
  CHECK(write_frame(&f, data, sizeof(data), AGORA_SHM_MEDIA_VIDEO) == 0);
  char out[64];
  CHECK(wait_recovery_frame(&f, out, sizeof(out)) == (ssize_t)sizeof(data));
  CHECK(memcmp(data, out, sizeof(data)) == 0);
  CHECK(atomic_load(&rebuild_attempts) == 0);
  fixture_close(&f);
  return 0;
}

static int test_rebuild_and_retained_shm(int server, int error, int failures) {
  TestFixture f;
  CHECK(open_recovery_fixture(&f, server) == 0);
  const char old[] = "cached-before-error";
  CHECK(write_frame(&f, old, sizeof(old), AGORA_SHM_MEDIA_VIDEO) == 0);
  char local_name[AGORA_SHM_IPC_SHM_NAME_BYTES];
  snprintf(local_name, sizeof(local_name), "/aloc_%ld_%u",
           (long)getpid(), ++g_name_counter);
  CHECK(agora_shm_manager_add(f.receiver, local_name, 256u) == 0);
  atomic_store(&fault_mode, server ? 1 : 2);
  atomic_store(&poll_error, error);
  atomic_store(&create_failures, -1);
  atomic_store(&poll_failures, failures);
  CHECK(wait_counter(&rebuild_attempts, 2) == 0);
  CHECK(atomic_load(&injected_polls) == failures);

  char out[64];
  AgoraShmIpcHeader header;
  CHECK(agora_shm_manager_poll(f.receiver, f.shm_name, &header, out,
                                sizeof(out)) == (ssize_t)sizeof(old));
  CHECK(memcmp(out, old, sizeof(old)) == 0);
  CHECK(agora_shm_manager_poll(f.receiver, f.shm_name, &header, out,
                                sizeof(out)) == -EAGAIN);
  CHECK(errno == EAGAIN);
  CHECK(wait_recovery_state(f.receiver, 1) == 0);

  AgoraShmIpcFrameMeta meta = make_meta(&f, AGORA_SHM_MEDIA_VIDEO);
  snprintf(meta.shm_name, sizeof(meta.shm_name), "%s", local_name);
  const char update[] = "written-during-recovery";
  CHECK(agora_shm_manager_write(f.receiver, local_name, update, sizeof(update),
                                 &meta) == -EAGAIN);
  CHECK(errno == EAGAIN);
  AgoraShmIpc direct;
  CHECK(agora_shm_ipc_open(local_name, 256u, 0, &direct) == 0);
  void *buf = out;
  size_t len = 0;
  CHECK(agora_shm_ipc_read(&direct, &buf, sizeof(out), &len, &header) == 0);
  CHECK(len == sizeof(update) && memcmp(out, update, len) == 0);
  agora_shm_ipc_close(&direct);
  CHECK(count_manifests_containing(local_name) == 1);

  int attempts = atomic_load(&rebuild_attempts);
  sleep_ms(120);
  CHECK(atomic_load(&rebuild_attempts) - attempts <= 8);
  atomic_store(&create_failures, 0);
  CHECK(wait_recovery_state(f.receiver, 0) == 0);
  CHECK(shm_exists(local_name) == 1);
  CHECK(count_manifests_containing(local_name) == 1);
  if (!server) {
    /* Recreated client uses a new address; wait until the server registers it. */
    sleep_ms(30);
  }
  const char fresh[] = "after-socket-rebuild";
  CHECK(write_frame(&f, fresh, sizeof(fresh), AGORA_SHM_MEDIA_VIDEO) == 0);
  CHECK(wait_recovery_frame(&f, out, sizeof(out)) == (ssize_t)sizeof(fresh));
  CHECK(memcmp(fresh, out, sizeof(fresh)) == 0);
  fixture_close(&f);
  CHECK(shm_exists(local_name) == 0);
  return 0;
}

static int test_close_during_recovery(int server) {
  TestFixture f;
  CHECK(open_recovery_fixture(&f, server) == 0);
  atomic_store(&fault_mode, server ? 1 : 2);
  atomic_store(&poll_error, EBADF);
  atomic_store(&create_failures, -1);
  atomic_store(&poll_failures, 1);
  CHECK(wait_counter(&rebuild_attempts, 2) == 0);
  struct timespec before, after;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
  agora_shm_manager_close(f.receiver);
  f.receiver = NULL;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
  double elapsed = (double)(after.tv_sec - before.tv_sec) +
                   (after.tv_nsec - before.tv_nsec) / 1e9;
  CHECK(elapsed < 0.5);
  fixture_close(&f);
  return 0;
}

static int test_keepalive_failure_recovers(void) {
  TestFixture f;
  CHECK(open_recovery_fixture(&f, 0) == 0);
  atomic_store(&fault_mode, 2);
  atomic_store(&keepalive_failures, 1);
  CHECK(wait_counter(&rebuild_attempts, 1) == 0);
  CHECK(wait_recovery_state(f.receiver, 0) == 0);
  sleep_ms(30);
  const char data[] = "after-keepalive-error";
  CHECK(write_frame(&f, data, sizeof(data), AGORA_SHM_MEDIA_VIDEO) == 0);
  char out[64];
  CHECK(wait_recovery_frame(&f, out, sizeof(out)) == (ssize_t)sizeof(data));
  fixture_close(&f);
  return 0;
}

static int test_success_resets_consecutive_errors(int server) {
  TestFixture f;
  CHECK(open_recovery_fixture(&f, server) == 0);
  atomic_store(&fault_mode, server ? 1 : 2);
  for (int round = 1; round <= 2; ++round) {
    atomic_store(&poll_failures, 2);
    CHECK(wait_counter(&injected_polls, round * 2) == 0);
    /* Wait until a subsequent successful poll clears recovery. */
    sleep_ms(30);
    CHECK(wait_recovery_state(f.receiver, 0) == 0);
    CHECK(atomic_load(&rebuild_attempts) == 0);
  }
  fixture_close(&f);
  return 0;
}

static int test_interrupted_polls_do_not_rebuild(int server) {
  TestFixture f;
  CHECK(open_recovery_fixture(&f, server) == 0);
  atomic_store(&fault_mode, server ? 1 : 2);
  atomic_store(&poll_error, EINTR);
  atomic_store(&poll_failures, 4);
  CHECK(wait_counter(&injected_polls, 4) == 0);
  /* A normal timeout after EINTR also must not cause reconstruction. */
  sleep_ms(30);
  atomic_store(&poll_error, EIO);
  atomic_store(&poll_failures, 2);
  CHECK(wait_counter(&injected_polls, 6) == 0);
  sleep_ms(30);
  CHECK(wait_recovery_state(f.receiver, 0) == 0);
  CHECK(atomic_load(&rebuild_attempts) == 0);
  fixture_close(&f);
  return 0;
}

static void *close_recovering_manager(void *arg) {
  agora_shm_manager_close(arg);
  return NULL;
}

static int test_close_races_successful_create(int server) {
  TestFixture f;
  CHECK(open_recovery_fixture(&f, server) == 0);
  atomic_store(&fault_mode, server ? 1 : 2);
  atomic_store(&poll_error, EBADF);
  atomic_store(&hold_create, 1);
  atomic_store(&poll_failures, 1);
  CHECK(wait_counter(&create_entered, 1) == 0);
  pthread_t closer;
  CHECK(pthread_create(&closer, NULL, close_recovering_manager, f.receiver) == 0);
  CHECK(wait_counter(&f.receiver->stop, 1) == 0);
  pthread_mutex_lock(&create_lock);
  release_create = 1;
  pthread_cond_broadcast(&create_condition);
  pthread_mutex_unlock(&create_lock);
  CHECK(pthread_join(closer, NULL) == 0);
  f.receiver = NULL;
  fixture_close(&f);
  return 0;
}

typedef struct RecoveryWrites {
  AgoraShmManager *manager;
  char name[AGORA_SHM_IPC_SHM_NAME_BYTES];
  atomic_int done;
  atomic_int failed;
} RecoveryWrites;

static void *write_during_rebuilds(void *arg) {
  RecoveryWrites *writes = arg;
  AgoraShmIpcFrameMeta meta = {0};
  snprintf(meta.shm_name, sizeof(meta.shm_name), "%s", writes->name);
  meta.media_type = AGORA_SHM_MEDIA_VIDEO;
  char data[16] = {0};
  while (!atomic_load(&writes->done)) {
    int rc = agora_shm_manager_write(writes->manager, writes->name, data,
                                      sizeof(data), &meta);
    if (rc != 0 && rc != -EAGAIN) {
      atomic_store(&writes->failed, 1);
      break;
    }
    sleep_ms(1);
  }
  return NULL;
}

static int test_concurrent_writes_and_rebuild(int server) {
  TestFixture f;
  CHECK(open_recovery_fixture(&f, server) == 0);
  RecoveryWrites writes = {.manager = f.receiver};
  snprintf(writes.name, sizeof(writes.name), "/acon_%ld_%u",
           (long)getpid(), ++g_name_counter);
  CHECK(agora_shm_manager_add(f.receiver, writes.name, 256u) == 0);
  atomic_init(&writes.done, 0);
  atomic_init(&writes.failed, 0);
  pthread_t writer;
  CHECK(pthread_create(&writer, NULL, write_during_rebuilds, &writes) == 0);
  atomic_store(&fault_mode, server ? 1 : 2);
  atomic_store(&poll_error, EBADF);
  for (int round = 1; round <= 8; ++round) {
    atomic_store(&create_failures, 2);
    atomic_store(&poll_failures, 1);
    CHECK(wait_counter(&rebuild_attempts, round * 3) == 0);
    CHECK(wait_recovery_state(f.receiver, 0) == 0);
  }
  atomic_store(&writes.done, 1);
  CHECK(pthread_join(writer, NULL) == 0);
  CHECK(atomic_load(&writes.failed) == 0);
  CHECK(shm_exists(writes.name) == 1);
  fixture_close(&f);
  CHECK(shm_exists(writes.name) == 0);
  return 0;
}

static int test_send_detects_invalid_socket(int server) {
  TestFixture f;
  CHECK(open_recovery_fixture(&f, server) == 0);
  char local[AGORA_SHM_IPC_SHM_NAME_BYTES];
  snprintf(local, sizeof(local), "/asnd_%ld_%u", (long)getpid(), ++g_name_counter);
  CHECK(agora_shm_manager_add(f.receiver, local, 256u) == 0);
  AgoraShmIpcFrameMeta meta = make_meta(&f, AGORA_SHM_MEDIA_VIDEO);
  snprintf(meta.shm_name, sizeof(meta.shm_name), "%s", local);
  atomic_store(&fault_mode, server ? 1 : 2);
  atomic_store(&create_failures, -1);
  atomic_store(&send_failures, 1);
  const char data[] = "send-detected-invalid-socket";
  CHECK(agora_shm_manager_write(f.receiver, local, data, sizeof(data), &meta) ==
        -EBADF);
  CHECK(errno == EBADF);
  CHECK(wait_counter(&rebuild_attempts, 2) == 0);
  CHECK(wait_recovery_state(f.receiver, 1) == 0);
  atomic_store(&create_failures, 0);
  CHECK(wait_recovery_state(f.receiver, 0) == 0);
  CHECK(shm_exists(local) == 1);
  fixture_close(&f);
  CHECK(shm_exists(local) == 0);
  return 0;
}

static int test_callback_after_rebuild(void) {
  TestFixture f;
  CHECK(open_recovery_fixture(&f, 1) == 0);
  agora_shm_manager_close(f.receiver);
  f.receiver = NULL;
  CallbackCapture capture = {0};
  atomic_init(&capture.called, 0);
  CHECK(agora_shm_manager_start(capture_frame, f.port, true, 4u, 1000u,
                                &capture, TEST_READ_CAP, &f.receiver) == 0);
  atomic_store(&fault_mode, 1);
  atomic_store(&poll_error, EBADF);
  atomic_store(&create_failures, 2);
  atomic_store(&poll_failures, 1);
  CHECK(wait_counter(&rebuild_attempts, 3) == 0);
  for (unsigned i = 0; i < 1500 && atomic_load(&f.receiver->recovering); ++i) {
    sleep_ms(1);
  }
  CHECK(atomic_load(&f.receiver->recovering) == 0);
  const char data[] = "callback-after-rebuild";
  CHECK(write_frame(&f, data, sizeof(data), AGORA_SHM_MEDIA_VIDEO) == 0);
  CHECK(wait_counter(&capture.called, 1) == 0);
  CHECK(capture.len == sizeof(data));
  CHECK(memcmp(capture.payload, data, sizeof(data)) == 0);
  fixture_close(&f);
  return 0;
}

int main(void) {
  CHECK(test_single_error_keeps_worker(1, EIO) == 0);
  CHECK(test_single_error_keeps_worker(0, EIO) == 0);
  CHECK(test_single_error_keeps_worker(1, EINTR) == 0);
  CHECK(test_single_error_keeps_worker(0, EINTR) == 0);
  CHECK(test_success_resets_consecutive_errors(1) == 0);
  CHECK(test_success_resets_consecutive_errors(0) == 0);
  CHECK(test_interrupted_polls_do_not_rebuild(1) == 0);
  CHECK(test_interrupted_polls_do_not_rebuild(0) == 0);
  CHECK(test_rebuild_and_retained_shm(1, EIO, 3) == 0);
  CHECK(test_rebuild_and_retained_shm(0, EIO, 3) == 0);
  CHECK(test_rebuild_and_retained_shm(1, EBADF, 1) == 0);
  CHECK(test_rebuild_and_retained_shm(0, EBADF, 1) == 0);
  CHECK(test_close_during_recovery(1) == 0);
  CHECK(test_close_during_recovery(0) == 0);
  CHECK(test_close_races_successful_create(1) == 0);
  CHECK(test_close_races_successful_create(0) == 0);
  CHECK(test_keepalive_failure_recovers() == 0);
  CHECK(test_concurrent_writes_and_rebuild(1) == 0);
  CHECK(test_concurrent_writes_and_rebuild(0) == 0);
  CHECK(test_send_detects_invalid_socket(1) == 0);
  CHECK(test_send_detects_invalid_socket(0) == 0);
  CHECK(test_callback_after_rebuild() == 0);
  puts("PASS test_agora_shm_manager_recovery");
  return 0;
}
