#define _POSIX_C_SOURCE 200809L
#include <poll.h>

static int test_poll(struct pollfd *, nfds_t, int);
#define poll test_poll
#include "../src/agora_localsock.c"
#undef poll
#include <stdio.h>

#define CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "FAIL line %d: %s errno=%d\n", __LINE__, #expr, errno); \
  return 1; \
} } while (0)

static short events;
static int system_error;

static int test_poll(struct pollfd *fds, nfds_t count, int timeout) {
  (void)count;
  (void)timeout;
  fds[0].revents = events;
  if (system_error != 0) {
    errno = system_error;
    return -1;
  }
  return events != 0 ? 1 : 0;
}

int main(void) {
  /* A real ephemeral bind avoids assuming a fixed port is available. */
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  CHECK(fd >= 0);
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  CHECK(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
  CHECK(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  socklen_t size = sizeof(addr);
  CHECK(getsockname(fd, (struct sockaddr *)&addr, &size) == 0);
  uint16_t port = ntohs(addr.sin_port);
  close(fd);

  agora_localsock_server *server = NULL;
  agora_localsock_client *client = NULL;
  CHECK(agora_localsock_server_create(port, 1000u, 1u, &server) == 0);
  CHECK(agora_localsock_client_create(port, &client) == 0);
  CHECK((fcntl(client->fd, F_GETFL, 0) & O_NONBLOCK) != 0);
  const short injected[] = {POLLNVAL, POLLERR, POLLHUP};
  const int expected[] = {EBADF, EIO, EIO};
  char buf[64];
  size_t len;
  for (size_t i = 0; i < sizeof(injected) / sizeof(injected[0]); ++i) {
    events = injected[i];
    CHECK(agora_localsock_server_poll(server, 0, buf, sizeof(buf), &len) == -1);
    CHECK(errno == expected[i] && len == 0);
    CHECK(agora_localsock_server_poll_events(server) == events);
    CHECK(agora_localsock_client_poll(client, 0, buf, sizeof(buf), &len) == -1);
    CHECK(errno == expected[i] && len == 0);
    CHECK(agora_localsock_client_poll_events(client) == events);
  }
  events = 0;
  system_error = EIO;
  CHECK(agora_localsock_server_poll(server, 0, buf, sizeof(buf), &len) == -1);
  CHECK(errno == EIO);
  CHECK(agora_localsock_client_poll(client, 0, buf, sizeof(buf), &len) == -1);
  CHECK(errno == EIO);
  system_error = 0;
  CHECK(agora_localsock_server_poll(server, 0, buf, sizeof(buf), &len) == 0);
  CHECK(len == 0);
  CHECK(agora_localsock_client_poll(client, 0, buf, sizeof(buf), &len) == -1);
  CHECK(errno == EAGAIN && len == 0);
  agora_localsock_client_destroy(client);
  agora_localsock_server_destroy(server);
  puts("PASS test_agora_localsock_errors");
  return 0;
}
