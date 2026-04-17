/**
 * created by:wei
 * copyright (c) 2026 Agora IO. All rights reserved.
 * date: 2026-04-15
 *
 * Minimal demo: server mode polls and prints peer count; client mode connects
 * and sends keep-alives from the main loop (application-driven timing).
 */

#include "agora_localsock.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr,
            "usage: %s server <port> <keepalive_ms> <max_clients>\n"
            "       %s client <port> <keepalive_ms> [seconds]\n",
            argv[0], argv[0]);
    return 1;
  }
  if (strcmp(argv[1], "server") == 0) {
    if (argc < 5) {
      fprintf(stderr, "usage: %s server <port> <keepalive_ms> <max_clients>\n", argv[0]);
      return 1;
    }
    uint16_t port = (uint16_t)atoi(argv[2]);
    uint32_t ka_ms = (uint32_t)atoi(argv[3]);
    size_t max_c = (size_t)atoi(argv[4]);
    agora_localsock_server *s = NULL;
    if (agora_localsock_server_create(port, ka_ms, max_c, &s) != 0) {
      perror("agora_localsock_server_create");
      return 1;
    }
    uint8_t srv_rx[65536];
    for (;;) {
      size_t srv_rx_len = 0u;
      if (agora_localsock_server_poll(s, 500, srv_rx, sizeof(srv_rx),
                                       &srv_rx_len) != 0) {
        perror("agora_localsock_server_poll");
        break;
      }
      printf("peers: %zu\n", agora_localsock_server_peer_count(s));
      fflush(stdout);
    }
    agora_localsock_server_destroy(s);
    return 0;
  }
  if (strcmp(argv[1], "client") == 0) {
    if (argc < 4) {
      fprintf(stderr, "usage: %s client <port> <keepalive_ms> [seconds]\n", argv[0]);
      return 1;
    }
    uint16_t port = (uint16_t)atoi(argv[2]);
    uint32_t ka_ms = (uint32_t)atoi(argv[3]);
    int run_s = argc >= 5 ? atoi(argv[4]) : 30;
    agora_localsock_client *c = NULL;
    if (agora_localsock_client_create(port, &c) != 0) {
      perror("agora_localsock_client_create");
      return 1;
    }
    struct timespec sleep_req;
    sleep_req.tv_sec = (time_t)(ka_ms / 1000u);
    sleep_req.tv_nsec = (long)((ka_ms % 1000u) * 1000000l);
    if (sleep_req.tv_sec == 0 && sleep_req.tv_nsec == 0) {
      sleep_req.tv_nsec = 1000000l;
    }
    if (agora_localsock_client_send_keepalive(c) != 0) {
      perror("agora_localsock_client_send_keepalive");
      agora_localsock_client_destroy(c);
      return 1;
    }
    if (run_s <= 0) {
      agora_localsock_client_destroy(c);
      return 0;
    }
    time_t end = time(NULL) + (time_t)run_s;
    while (time(NULL) < end) {
      for (;;) {
        int r = nanosleep(&sleep_req, &sleep_req);
        if (r == 0) {
          break;
        }
        if (errno != EINTR) {
          perror("nanosleep");
          goto client_done;
        }
      }
      if (agora_localsock_client_send_keepalive(c) != 0) {
        perror("agora_localsock_client_send_keepalive");
        break;
      }
    }
  client_done:
    agora_localsock_client_destroy(c);
    return 0;
  }
  fprintf(stderr, "unknown mode: %s\n", argv[1]);
  return 1;
}
