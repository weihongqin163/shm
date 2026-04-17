/**
 * created by:wei
 * copyright (c) 2026 Agora IO. All rights reserved.
 * date: 2026-04-15
 */

#include "agora_shm_ipc.h"
#include "agora_shm_ipc_notify.h"

#include <errno.h>
#include <poll.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_onnotify_payload(const void *buf, size_t len) {
  if (buf == NULL || len < sizeof(AgoraShmIpcHeader)) {
    return;
  }
  const AgoraShmIpcHeader *h = (const AgoraShmIpcHeader *)buf;
  unsigned seqv =
      atomic_load_explicit(&h->seq, memory_order_relaxed);
  printf("[ONNOTIFY]%s,%s,%u, %u,%u\n", h->shm_name, h->user_id, seqv,
         (unsigned)h->media_type, (unsigned)h->stream_type);
}

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
      alignas(AgoraShmIpcHeader) unsigned char notify_buf[sizeof(AgoraShmIpcHeader)];
      ssize_t nr = recv(nfd, notify_buf, sizeof(notify_buf), 0);
      if (nr > 0) {
        print_onnotify_payload(notify_buf, (size_t)nr);
      } else if (nr < 0) {
        perror("recv");
      }
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
    AgoraShmIpcFrameMeta meta;
    memset(&meta, 0, sizeof(meta));
    if (agora_shm_ipc_read(&ctx, buf, payload_size, &out_len, &meta) != 0) {
      if (errno != EAGAIN) {
        perror("agora_shm_ipc_read");
      }
      continue;
    }

    printf("reader seq=%u, frame=%u shm=%s user_id=%.16s... media=%u stream=%u "
           "wxh=%dx%d audio %d/%d/%d\n",
           ctx.header->seq, frame, meta.shm_name, meta.user_id, meta.media_type,
           meta.stream_type, (int)meta.width, (int)meta.height,
           (int)meta.sample_rate, (int)meta.channels, (int)meta.bits);
    ++frame;
  }

  free(buf);
  agora_shm_ipc_close(&ctx);
  agora_shm_ipc_notify_fini(&notify);
  return 0;
}
