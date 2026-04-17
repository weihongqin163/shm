/**
 * created by:wei
 * copyright (c) 2026 Agora IO. All rights reserved.
 * date: 2026-04-15
 */

#include "agora_localsock.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct agora_localsock_slot {
  struct sockaddr_in peer;
  uint64_t last_seen_ns;
} agora_localsock_slot;

struct agora_localsock_server {
  int fd;
  uint32_t keepalive_interval_ms;
  uint64_t timeout_ns;
  size_t max_clients;
  size_t n_clients;
  agora_localsock_slot *slots;
  pthread_mutex_t io_mu;
};

struct agora_localsock_client {
  int fd;
  pthread_mutex_t mu;
};

typedef enum {
  AGORA_LS_PARSE_INVALID = 0,
  AGORA_LS_PARSE_KEEPALIVE,
  AGORA_LS_PARSE_APP,
  AGORA_LS_PARSE_WRITECMD,
  AGORA_LS_PARSE_UNKNOWN_OK,
} agora_localsock_parse_kind;

static int mono_now_ns(uint64_t *out) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return -1;
  }
  *out = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  return 0;
}

static int sin_peer_eq(const struct sockaddr_in *a, const struct sockaddr_in *b) {
  return a->sin_family == b->sin_family && a->sin_port == b->sin_port &&
         a->sin_addr.s_addr == b->sin_addr.s_addr;
}

static ssize_t find_peer(agora_localsock_server *s, const struct sockaddr_in *peer) {
  for (size_t i = 0; i < s->n_clients; i++) {
    if (sin_peer_eq(&s->slots[i].peer, peer)) {
      return (ssize_t)i;
    }
  }
  return -1;
}

static agora_localsock_parse_kind parse_datagram(const uint8_t *buf, ssize_t nread,
                                                 uint16_t *out_msg_type,
                                                 uint32_t *out_payload_len) {
  if (nread < (ssize_t)AGORA_LOCALSOCK_HEADER_BYTES) {
    return AGORA_LS_PARSE_INVALID;
  }
  agora_localsock_header h;
  memcpy(&h, buf, sizeof(h));
  if (h.magic != AGORA_LOCALSOCK_MAGIC || h.ver != (uint16_t)AGORA_LOCALSOCK_VER) {
    return AGORA_LS_PARSE_INVALID;
  }
  uint16_t msg_type = h.msg_type;
  uint32_t payload_len = h.payload_len;
  size_t total = (size_t)AGORA_LOCALSOCK_HEADER_BYTES + (size_t)payload_len;
  if (total > (size_t)SSIZE_MAX || (ssize_t)total != nread) {
    return AGORA_LS_PARSE_INVALID;
  }
  *out_msg_type = msg_type;
  *out_payload_len = payload_len;
  if (msg_type == AGORA_LOCALSOCK_MSG_KEEPALIVE) {
    if (payload_len != 0u) {
      return AGORA_LS_PARSE_INVALID;
    }
    return AGORA_LS_PARSE_KEEPALIVE;
  }
  if (msg_type == AGORA_LOCALSOCK_MSG_APP) {
    return AGORA_LS_PARSE_APP;
  }
  if (msg_type == AGORA_LOCALSOCK_MSG_WRITECMD) {
    return AGORA_LS_PARSE_WRITECMD;
  }
  return AGORA_LS_PARSE_UNKNOWN_OK;
}

static void evict_stale(agora_localsock_server *s, uint64_t now_ns) {
  size_t i = 0;
  while (i < s->n_clients) {
    uint64_t idle = now_ns - s->slots[i].last_seen_ns;
    if (idle > s->timeout_ns) {
      s->slots[i] = s->slots[s->n_clients - 1u];
      s->n_clients--;
    } else {
      i++;
    }
  }
}

