/**
 * created by:wei
 * copyright (c) 2026 Agora IO. All rights reserved.
 * date: 2026-04-15
 */

#include "agora_shm_manager.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;

static void on_sigint(int signo) {
  (void)signo;
  g_stop = 1;
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
  (void)argc;
  (void)setvbuf(stdout, NULL, _IOLBF, 0);

  const char *reader_sock = "/tmp/agora_reader.sock";
  size_t max_payload = 4096u;

  if (argc >= 2) {
    reader_sock = argv[1];
  }
  if (argc >= 3) {
    max_payload = (size_t)strtoul(argv[2], NULL, 10);
    if (max_payload == 0u) {
      fprintf(stderr, "max_payload must be > 0\n");
      return 1;
    }
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_sigint;
  if (sigaction(SIGINT, &sa, NULL) != 0) {
    perror("sigaction");
    return 1;
  }

  AgoraShmManager *mgr = NULL;
  if (agora_shm_manager_start(reader_sock, on_frame, NULL, max_payload, &mgr) !=
      0) {
    perror("agora_shm_manager_start");
    return 1;
  }

  printf("agora_manager_demo: listening on notify path \"%s\" "
         "(max_payload=%zu). Run agora_writer_demo with the same reader path. "
         "Ctrl+C to exit.\n",
         reader_sock, max_payload);

  while (g_stop == 0) {
    (void)pause();
  }

  agora_shm_manager_close(mgr);
  return 0;
}
