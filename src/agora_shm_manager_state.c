#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "agora_shm_manager_state.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define AGORA_STATE_CREATING_PREFIX ".creating-"
#define AGORA_STATE_MANAGER_PREFIX "manager-"
#define AGORA_STATE_LOCK_FILE "owner.lock"
#define AGORA_STATE_MANIFEST_FILE "shm.list"
#define AGORA_STATE_MANIFEST_TMP_FILE "shm.list.tmp"

static void state_reset(AgoraShmManagerState *state) {
  memset(state, 0, sizeof(*state));
  state->lock_fd = -1;
}

static int validate_shm_name(const char *shm_name) {
  if (shm_name == NULL || shm_name[0] != '/') {
    errno = EINVAL;
    return -1;
  }
  size_t len = strnlen(shm_name, AGORA_SHM_IPC_SHM_NAME_BYTES);
  if (len == 0u || len >= AGORA_SHM_IPC_SHM_NAME_BYTES ||
      strchr(shm_name, '\n') != NULL || strchr(shm_name, '\r') != NULL) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}

static int write_all(int fd, const void *data, size_t len) {
  const unsigned char *p = (const unsigned char *)data;
  while (len > 0u) {
    ssize_t nw = write(fd, p, len);
    if (nw < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (nw == 0) {
      errno = EIO;
      return -1;
    }
    p += (size_t)nw;
    len -= (size_t)nw;
  }
  return 0;
}

static int read_all(int fd, void *data, size_t len) {
  unsigned char *p = (unsigned char *)data;
  while (len > 0u) {
    ssize_t nr = read(fd, p, len);
    if (nr < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (nr == 0) {
      errno = EIO;
      return -1;
    }
    p += (size_t)nr;
    len -= (size_t)nr;
  }
  return 0;
}

static int open_dir_nofollow(const char *path) {
  return open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}

static int persist_manifest(AgoraShmManagerState *state) {
  int dir_fd = open_dir_nofollow(state->dir_path);
  if (dir_fd < 0) {
    return -1;
  }

  (void)unlinkat(dir_fd, AGORA_STATE_MANIFEST_TMP_FILE, 0);
  int fd = openat(dir_fd, AGORA_STATE_MANIFEST_TMP_FILE,
                  O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    int saved_errno = errno;
    (void)close(dir_fd);
    errno = saved_errno;
    return -1;
  }

  int rc = 0;
  for (size_t i = 0u; i < state->name_count; ++i) {
    size_t len = strlen(state->names[i]);
    if (write_all(fd, state->names[i], len) != 0 ||
        write_all(fd, "\n", 1u) != 0) {
      rc = -1;
      break;
    }
  }
  if (rc == 0 && fsync(fd) != 0) {
    rc = -1;
  }
  int saved_errno = errno;
  if (close(fd) != 0 && rc == 0) {
    rc = -1;
    saved_errno = errno;
  }
  if (rc == 0 && renameat(dir_fd, AGORA_STATE_MANIFEST_TMP_FILE, dir_fd,
                          AGORA_STATE_MANIFEST_FILE) != 0) {
    rc = -1;
    saved_errno = errno;
  }
  if (rc == 0 && fsync(dir_fd) != 0) {
    rc = -1;
    saved_errno = errno;
  }
  if (rc != 0) {
    (void)unlinkat(dir_fd, AGORA_STATE_MANIFEST_TMP_FILE, 0);
  }
  (void)close(dir_fd);
  if (rc != 0) {
    errno = saved_errno;
  }
  return rc;
}

static int parse_manifest(
    int dir_fd,
    char names[AGORA_SHM_MANAGER_STATE_MAX_NAMES][AGORA_SHM_IPC_SHM_NAME_BYTES],
    size_t *name_count) {
  int fd = openat(dir_fd, AGORA_STATE_MANIFEST_FILE,
                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return -1;
  }
  struct stat st;
  if (fstat(fd, &st) != 0) {
    int saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;
    return -1;
  }
  const size_t max_size =
      AGORA_SHM_MANAGER_STATE_MAX_NAMES * AGORA_SHM_IPC_SHM_NAME_BYTES;
  if (!S_ISREG(st.st_mode) || st.st_size < 0 ||
      (uintmax_t)st.st_size > (uintmax_t)max_size) {
    (void)close(fd);
    errno = EPROTO;
    return -1;
  }

  size_t size = (size_t)st.st_size;
  char *data = (char *)malloc(size + 1u);
  if (data == NULL) {
    int saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;
    return -1;
  }
  if (size > 0u && read_all(fd, data, size) != 0) {
    int saved_errno = errno;
    free(data);
    (void)close(fd);
    errno = saved_errno;
    return -1;
  }
  (void)close(fd);
  data[size] = '\0';
  if (size > 0u && data[size - 1u] != '\n') {
    free(data);
    errno = EPROTO;
    return -1;
  }

  size_t count = 0u;
  size_t begin = 0u;
  for (size_t i = 0u; i < size; ++i) {
    if (data[i] != '\n') {
      continue;
    }
    size_t len = i - begin;
    if (count >= AGORA_SHM_MANAGER_STATE_MAX_NAMES || len == 0u ||
        len >= AGORA_SHM_IPC_SHM_NAME_BYTES || data[begin] != '/' ||
        memchr(data + begin, '\0', len) != NULL ||
        memchr(data + begin, '\r', len) != NULL) {
      free(data);
      errno = EPROTO;
      return -1;
    }
    memcpy(names[count], data + begin, len);
    names[count][len] = '\0';
    ++count;
    begin = i + 1u;
  }
  free(data);
  *name_count = count;
  return 0;
}

static int recover_published_dir(int root_fd, const char *entry_name) {
  int dir_fd = openat(root_fd, entry_name,
                      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (dir_fd < 0) {
    return -1;
  }
  int lock_fd =
      openat(dir_fd, AGORA_STATE_LOCK_FILE, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  if (lock_fd < 0) {
    int saved_errno = errno;
    (void)close(dir_fd);
    errno = saved_errno;
    return -1;
  }
  if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    int saved_errno = errno;
    (void)close(lock_fd);
    (void)close(dir_fd);
    if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN) {
      return 0;
    }
    errno = saved_errno;
    return -1;
  }

  char names[AGORA_SHM_MANAGER_STATE_MAX_NAMES][AGORA_SHM_IPC_SHM_NAME_BYTES];
  size_t name_count = 0u;
  if (parse_manifest(dir_fd, names, &name_count) != 0) {
    int saved_errno = errno;
    (void)close(lock_fd);
    (void)close(dir_fd);
    errno = saved_errno;
    return -1;
  }
  for (size_t i = 0u; i < name_count; ++i) {
    if (agora_shm_ipc_unlink(names[i]) != 0 && errno != ENOENT) {
      int saved_errno = errno;
      (void)close(lock_fd);
      (void)close(dir_fd);
      errno = saved_errno;
      return -1;
    }
  }

  if (unlinkat(dir_fd, AGORA_STATE_MANIFEST_TMP_FILE, 0) != 0 &&
      errno != ENOENT) {
    int saved_errno = errno;
    (void)close(lock_fd);
    (void)close(dir_fd);
    errno = saved_errno;
    return -1;
  }
  if (unlinkat(dir_fd, AGORA_STATE_MANIFEST_FILE, 0) != 0 ||
      unlinkat(dir_fd, AGORA_STATE_LOCK_FILE, 0) != 0) {
    int saved_errno = errno;
    (void)close(lock_fd);
    (void)close(dir_fd);
    errno = saved_errno;
    return -1;
  }
  (void)close(dir_fd);
  if (unlinkat(root_fd, entry_name, AT_REMOVEDIR) != 0) {
    int saved_errno = errno;
    (void)close(lock_fd);
    errno = saved_errno;
    return -1;
  }
  (void)close(lock_fd);
  return 0;
}

static int recover_all(int root_fd) {
  int scan_fd = dup(root_fd);
  if (scan_fd < 0) {
    return -1;
  }
  DIR *dir = fdopendir(scan_fd);
  if (dir == NULL) {
    int saved_errno = errno;
    (void)close(scan_fd);
    errno = saved_errno;
    return -1;
  }
  const size_t prefix_len = strlen(AGORA_STATE_MANAGER_PREFIX);
  int rc = 0;
  struct dirent *entry;
  errno = 0;
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, AGORA_STATE_MANAGER_PREFIX, prefix_len) != 0) {
      continue;
    }
    if (recover_published_dir(root_fd, entry->d_name) != 0) {
      if (errno == ENOENT || errno == ENOTDIR) {
        errno = 0;
        continue;
      }
      rc = -1;
      break;
    }
    errno = 0;
  }
  int saved_errno = errno;
  if (rc == 0 && saved_errno != 0) {
    rc = -1;
  }
  (void)closedir(dir);
  if (rc != 0) {
    errno = saved_errno;
  }
  return rc;
}

