/**
 * created by:wei
 * copyright (c) 2026 Agora IO. All rights reserved.
 * date: 2026-04-15
 */

#include "agora_shm_ipc.h"
#include "agora_shm_manager.h"

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>
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

/** Manager binds notify_write_bind + ".recv" (must fit sockaddr_un.sun_path). */
static int build_manager_recv_path(char *out, size_t out_cap,
                                   const char *notify_write_bind) {
  const char *suf = ".recv";
  size_t nb = strlen(notify_write_bind);
  size_t sl = strlen(suf);
  if (nb == 0u || nb + sl + 1u > out_cap) {
    errno = EINVAL;
    return -1;
  }
  if (nb + sl >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  (void)memcpy(out, notify_write_bind, nb);
  (void)memcpy(out + nb, suf, sl + 1u);
  return 0;
}

int main(int argc, char **argv) {
  (void)setvbuf(stdout, NULL, _IOLBF, 0);

  if (argc != 4) {
    fprintf(stderr,
            "usage: %s <notify_write_bind> <notify_peer_recv> <write_shm_name>\n"
            "  notify_write_bind : local path for writer notify bind\n"
            "  notify_peer_recv  : peer path for sendto after each write\n"
            "  write_shm_name    : POSIX shm for this process (ipc write)\n"
            "  manager listens on: <notify_write_bind>.recv\n",
            argc > 0 ? argv[0] : "agora_manager_demo");
    return 1;
  }

  const char *notify_write_bind = argv[1];
  const char *notify_peer_recv = argv[2];
  const char *write_shm_name = argv[3];

  char my_manager_recv[sizeof(((struct sockaddr_un *)0)->sun_path)];
  if (build_manager_recv_path(my_manager_recv, sizeof(my_manager_recv),
                              notify_write_bind) != 0) {
    perror("build_manager_recv_path");
    return 1;
  }

  const size_t k_payload = 4096u;

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_sigint;
  if (sigaction(SIGINT, &sa, NULL) != 0) {
    perror("sigaction");
    return 1;
  }

  AgoraShmIpc my_writer;
  memset(&my_writer, 0, sizeof(my_writer));
  if (agora_shm_ipc_open(write_shm_name, k_payload, 1, &my_writer) != 0) {
    if (errno == EEXIST) {
      if (agora_shm_ipc_open(write_shm_name, k_payload, 0, &my_writer) != 0) {
        perror("agora_shm_ipc_open write_shm attach");
        return 1;
      }
    } else {
      perror("agora_shm_ipc_open write_shm create");
      return 1;
    }
  }
  if (agora_shm_ipc_writer_session_begin(&my_writer) != 0) {
    perror("agora_shm_ipc_writer_session_begin");
    agora_shm_ipc_close(&my_writer);
    return 1;
  }

  AgoraShmManager *mgr = NULL;
  if (agora_shm_manager_start(my_manager_recv, on_frame, NULL, k_payload, &mgr) !=
      0) {
    perror("agora_shm_manager_start");
    agora_shm_ipc_close(&my_writer);
    return 1;
  }

  AgoraShmIpcNotify wnotify;
  memset(&wnotify, 0, sizeof(wnotify));
  if (agora_shm_ipc_notify_writer_init(&wnotify, notify_write_bind,
                                       notify_peer_recv) != 0) {
    perror("agora_shm_ipc_notify_writer_init");
    agora_shm_manager_close(mgr);
    agora_shm_ipc_close(&my_writer);
    return 1;
  }

  printf("agora_manager_demo writer_bind=%s sendto=%s write_shm=%s "
         "manager_recv=%s (Ctrl+C exit)\n",
         notify_write_bind, notify_peer_recv, write_shm_name, my_manager_recv);

  uint8_t *buf = (uint8_t *)malloc(k_payload);
  if (buf == NULL) {
    agora_shm_ipc_notify_fini(&wnotify);
    agora_shm_manager_close(mgr);
    agora_shm_ipc_close(&my_writer);
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

    if (agora_shm_ipc_write(&my_writer, buf, send_len, &meta, &wnotify) != 0) {
      perror("agora_shm_ipc_write");
      break;
    }
    ++seq;
    sleep_ms(330);
  }

  free(buf);
  agora_shm_ipc_notify_fini(&wnotify);
  agora_shm_manager_close(mgr);
  agora_shm_ipc_close(&my_writer);
  return 0;
}
