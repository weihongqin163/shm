#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "agora_shm_manager_state.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL %s:%d: %s (errno=%d)\n", __FILE__, __LINE__,       \
              #cond, errno);                                                   \
      return -1;                                                               \
    }                                                                          \
  } while (0)

static unsigned g_name_counter;

static void make_shm_name(char *out, size_t cap, const char *tag) {
  (void)snprintf(out, cap, "/astate_%s_%ld_%u", tag, (long)getpid(),
                 ++g_name_counter);
}

static int create_test_shm(const char *shm_name) {
  AgoraShmIpc ipc;
  memset(&ipc, 0, sizeof(ipc));
  if (agora_shm_ipc_open(shm_name, 64u, 1, &ipc) != 0) {
    return -1;
  }
  agora_shm_ipc_close(&ipc);
  return 0;
}

static int shm_exists(const char *shm_name) {
  int fd = shm_open(shm_name, O_RDWR, 0);
  if (fd >= 0) {
    (void)close(fd);
    return 1;
  }
  return 0;
}

static int manifest_contains(const char *dir_path, const char *shm_name) {
  char path[PATH_MAX];
  int n = snprintf(path, sizeof(path), "%s/shm.list", dir_path);
  if (n < 0 || (size_t)n >= sizeof(path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  FILE *fp = fopen(path, "r");
  if (fp == NULL) {
    return -1;
  }
  char line[AGORA_SHM_IPC_SHM_NAME_BYTES + 2u];
  int found = 0;
  while (fgets(line, sizeof(line), fp) != NULL) {
    line[strcspn(line, "\n")] = '\0';
    if (strcmp(line, shm_name) == 0) {
      found = 1;
      break;
    }
  }
  int saved_errno = errno;
  (void)fclose(fp);
  errno = saved_errno;
  return found;
}

static int wait_child_ok(pid_t child) {
  int status = 0;
  if (waitpid(child, &status, 0) != child) {
    return -1;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    errno = ECHILD;
    return -1;
  }
  return 0;
}

static int write_file(const char *path, const char *contents) {
  int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
  if (fd < 0) {
    return -1;
  }
  size_t len = strlen(contents);
  ssize_t nw = write(fd, contents, len);
  int saved_errno = errno;
  (void)close(fd);
  if (nw != (ssize_t)len) {
    errno = nw < 0 ? saved_errno : EIO;
    return -1;
  }
  return 0;
}

static int test_add_remove_and_close_manifest(void) {
  char root[] = "/tmp/agora_shm_state_test_XXXXXX";
  CHECK(mkdtemp(root) != NULL);

  AgoraShmManagerState state;
  CHECK(agora_shm_manager_state_open_at(&state, root) == 0);
  CHECK(agora_shm_manager_state_add(&state, "/state_one") == 0);
  CHECK(manifest_contains(state.dir_path, "/state_one") == 1);
  CHECK(agora_shm_manager_state_remove(&state, "/state_one") == 0);
  CHECK(manifest_contains(state.dir_path, "/state_one") == 0);

  char owned_dir[PATH_MAX];
  memcpy(owned_dir, state.dir_path, sizeof(owned_dir));
  agora_shm_manager_state_close(&state);
  CHECK(access(owned_dir, F_OK) == -1 && errno == ENOENT);
  CHECK(rmdir(root) == 0);
  return 0;
}

static int test_live_manager_is_not_recovered_in_same_process(void) {
  char root[] = "/tmp/agora_shm_state_test_XXXXXX";
  char shm_name[AGORA_SHM_IPC_SHM_NAME_BYTES];
  make_shm_name(shm_name, sizeof(shm_name), "live");
  CHECK(mkdtemp(root) != NULL);
  CHECK(create_test_shm(shm_name) == 0);

  AgoraShmManagerState first;
  AgoraShmManagerState second;
  CHECK(agora_shm_manager_state_open_at(&first, root) == 0);
  CHECK(agora_shm_manager_state_add(&first, shm_name) == 0);
  CHECK(agora_shm_manager_state_open_at(&second, root) == 0);
  CHECK(shm_exists(shm_name) == 1);

  agora_shm_manager_state_close(&second);
  CHECK(agora_shm_manager_state_remove(&first, shm_name) == 0);
  agora_shm_manager_state_close(&first);
  CHECK(agora_shm_ipc_unlink(shm_name) == 0);
  CHECK(rmdir(root) == 0);
  return 0;
}

static int test_dead_owner_is_recovered(void) {
  char root[] = "/tmp/agora_shm_state_test_XXXXXX";
  char shm_name[AGORA_SHM_IPC_SHM_NAME_BYTES];
  make_shm_name(shm_name, sizeof(shm_name), "stale");
  CHECK(mkdtemp(root) != NULL);
  CHECK(create_test_shm(shm_name) == 0);

  pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    AgoraShmManagerState abandoned;
    if (agora_shm_manager_state_open_at(&abandoned, root) != 0 ||
        agora_shm_manager_state_add(&abandoned, shm_name) != 0) {
      _exit(2);
    }
    _exit(0);
  }
  CHECK(wait_child_ok(child) == 0);

  AgoraShmManagerState recovery;
  CHECK(agora_shm_manager_state_open_at(&recovery, root) == 0);
  CHECK(shm_exists(shm_name) == 0);
  agora_shm_manager_state_close(&recovery);
  CHECK(rmdir(root) == 0);
  return 0;
}

static int test_malformed_manifest_fails_without_unlinking(void) {
  char root[] = "/tmp/agora_shm_state_test_XXXXXX";
  char unrelated[AGORA_SHM_IPC_SHM_NAME_BYTES];
  char bad_dir[PATH_MAX];
  char lock_path[PATH_MAX];
  char list_path[PATH_MAX];
  make_shm_name(unrelated, sizeof(unrelated), "unrelated");
  CHECK(mkdtemp(root) != NULL);
  CHECK(create_test_shm(unrelated) == 0);
  CHECK(snprintf(bad_dir, sizeof(bad_dir), "%s/manager-bad", root) > 0);
  CHECK(mkdir(bad_dir, 0700) == 0);
  CHECK(snprintf(lock_path, sizeof(lock_path), "%s/owner.lock", bad_dir) > 0);
  CHECK(snprintf(list_path, sizeof(list_path), "%s/shm.list", bad_dir) > 0);
  CHECK(write_file(lock_path, "") == 0);
  CHECK(write_file(list_path, "not-a-posix-shm-name\n") == 0);

  AgoraShmManagerState state;
  errno = 0;
  CHECK(agora_shm_manager_state_open_at(&state, root) == -1);
  CHECK(errno == EPROTO);
  CHECK(shm_exists(unrelated) == 1);
  CHECK(access(bad_dir, F_OK) == 0);

  CHECK(unlink(list_path) == 0);
  CHECK(unlink(lock_path) == 0);
  CHECK(rmdir(bad_dir) == 0);
  CHECK(rmdir(root) == 0);
  CHECK(agora_shm_ipc_unlink(unrelated) == 0);
  return 0;
}

int main(void) {
  CHECK(test_add_remove_and_close_manifest() == 0);
  CHECK(test_live_manager_is_not_recovered_in_same_process() == 0);
  CHECK(test_dead_owner_is_recovered() == 0);
  CHECK(test_malformed_manifest_fails_without_unlinking() == 0);
  puts("PASS test_agora_shm_manager_state");
  return 0;
}