static int ensure_root(const char *root) {
  if (mkdir(root, 0700) != 0 && errno != EEXIST) {
    return -1;
  }
  struct stat st;
  if (lstat(root, &st) != 0) {
    return -1;
  }
  if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
    errno = ENOTDIR;
    return -1;
  }
  return 0;
}

int agora_shm_manager_state_open_at(AgoraShmManagerState *state,
                                    const char *root) {
  if (state == NULL || root == NULL || root[0] == '\0') {
    errno = EINVAL;
    return -1;
  }
  state_reset(state);
  if (ensure_root(root) != 0) {
    return -1;
  }
  int root_fd = open_dir_nofollow(root);
  if (root_fd < 0) {
    return -1;
  }
  if (recover_all(root_fd) != 0) {
    int saved_errno = errno;
    (void)close(root_fd);
    errno = saved_errno;
    return -1;
  }

  char creating_path[PATH_MAX];
  int n = snprintf(creating_path, sizeof(creating_path), "%s/%sXXXXXX", root,
                   AGORA_STATE_CREATING_PREFIX);
  if (n < 0 || (size_t)n >= sizeof(creating_path)) {
    (void)close(root_fd);
    errno = ENAMETOOLONG;
    return -1;
  }
  if (mkdtemp(creating_path) == NULL) {
    int saved_errno = errno;
    (void)close(root_fd);
    errno = saved_errno;
    return -1;
  }

  int creating_fd = open_dir_nofollow(creating_path);
  if (creating_fd < 0) {
    int saved_errno = errno;
    (void)rmdir(creating_path);
    (void)close(root_fd);
    errno = saved_errno;
    return -1;
  }
  int lock_fd =
      openat(creating_fd, AGORA_STATE_LOCK_FILE,
             O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    int saved_errno = errno;
    if (lock_fd >= 0) {
      (void)close(lock_fd);
      (void)unlinkat(creating_fd, AGORA_STATE_LOCK_FILE, 0);
    }
    (void)close(creating_fd);
    (void)rmdir(creating_path);
    (void)close(root_fd);
    errno = saved_errno;
    return -1;
  }

  state->lock_fd = lock_fd;
  (void)strncpy(state->dir_path, creating_path, sizeof(state->dir_path) - 1u);
  if (persist_manifest(state) != 0) {
    int saved_errno = errno;
    agora_shm_manager_state_close(state);
    (void)close(creating_fd);
    (void)close(root_fd);
    errno = saved_errno;
    return -1;
  }

  const char *base = strrchr(creating_path, '/');
  base = base == NULL ? creating_path : base + 1;
  const char *suffix = base + strlen(AGORA_STATE_CREATING_PREFIX);
  char published_path[PATH_MAX];
  n = snprintf(published_path, sizeof(published_path), "%s/%s%s", root,
               AGORA_STATE_MANAGER_PREFIX, suffix);
  if (n < 0 || (size_t)n >= sizeof(published_path) ||
      rename(creating_path, published_path) != 0) {
    int saved_errno =
        n < 0 || (size_t)n >= sizeof(published_path) ? ENAMETOOLONG : errno;
    agora_shm_manager_state_close(state);
    (void)close(creating_fd);
    (void)close(root_fd);
    errno = saved_errno;
    return -1;
  }
  (void)strncpy(state->dir_path, published_path, sizeof(state->dir_path) - 1u);
  if (fsync(root_fd) != 0) {
    int saved_errno = errno;
    agora_shm_manager_state_close(state);
    (void)close(creating_fd);
    (void)close(root_fd);
    errno = saved_errno;
    return -1;
  }
  (void)close(creating_fd);
  (void)close(root_fd);
  return 0;
}