static int apply_datagram(agora_localsock_server *s, const uint8_t *buf, ssize_t nread,
                          const struct sockaddr_in *peer, uint64_t now_ns) {
  uint16_t msg_type = 0;
  uint32_t payload_len = 0;
  agora_localsock_parse_kind k = parse_datagram(buf, nread, &msg_type, &payload_len);
  (void)msg_type;
  (void)payload_len;
  if (k == AGORA_LS_PARSE_INVALID) {
    return 0;
  }
  ssize_t idx = find_peer(s, peer);
  if (k == AGORA_LS_PARSE_KEEPALIVE || k == AGORA_LS_PARSE_APP ||
      k == AGORA_LS_PARSE_WRITECMD) {
    if (idx >= 0) {
      s->slots[(size_t)idx].last_seen_ns = now_ns;
      return 0;
    }
    if (s->n_clients >= s->max_clients) {
      return 0;
    }
    s->slots[s->n_clients].peer = *peer;
    s->slots[s->n_clients].last_seen_ns = now_ns;
    s->n_clients++;
    return 0;
  }
  /* Unknown msg_type: refresh only if already registered. */
  if (idx >= 0) {
    s->slots[(size_t)idx].last_seen_ns = now_ns;
  }
  return 0;
}

int agora_localsock_server_create(uint16_t port, uint32_t keepalive_interval_ms,
                                  size_t max_clients, agora_localsock_server **out) {
  if (out == NULL || max_clients == 0u || keepalive_interval_ms == 0u ||
      port == 0u) {
    errno = EINVAL;
    return -1;
  }
  agora_localsock_server *s =
      (agora_localsock_server *)calloc(1, sizeof(agora_localsock_server));
  if (s == NULL) {
    return -1;
  }
  if (pthread_mutex_init(&s->io_mu, NULL) != 0) {
    free(s);
    return -1;
  }
  s->slots =
      (agora_localsock_slot *)calloc(max_clients, sizeof(agora_localsock_slot));
  if (s->slots == NULL) {
    pthread_mutex_destroy(&s->io_mu);
    free(s);
    return -1;
  }
  s->max_clients = max_clients;
  s->keepalive_interval_ms = keepalive_interval_ms;
  s->timeout_ns = (uint64_t)5 * (uint64_t)keepalive_interval_ms * 1000000ull;
  s->fd = -1;

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    free(s->slots);
    pthread_mutex_destroy(&s->io_mu);
    free(s);
    return -1;
  }
  int yes = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
    int e = errno;
    close(fd);
    free(s->slots);
    pthread_mutex_destroy(&s->io_mu);
    free(s);
    errno = e;
    return -1;
  }
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    int e = errno;
    close(fd);
    free(s->slots);
    pthread_mutex_destroy(&s->io_mu);
    free(s);
    errno = e;
    return -1;
  }
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
    close(fd);
    free(s->slots);
    pthread_mutex_destroy(&s->io_mu);
    free(s);
    errno = EINVAL;
    return -1;
  }
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    int e = errno;
    close(fd);
    free(s->slots);
    pthread_mutex_destroy(&s->io_mu);
    free(s);
    errno = e;
    return -1;
  }
  s->fd = fd;
  *out = s;
  return 0;
}

void agora_localsock_server_destroy(agora_localsock_server *s) {
  if (s == NULL) {
    return;
  }
  if (s->fd >= 0) {
    close(s->fd);
  }
  free(s->slots);
  pthread_mutex_destroy(&s->io_mu);
  free(s);
}

size_t agora_localsock_server_peer_count(const agora_localsock_server *s) {
  return s == NULL ? 0u : s->n_clients;
}

