/**
 * created by:wei
 * copyright (c) 2026 Agora IO. All rights reserved.
 * date: 2026-04-15
 */

#include "agora_shm_ipc.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  (void)setvbuf(stdout, NULL, _IOLBF, 0);

  const char *shm_name = "/agsh1";
  size_t payload_size = 4096u;

  if (argc >= 2) {
    shm_name = argv[1];
  }
  if (argc >= 3) {
    payload_size = (size_t)strtoul(argv[2], NULL, 10);
    if (payload_size == 0u) {
      fprintf(stderr, "payload_size must be > 0\n");
      return 1;
    }
  }

  AgoraShmIpc ctx;
  memset(&ctx, 0, sizeof(ctx));
  for (;;) {
    if (agora_shm_ipc_open(shm_name, payload_size, 0, &ctx) == 0) {
      break;
    }
    if (errno != ENOENT) {
      perror("agora_shm_ipc_open");
      return 1;
    }
    (void)usleep(20000);
  }

  uint8_t *buf = (uint8_t *)malloc(payload_size);
  if (!buf) {
    fprintf(stderr, "malloc failed\n");
    agora_shm_ipc_close(&ctx);
    return 1;
  }

  unsigned frame = 0u;
  unsigned last_seq = 0u;
  for (;;) {
    (void)usleep(2000);
    size_t out_len = 0u;
    AgoraShmIpcFrameMeta meta;
    memset(&meta, 0, sizeof(meta));
    if (agora_shm_ipc_read(&ctx, buf, payload_size, &out_len, &meta) != 0) {
      if (errno != EAGAIN) {
        perror("agora_shm_ipc_read");
      }
      continue;
    }
    unsigned cur =
        atomic_load_explicit(&ctx.header->seq, memory_order_relaxed);
    if (cur == last_seq) {
      continue;
    }
    last_seq = cur;

    printf("reader seq=%u, frame=%u shm=%s user_id=%.16s... media=%u stream=%u "
           "wxh=%dx%d audio %d/%d/%d\n",
           cur, frame, meta.shm_name, meta.user_id, meta.media_type,
           meta.stream_type, (int)meta.width, (int)meta.height,
           (int)meta.sample_rate, (int)meta.channels, (int)meta.bits);
    ++frame;
  }

  free(buf);
  agora_shm_ipc_close(&ctx);
  return 0;
}