int agora_shm_manager_state_open(AgoraShmManagerState *state) {
  return agora_shm_manager_state_open_at(state, AGORA_SHM_MANAGER_STATE_ROOT);
}

int agora_shm_manager_state_add(AgoraShmManagerState *state,
                                const char *shm_name) {
  if (state == NULL || state->lock_fd < 0 || validate_shm_name(shm_name) != 0) {
    if (state == NULL || state->lock_fd < 0) {
      errno = EINVAL;
    }
    return -1;
  }
  for (size_t i = 0u; i < state->name_count; ++i) {
    if (strcmp(state->names[i], shm_name) == 0) {
      errno = EEXIST;
      return -1;
    }
  }
  if (state->name_count >= AGORA_SHM_MANAGER_STATE_MAX_NAMES) {
    errno = ENOSPC;
    return -1;
  }
  size_t index = state->name_count;
  (void)strncpy(state->names[index], shm_name,
                sizeof(state->names[index]) - 1u);
  ++state->name_count;
  if (persist_manifest(state) != 0) {
    int saved_errno = errno;
    --state->name_count;
    memset(state->names[index], 0, sizeof(state->names[index]));
    errno = saved_errno;
    return -1;
  }
  return 0;
}

int agora_shm_manager_state_remove(AgoraShmManagerState *state,
                                   const char *shm_name) {
  if (state == NULL || state->lock_fd < 0 || validate_shm_name(shm_name) != 0) {
    if (state == NULL || state->lock_fd < 0) {
      errno = EINVAL;
    }
    return -1;
  }
  size_t index = state->name_count;
  for (size_t i = 0u; i < state->name_count; ++i) {
    if (strcmp(state->names[i], shm_name) == 0) {
      index = i;
      break;
    }
  }
  if (index == state->name_count) {
    errno = ENOENT;
    return -1;
  }

  char old_names[AGORA_SHM_MANAGER_STATE_MAX_NAMES]
                [AGORA_SHM_IPC_SHM_NAME_BYTES];
  size_t old_count = state->name_count;
  memcpy(old_names, state->names, sizeof(old_names));
  for (size_t i = index + 1u; i < state->name_count; ++i) {
    memcpy(state->names[i - 1u], state->names[i], sizeof(state->names[i]));
  }
  --state->name_count;
  memset(state->names[state->name_count], 0,
         sizeof(state->names[state->name_count]));
  if (persist_manifest(state) != 0) {
    int saved_errno = errno;
    memcpy(state->names, old_names, sizeof(old_names));
    state->name_count = old_count;
    errno = saved_errno;
    return -1;
  }
  return 0;
}

void agora_shm_manager_state_abandon(AgoraShmManagerState *state) {
  if (state == NULL) {
    return;
  }
  if (state->lock_fd >= 0) {
    (void)close(state->lock_fd);
  }
  state_reset(state);
}

void agora_shm_manager_state_close(AgoraShmManagerState *state) {
  if (state == NULL) {
    return;
  }
  if (state->dir_path[0] != '\0') {
    int dir_fd = open_dir_nofollow(state->dir_path);
    if (dir_fd >= 0) {
      (void)unlinkat(dir_fd, AGORA_STATE_MANIFEST_TMP_FILE, 0);
      (void)unlinkat(dir_fd, AGORA_STATE_MANIFEST_FILE, 0);
      (void)unlinkat(dir_fd, AGORA_STATE_LOCK_FILE, 0);
      (void)close(dir_fd);
    }
    (void)rmdir(state->dir_path);
  }
  agora_shm_manager_state_abandon(state);
}