int agora_localsock_server_send_datagram(agora_localsock_server *s,
                                         uint16_t msg_type, const void *payload,
                                         size_t payload_len) {
  if (s == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (payload_len > 0u && payload == NULL) {
    errno = EINVAL;
    return -1;
  }
  size_t total = (size_t)AGORA_LOCALSOCK_HEADER_BYTES + payload_len;
  if (total > (size_t)AGORA_LOCALSOCK_MAX_DATAGRAM_BYTES) {
    errno = EMSGSIZE;
    return -1;
  }
  uint8_t *buf = (uint8_t *)malloc(total);
  if (buf == NULL) {
    return -1;
  }
  agora_localsock_header h;
  h.magic = AGORA_LOCALSOCK_MAGIC;
  h.ver = (uint16_t)AGORA_LOCALSOCK_VER;
  h.msg_type = msg_type;
  h.payload_len = (uint32_t)payload_len;
  memcpy(buf, &h, sizeof(h));
  if (payload_len > 0u) {
    memcpy(buf + AGORA_LOCALSOCK_HEADER_BYTES, payload, payload_len);
  }

  pthread_mutex_lock(&s->io_mu);
  if (s->n_clients == 0u) {
    pthread_mutex_unlock(&s->io_mu);
    free(buf);
    return 0;
  }
  for (size_t i = 0; i < s->n_clients; i++) {
    ssize_t nw =
        sendto(s->fd, buf, total, 0, (struct sockaddr *)&s->slots[i].peer,
               (socklen_t)sizeof(s->slots[i].peer));
    if (nw != (ssize_t)total) {
      int e = errno;
      pthread_mutex_unlock(&s->io_mu);
      free(buf);
      errno = (nw < 0) ? e : EIO;
      return -1;
    }
  }
  pthread_mutex_unlock(&s->io_mu);
  free(buf);
  return 0;
}

int agora_localsock_server_poll(agora_localsock_server *s, int timeout_ms,
                                  void *buf, size_t cap, size_t *out_len) {
  if (s == NULL || buf == NULL || out_len == NULL || cap == 0u) {
    errno = EINVAL;
    return -1;
  }
  *out_len = 0u;
  struct pollfd pfd;
  pfd.fd = s->fd;
  pfd.events = POLLIN;
  pfd.revents = 0;
  int pr = poll(&pfd, 1u, timeout_ms);
  if (pr < 0) {
    return -1;
  }
  uint64_t now_ns = 0;
  if (mono_now_ns(&now_ns) != 0) {
    return -1;
  }
  if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
    errno = EIO;
    return -1;
  }
  if ((pfd.revents & POLLIN) != 0) {
    uint8_t *recv_buf = (uint8_t *)buf;
    for (;;) {
      struct sockaddr_in peer;
      socklen_t plen = sizeof(peer);
      ssize_t nr = 0;
      pthread_mutex_lock(&s->io_mu);
      nr = recvfrom(s->fd, recv_buf, cap, 0, (struct sockaddr *)&peer, &plen);
      pthread_mutex_unlock(&s->io_mu);
      if (nr < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        return -1;
      }
      if (plen != sizeof(peer) || peer.sin_family != AF_INET) {
        continue;
      }
      *out_len = (size_t)nr;
      pthread_mutex_lock(&s->io_mu);
      (void)apply_datagram(s, recv_buf, nr, &peer, now_ns);
      pthread_mutex_unlock(&s->io_mu);
      if (mono_now_ns(&now_ns) != 0) {
        return -1;
      }
      break;
    }
  }
  if (mono_now_ns(&now_ns) != 0) {
    return -1;
  }
  pthread_mutex_lock(&s->io_mu);
  evict_stale(s, now_ns);
  pthread_mutex_unlock(&s->io_mu);
  return 0;
}

static int pack_keepalive(uint8_t *out12) {
  agora_localsock_header h;
  h.magic = AGORA_LOCALSOCK_MAGIC;
  h.ver = (uint16_t)AGORA_LOCALSOCK_VER;
  h.msg_type = (uint16_t)AGORA_LOCALSOCK_MSG_KEEPALIVE;
  h.payload_len = 0u;
  memcpy(out12, &h, sizeof(h));
  return 0;
}

int agora_localsock_client_create(uint16_t server_port,
                                  agora_localsock_client **out) {
  if (out == NULL || server_port == 0u) {
    errno = EINVAL;
    return -1;
  }
  agora_localsock_client *c =
      (agora_localsock_client *)calloc(1, sizeof(agora_localsock_client));
  if (c == NULL) {
    return -1;
  }
  c->fd = -1;
  if (pthread_mutex_init(&c->mu, NULL) != 0) {
    free(c);
    return -1;
  }
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    int e = errno;
    pthread_mutex_destroy(&c->mu);
    free(c);
    errno = e;
    return -1;
  }
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(server_port);
  if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
    close(fd);
    pthread_mutex_destroy(&c->mu);
    free(c);
    errno = EINVAL;
    return -1;
  }
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    int e = errno;
    close(fd);
    pthread_mutex_destroy(&c->mu);
    free(c);
    errno = e;
    return -1;
  }
  c->fd = fd;
  *out = c;
  return 0;
}

