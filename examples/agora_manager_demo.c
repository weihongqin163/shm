/**
 * created by:wei
 * copyright (c) 2026 Agora IO. All rights reserved.
 * date: 2026-04-15
 */

#include "agora_shm_ipc.h"
#include "agora_shm_manager.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;

static void on_sigint(int signo) {
  (void)signo;
  g_stop = 1;
}

static void sleep_ms(int ms) {
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000);
  ts.tv_nsec = (long)((ms % 1000) * 1000000);
  (void)nanosleep(&ts, NULL);
}

static void on_frame(const char *shm_name, const void *payload, size_t len,
                     const AgoraShmIpcFrameMeta *meta, void *user) {
  (void)payload;
  (void)user;
  if (meta == NULL) {
    printf("[MANAGER] shm=%s len=%zu\n", shm_name, len);
    return;
  }
  printf("[MANAGER] shm=%s len=%zu user_id=%.16s... media=%u stream=%u "
         "wxh=%dx%d\n",
         shm_name, len, meta->user_id, meta->media_type, meta->stream_type,
         (int)meta->width, (int)meta->height);
}

int main(int argc, char **argv) {
  (void)setvbuf(stdout, NULL, _IOLBF, 0);

  if (argc != 6) {
    fprintf(stderr,
            "usage: %s <port> <server_mode 0|1> <max_clients> <keepalive_ms> "
            "<write_shm_name>\n"
            "  After start: agora_shm_manager_add then loop "
            "agora_shm_manager_write every 200ms.\n",
            argc > 0 ? argv[0] : "agora_manager_demo");
    return 1;
  }

  unsigned long port_ul = strtoul(argv[1], NULL, 10);
  if (port_ul == 0ul || port_ul > 65535ul) {
    fprintf(stderr, "invalid port\n");
    return 1;
  }
  uint16_t port = (uint16_t)port_ul;

  if (argv[2][0] == '\0' || argv[2][1] != '\0' ||
      (argv[2][0] != '0' && argv[2][0] != '1')) {
    fprintf(stderr, "server_mode must be 0 or 1\n");
    return 1;
  }
  bool server_mode = (argv[2][0] == '1');

  unsigned long max_cli = strtoul(argv[3], NULL, 10);
  if (max_cli == 0ul) {
    fprintf(stderr, "max_clients must be > 0 (used in server mode)\n");
    return 1;
  }

  unsigned long ka_ms = strtoul(argv[4], NULL, 10);
  if (ka_ms == 0ul) {
    fprintf(stderr, "keepalive_ms must be > 0 (used in server mode)\n");
    return 1;
  }

  const char *write_shm_name = argv[5];

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_sigint;
  if (sigaction(SIGINT, &sa, NULL) != 0) {
    perror("sigaction");
    return 1;
  }

  const size_t k_payload = 4096u;

  AgoraShmManager *mgr = NULL;
  if (agora_shm_manager_start(on_frame, port, server_mode, (size_t)max_cli,
                              (uint32_t)ka_ms, NULL, k_payload, &mgr) != 0) {
    perror("agora_shm_manager_start");
    return 1;
  }

  if (agora_shm_manager_add(mgr, write_shm_name, k_payload) != 0) {
    perror("agora_shm_manager_add");
    agora_shm_manager_close(mgr);
    return 1;
  }

  printf("agora_manager_demo port=%u server_mode=%d max_clients=%lu "
         "keepalive_ms=%lu write_shm=%s add+write 200ms (Ctrl+C exit)\n",
         (unsigned)port, server_mode ? 1 : 0, max_cli, ka_ms, write_shm_name);

  uint8_t *buf = (uint8_t *)malloc(k_payload);
  if (buf == NULL) {
    agora_shm_manager_close(mgr);
    return 1;
  }

  unsigned seq = 0u;
  while (g_stop == 0) {
    for (size_t j = 0u; j < k_payload; ++j) {
      buf[j] = (uint8_t)((seq + (unsigned)j) & 0xFFu);
    }
    size_t send_len = 256u;

    AgoraShmIpcFrameMeta meta;
    memset(&meta, 0, sizeof(meta));
    (void)snprintf(meta.user_id, sizeof(meta.user_id), "mgr-demo");
    (void)strncpy(meta.shm_name, write_shm_name, sizeof(meta.shm_name) - 1u);
    meta.media_type = (uint32_t)AGORA_SHM_MEDIA_VIDEO;
    meta.stream_type = (uint32_t)AGORA_SHM_STREAM_MAIN;
    meta.width = 640;
    meta.height = 480;
    meta.sample_rate = 48000;
    meta.channels = 2;
    meta.bits = 16;

    if (agora_shm_manager_write(mgr, write_shm_name, buf, send_len, &meta) !=
        0) {
      perror("agora_shm_manager_write");
      break;
    }
    ++seq;
    sleep_ms(200);
  }

  free(buf);
  agora_shm_manager_close(mgr);
  return 0;
}
