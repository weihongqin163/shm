/**
 * author: Yan Zhennan
 * date: 2026-04-13
 */

#include "agora_shm_ipc.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  (void)setvbuf(stdout, NULL, _IOLBF, 0);

  const char *shm_name = "/agsh1";
  const char *reader_sock = "/tmp/agora_reader.sock";
  size_t payload_size = 4096u;

  if (argc >= 2) {
    shm_name = argv[1];
  }
  if (argc >= 3) {
    reader_sock = argv[2];
  }
  if (argc >= 4) {
    payload_size = (size_t)strtoul(argv[3], NULL, 10);
    if (payload_size == 0u) {
      fprintf(stderr, "payload_size must be > 0\n");
      return 1;
    }
  }

  AgoraShmIpcNotify notify;
  memset(&notify, 0, sizeof(notify));
  if (agora_shm_ipc_notify_reader_init(&notify, reader_sock) != 0) {
    perror("agora_shm_ipc_notify_reader_init");
    return 1;
  }

  int nfd = agora_shm_ipc_notify_fd(&notify);
  if (nfd < 0) {
    perror("agora_shm_ipc_notify_fd");
    agora_shm_ipc_notify_fini(&notify);
    return 1;
  }

  AgoraShmIpc ctx;
  memset(&ctx, 0, sizeof(ctx));
  for (;;) {
    if (agora_shm_ipc_open(shm_name, payload_size, 0, &ctx) == 0) {
      break;
    }
    if (errno != ENOENT) {
      perror("agora_shm_ipc_open");
      agora_shm_ipc_notify_fini(&notify);
      return 1;
    }
    (void)usleep(20000);
  }

  uint8_t *buf = (uint8_t *)malloc(payload_size);
  if (!buf) {
    fprintf(stderr, "malloc failed\n");
    agora_shm_ipc_close(&ctx);
    agora_shm_ipc_notify_fini(&notify);
    return 1;
  }

  unsigned frame = 0u;
  for (;;) {
    struct pollfd pfd;
    pfd.fd = nfd;
    pfd.events = POLLIN;
    int pr = poll(&pfd, 1, 500);
    if (pr < 0) {
      perror("poll");
      break;
    }

    const int poll_in = (pr > 0 && (pfd.revents & POLLIN) != 0);
    if (poll_in) {
      char junk[8];
      (void)recv(nfd, junk, sizeof(junk), 0);
    }

    /* Read only when woken by notify or on poll timeout (missed datagram). */
    if (!poll_in && pr != 0) {
      continue;
    }
    /* for timed out,do not read */
    if (pr == 0) {
      continue;
    }

    size_t out_len = 0u;
    if (agora_shm_ipc_read(&ctx, buf, payload_size, &out_len) != 0) {
      if (errno != EAGAIN) {
        perror("agora_shm_ipc_read");
      }
      continue;
    }

    printf("reader seq=%u, frame=%u\n", ctx.header->seq, frame);
    ++frame;
  }

  free(buf);
  agora_shm_ipc_close(&ctx);
  agora_shm_ipc_notify_fini(&notify);
  return 0;
}