void agora_localsock_client_destroy(agora_localsock_client *c) {
  if (c == NULL) {
    return;
  }
  if (c->fd >= 0) {
    close(c->fd);
    c->fd = -1;
  }
  pthread_mutex_destroy(&c->mu);
  free(c);
}

int agora_localsock_client_poll(agora_localsock_client *c, int timeout_ms,
                                void *buf, size_t cap, size_t *out_len) {
  if (c == NULL || buf == NULL || out_len == NULL || cap == 0u) {
    errno = EINVAL;
    return -1;
  }
  pthread_mutex_lock(&c->mu);
  struct pollfd pfd;
  pfd.fd = c->fd;
  pfd.events = POLLIN;
  pfd.revents = 0;
  int pr = poll(&pfd, 1u, timeout_ms);
  if (pr < 0) {
    int e = errno;
    pthread_mutex_unlock(&c->mu);
    errno = e;
    return -1;
  }
  if (pr == 0) {
    pthread_mutex_unlock(&c->mu);
    errno = EAGAIN;
    return -1;
  }
  if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
    pthread_mutex_unlock(&c->mu);
    errno = EIO;
    return -1;
  }
  ssize_t nr = recv(c->fd, buf, cap, 0);
  if (nr < 0) {
    int e = errno;
    pthread_mutex_unlock(&c->mu);
    errno = e;
    return -1;
  }
  *out_len = (size_t)nr;
  pthread_mutex_unlock(&c->mu);
  return 0;
}

int agora_localsock_client_send_keepalive(agora_localsock_client *c) {
  if (c == NULL) {
    errno = EINVAL;
    return -1;
  }
  uint8_t pkt[AGORA_LOCALSOCK_HEADER_BYTES];
  (void)pack_keepalive(pkt);
  pthread_mutex_lock(&c->mu);
  ssize_t nw = send(c->fd, pkt, sizeof(pkt), 0);
  pthread_mutex_unlock(&c->mu);
  if (nw != (ssize_t)sizeof(pkt)) {
    if (nw < 0) {
      return -1;
    }
    errno = EIO;
    return -1;
  }
  return 0;
}

int agora_localsock_client_send_datagram(agora_localsock_client *c,
                                         uint16_t msg_type, const void *payload,
                                         size_t payload_len) {
  if (c == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (payload_len > 0u && payload == NULL) {
    errno = EINVAL;
    return -1;
  }
  size_t total = (size_t)AGORA_LOCALSOCK_HEADER_BYTES + payload_len;
  if (total > (size_t)AGORA_LOCALSOCK_MAX_DATAGRAM_BYTES) {
    errno = EMSGSIZE;
    return -1;
  }
  uint8_t *buf = (uint8_t *)malloc(total);
  if (buf == NULL) {
    return -1;
  }
  agora_localsock_header h;
  h.magic = AGORA_LOCALSOCK_MAGIC;
  h.ver = (uint16_t)AGORA_LOCALSOCK_VER;
  h.msg_type = msg_type;
  h.payload_len = (uint32_t)payload_len;
  memcpy(buf, &h, sizeof(h));
  if (payload_len > 0u) {
    memcpy(buf + AGORA_LOCALSOCK_HEADER_BYTES, payload, payload_len);
  }
  pthread_mutex_lock(&c->mu);
  ssize_t nw = send(c->fd, buf, total, 0);
  pthread_mutex_unlock(&c->mu);
  free(buf);
  if (nw != (ssize_t)total) {
    if (nw < 0) {
      return -1;
    }
    errno = EIO;
    return -1;
  }
  return 0;
}

int agora_localsock_client_send_data(agora_localsock_client *c, const char *data,
                                     int data_len) {
  if (c == NULL || data_len < 0) {
    errno = EINVAL;
    return -1;
  }
  if (data_len > 0 && data == NULL) {
    errno = EINVAL;
    return -1;
  }
  return agora_localsock_client_send_datagram(
      c, (uint16_t)AGORA_LOCALSOCK_MSG_APP, data, (size_t)data_len);
}
