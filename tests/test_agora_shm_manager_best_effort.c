/* State fault injection is private to this test translation unit. */
#define _DARWIN_C_SOURCE
#define main original_poll_tests_main
#include "test_agora_shm_manager_poll.c"
#undef main

static char isolated_root[] = "/tmp/agora_shm_best_effort_XXXXXX";
static const char *state_root = isolated_root;
static int open_error;
static int add_error;

static int best_effort_state_open(AgoraShmManagerState *state) {
  if (open_error != 0) {
    errno = open_error;
    return -1;
  }
  return agora_shm_manager_state_open_at(state, state_root);
}

static int best_effort_state_add(AgoraShmManagerState *state,
                                 const char *shm_name) {
  if (add_error != 0) {
    errno = add_error;
    return -1;
  }
  return agora_shm_manager_state_add(state, shm_name);
}

#define agora_shm_manager_state_open best_effort_state_open
#define agora_shm_manager_state_add best_effort_state_add
#include "../src/agora_shm_manager.c"
#undef agora_shm_manager_state_open
#undef agora_shm_manager_state_add

static int open_media_fixture(TestFixture *f, int server_writer) {
  memset(f, 0, sizeof(*f));
  f->port = reserve_udp_port();
  CHECK(f->port != 0u);
  snprintf(f->shm_name, sizeof(f->shm_name), "/abest_%ld_%u",
           (long)getpid(), ++g_name_counter);
  if (server_writer) {
    CHECK(agora_shm_manager_start(noop_frame, f->port, true, 4u, 1000u, NULL,
                                  TEST_READ_CAP, &f->writer) == 0);
    CHECK(agora_shm_manager_start(NULL, f->port, false, 0u, 0u, NULL,
                                  TEST_READ_CAP, &f->receiver) == 0);
    /* The client must register its address before the server can notify it. */
    sleep_ms(30);
  } else {
    CHECK(agora_shm_manager_start(NULL, f->port, true, 4u, 1000u, NULL,
                                  TEST_READ_CAP, &f->receiver) == 0);
    CHECK(agora_shm_manager_start(noop_frame, f->port, false, 0u, 0u, NULL,
                                  TEST_READ_CAP, &f->writer) == 0);
  }
  return 0;
}

static int verify_media_lifecycle(TestFixture *f) {
  CHECK(agora_shm_manager_add(f->writer, f->shm_name, 256u) == 0);
  CHECK(shm_exists(f->shm_name) == 1);
  CHECK(agora_shm_manager_add(f->writer, f->shm_name, 256u) == -EEXIST);
  CHECK(errno == EEXIST);
  CHECK(agora_shm_manager_add(f->writer, "/invalid_size", 0u) == -EINVAL);

  const char payload[] = "media-without-persistent-state";
  CHECK(write_frame(f, payload, sizeof(payload), AGORA_SHM_MEDIA_VIDEO) == 0);
  char out[128];
  AgoraShmIpcHeader header;
  CHECK(wait_for_poll(f, &header, out, sizeof(out)) == (ssize_t)sizeof(payload));
  CHECK(memcmp(out, payload, sizeof(payload)) == 0);

  CHECK(agora_shm_manager_remove(f->writer, f->shm_name) == 0);
  CHECK(shm_exists(f->shm_name) == 0);
  CHECK(wait_for_missing_read_entry(f->receiver, f->shm_name) == 0);
  CHECK(agora_shm_manager_add(f->writer, f->shm_name, 256u) == 0);
  fixture_close(f);
  CHECK(shm_exists(f->shm_name) == 0);
  return 0;
}

static int test_open_failure(int server_writer, int error) {
  TestFixture f;
  open_error = error;
  CHECK(open_media_fixture(&f, server_writer) == 0);
  open_error = 0;
  CHECK(f.writer->state.lock_fd == -1);
  CHECK(f.receiver->state.lock_fd == -1);
  CHECK(verify_media_lifecycle(&f) == 0);
  return 0;
}

static int test_open_at_failure(int server_writer) {
  char blocked[PATH_MAX];
  snprintf(blocked, sizeof(blocked), "%s/not-a-directory", isolated_root);
  int fd = open(blocked, O_CREAT | O_EXCL | O_WRONLY, 0600);
  CHECK(fd >= 0);
  CHECK(close(fd) == 0);
  state_root = blocked;
  TestFixture f;
  CHECK(open_media_fixture(&f, server_writer) == 0);
  state_root = isolated_root;
  CHECK(f.writer->state.lock_fd == -1);
  CHECK(verify_media_lifecycle(&f) == 0);
  CHECK(unlink(blocked) == 0);
  return 0;
}

static int test_add_failure(int server_writer, int error) {
  TestFixture f;
  CHECK(open_media_fixture(&f, server_writer) == 0);
  add_error = error;
  CHECK(verify_media_lifecycle(&f) == 0);
  add_error = 0;
  return 0;
}

static int test_deleted_instance_directory(int server_writer) {
  TestFixture f;
  CHECK(open_media_fixture(&f, server_writer) == 0);
  CHECK(strncmp(f.writer->state.dir_path, isolated_root,
                  strlen(isolated_root)) == 0);
  int fd = open(f.writer->state.dir_path, O_RDONLY | O_DIRECTORY);
  CHECK(fd >= 0);
  CHECK(unlinkat(fd, "shm.list", 0) == 0);
  CHECK(unlinkat(fd, "owner.lock", 0) == 0);
  CHECK(close(fd) == 0);
  CHECK(rmdir(f.writer->state.dir_path) == 0);
  /* Keep the original lock fd open, matching a live process after deletion. */
  CHECK(fcntl(f.writer->state.lock_fd, F_GETFD) >= 0);
  CHECK(verify_media_lifecycle(&f) == 0);
  return 0;
}

int main(int argc, char **argv) {
  CHECK(mkdtemp(isolated_root) != NULL);
  const char *scenario = argc > 1 ? argv[1] : "all";
  int all = strcmp(scenario, "all") == 0;
  const int errors[] = {EACCES, ENOSPC, EIO};
  for (int server = 0; server <= 1; ++server) {
    if (all || strcmp(scenario, "open") == 0) {
      for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        CHECK(test_open_failure(server, errors[i]) == 0);
      }
    }
    if (all || strcmp(scenario, "open_at") == 0) {
      CHECK(test_open_at_failure(server) == 0);
    }
    if (all || strcmp(scenario, "add") == 0) {
      for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        CHECK(test_add_failure(server, errors[i]) == 0);
      }
    }
    if (all || strcmp(scenario, "deleted") == 0) {
      CHECK(test_deleted_instance_directory(server) == 0);
    }
  }
  /* Failed state updates may intentionally retain empty recovery directories. */
  AgoraShmManagerState cleanup;
  CHECK(agora_shm_manager_state_open_at(&cleanup, isolated_root) == 0);
  agora_shm_manager_state_close(&cleanup);
  CHECK(rmdir(isolated_root) == 0);
  puts("PASS test_agora_shm_manager_best_effort");
  return 0;
}
