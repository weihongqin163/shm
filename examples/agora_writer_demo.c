/**
 * created by:wei
 * copyright (c) 2026 Agora IO. All rights reserved.
 * date: 2026-04-15
 */

#include "agora_shm_ipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void sleep_ms(int ms) {
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000);
  ts.tv_nsec = (long)((ms % 1000) * 1000000);
  (void)nanosleep(&ts, NULL);
}

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

  if (agora_shm_ipc_open(shm_name, payload_size, 1, &ctx) != 0) {
    if (errno == EEXIST) {
      if (agora_shm_ipc_open(shm_name, payload_size, 0, &ctx) != 0) {
        perror("agora_shm_ipc_open attach");
        (void)agora_shm_ipc_unlink(shm_name);
        return 1;
      }
    } else {
      perror("agora_shm_ipc_open create");
      return 1;
    }
  }

  if (agora_shm_ipc_writer_session_begin(&ctx) != 0) {
    perror("agora_shm_ipc_writer_session_begin");
    agora_shm_ipc_close(&ctx);
    return 1;
  }

  uint8_t *buf = (uint8_t *)malloc(payload_size);
  if (!buf) {
    fprintf(stderr, "malloc failed\n");
    agora_shm_ipc_close(&ctx);
    return 1;
  }

  unsigned seq = 0u;
  for (int i = 0; i < 1000000; ++i) {
    for (size_t j = 0u; j < payload_size; ++j) {
      buf[j] = (uint8_t)((seq + (unsigned)j) & 0xFFu);
    }
    size_t send_len = payload_size;
    if (send_len > 256u) {
      send_len = 256u;
    }

    AgoraShmIpcFrameMeta meta;
    memset(&meta, 0, sizeof(meta));
    (void)strncpy(meta.user_id, "demo-writer-user-000000000000000000000000000000",
                  sizeof(meta.user_id) - 1u);
    (void)strncpy(meta.shm_name, shm_name, sizeof(meta.shm_name) - 1u);
    meta.media_type = (uint32_t)AGORA_SHM_MEDIA_VIDEO;
    meta.stream_type = (uint32_t)AGORA_SHM_STREAM_MAIN;
    meta.width = 1280;
    meta.height = 720;
    meta.sample_rate = 48000;
    meta.channels = 2;
    meta.bits = 16;

    if (agora_shm_ipc_write(&ctx, buf, send_len, &meta) != 0) {
      perror("agora_shm_ipc_write");
      break;
    }
    printf("writer seq=%u, frame=%u\n", ctx.header->seq, seq);

    ++seq;

    sleep_ms(330);
  }

  free(buf);
  agora_shm_ipc_close(&ctx);
  return 0;
}
