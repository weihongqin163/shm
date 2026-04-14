/**
 * author: Yan Zhennan
 * date: 2026-04-13
 */

#ifndef AGORA_SHM_IPC_H
#define AGORA_SHM_IPC_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/un.h>

typedef struct AgoraShmIpcHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t payload_size;
  uint32_t data_len;
  atomic_uint seq;
} AgoraShmIpcHeader;

typedef struct AgoraShmIpc {
  void *region_base;
  AgoraShmIpcHeader *header;
  uint8_t *payload;
  size_t map_size;
  size_t payload_size;
  int fd;
  int creator;
} AgoraShmIpc;

typedef struct AgoraShmIpcNotify {
  int fd;
  int is_writer;
  struct sockaddr_un peer;
  socklen_t peer_len;
  char bind_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
  int has_bind_path;
} AgoraShmIpcNotify;

/**
 * Opens POSIX shared memory.
 *
 * @param shm_name      POSIX shm name (e.g. "/agsh1"); must start with '/' on
 *                      most systems; keep short on macOS.
 * @param payload_size  User payload bytes; total object size is
 *                      sizeof(AgoraShmIpcHeader) + payload_size.
 * @param is_creator    Non-zero: create with O_CREAT|O_EXCL, ftruncate, init
 *                      header. Zero: attach; payload_size must match header.
 * @param out           Filled on success; zeroed by callee on entry.
 * @return 0 on success, -1 on error (errno set).
 */
int agora_shm_ipc_open(const char *shm_name, size_t payload_size, int is_creator,
                       AgoraShmIpc *out);

void agora_shm_ipc_close(AgoraShmIpc *ctx);

/** Removes the POSIX shared memory object (name). */
int agora_shm_ipc_unlink(const char *shm_name);

/**
 * Writer-side recovery: reset seqlock and logical length.
 * Call once per writer process before the first agora_shm_ipc_write.
 */
int agora_shm_ipc_writer_session_begin(AgoraShmIpc *ctx);

/**
 * Writes payload under seqlock. Never blocks the reader's scheduling of writes;
 * the reader may observe EAGAIN if a stable snapshot was not available.
 *
 * @param notify If non-NULL and initialized as writer, sends one byte to the
 *               reader socket after a successful SHM commit (best-effort).
 */
int agora_shm_ipc_write(AgoraShmIpc *ctx, const void *data, size_t len,
                        const AgoraShmIpcNotify *notify);

/**
 * Reads the latest stable payload into buf.
 *
 * @return 0 on success, *out_len set. -1 with errno EAGAIN if no complete
 *         frame yet or concurrent write; ENOBUFS if cap < data_len; EINVAL /
 *         EIO for format issues.
 */
int agora_shm_ipc_read(AgoraShmIpc *ctx, void *buf, size_t cap, size_t *out_len);

/** Writer: bind writer_bind_path; peer is reader_recv_path for sendto. */
int agora_shm_ipc_notify_writer_init(AgoraShmIpcNotify *n,
                                     const char *writer_bind_path,
                                     const char *reader_recv_path);

/** Reader: bind reader_recv_path; use returned fd with poll/recv. */
int agora_shm_ipc_notify_reader_init(AgoraShmIpcNotify *n,
                                     const char *reader_recv_path);

/** Socket fd for poll(2); valid after successful *_notify_*_init. */
int agora_shm_ipc_notify_fd(const AgoraShmIpcNotify *n);

/** Closes fd and unlinks the bound path if this side called bind. */
void agora_shm_ipc_notify_fini(AgoraShmIpcNotify *n);

#endif /* AGORA_SHM_IPC_H */
