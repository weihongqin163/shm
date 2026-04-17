/**
 * created by:wei
 * copyright (c) 2026 Agora IO. All rights reserved.
 * date: 2026-04-15
 *
 * Loopback UDP (127.0.0.1) with keep-alive and server-side peer expiry.
 * See PLAN_localsocket.md for protocol and policy.
 *
 * Wire integers use the same in-memory layout as the packed struct below on a
 * little-endian host (e.g. x86/x86-64, aarch64). No byte-order conversion is
 * performed. Non-little-endian targets are out of scope; there is no compile
 * #error guard—callers rely on this documentation and platform choice.
 */

#ifndef AGORA_LOCALSOCK_H
#define AGORA_LOCALSOCK_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

/** Magic (native uint32 on little-endian wire = first byte 0x41 'A'). */
#define AGORA_LOCALSOCK_MAGIC 0x4C4F4341u

#define AGORA_LOCALSOCK_VER 1u

#define AGORA_LOCALSOCK_MSG_KEEPALIVE 1u
#define AGORA_LOCALSOCK_MSG_APP 2u
#define AGORA_LOCALSOCK_MSG_WRITECMD 3u

/** Fixed header size on the wire (packed struct, 12 bytes on LE hosts). */
#define AGORA_LOCALSOCK_HEADER_BYTES 12u

/** Max IPv4 UDP payload (excluding IP/UDP headers); full datagram must fit. */
#define AGORA_LOCALSOCK_MAX_DATAGRAM_BYTES 65507u

/** Max application payload bytes in one `agora_localsock_client_send_data` call. */
#define AGORA_LOCALSOCK_MAX_APP_PAYLOAD_BYTES \
  (AGORA_LOCALSOCK_MAX_DATAGRAM_BYTES - AGORA_LOCALSOCK_HEADER_BYTES)

/**
 * 12-byte datagram header (packed). Layout is native on little-endian CPUs;
 * other translation units may fill this struct and memcpy to/from UDP payloads.
 */
typedef struct __attribute__((packed)) agora_localsock_header {
  uint32_t magic;
  uint16_t ver;
  uint16_t msg_type;
  uint32_t payload_len;
} agora_localsock_header;

_Static_assert(sizeof(agora_localsock_header) == AGORA_LOCALSOCK_HEADER_BYTES,
               "agora_localsock_header wire size");

typedef struct agora_localsock_server agora_localsock_server;
typedef struct agora_localsock_client agora_localsock_client;

/**
 * Creates a UDP server bound to 127.0.0.1:port.
 *
 * keepalive_interval_ms: client refresh period; a peer is removed after
 *   5 * keepalive_interval_ms of silence (monotonic clock).
 * max_clients: maximum distinct peer addresses tracked (must be > 0).
 *
 * Returns 0 on success; on failure returns -1 and errno is set.
 */
int agora_localsock_server_create(uint16_t port, uint32_t keepalive_interval_ms,
                                  size_t max_clients,
                                  agora_localsock_server **out);

void agora_localsock_server_destroy(agora_localsock_server *s);

/**
 * Waits up to timeout_ms for input, receives at most one valid datagram into
 * buf (up to cap bytes), updates the peer table, and evicts timed-out peers.
 * timeout_ms < 0 means wait indefinitely. On success, *out_len is the received
 * length (0 if none this call). Caller may call again with timeout_ms == 0
 * to drain additional datagrams. buf and out_len must be non-NULL and cap > 0.
 * Returns 0 on success, -1 on error (errno set).
 */
int agora_localsock_server_poll(agora_localsock_server *s, int timeout_ms,
                                  void *buf, size_t cap, size_t *out_len);

/**
 * Sends one datagram (header + payload) to every tracked peer. msg_type is
 * written into the localsock header; payload_len follows payload size.
 * Returns 0 if there are no peers; otherwise 0 if all sendto calls succeed, or
 * -1 on first failure (errno set). Serialized with server_poll recv path.
 */
int agora_localsock_server_send_datagram(agora_localsock_server *s,
                                         uint16_t msg_type, const void *payload,
                                         size_t payload_len);

/** Number of peers currently tracked (for tests / diagnostics). */
size_t agora_localsock_server_peer_count(const agora_localsock_server *s);

/**
 * Creates a client connected to 127.0.0.1:server_port (no explicit bind).
 * Keep-alive timing is owned by the application: call
 * agora_localsock_client_send_keepalive() from your business loop on a period
 * compatible with the server's keepalive_interval_ms (server evicts after
 * 5 * that interval without traffic).
 */
int agora_localsock_client_create(uint16_t server_port,
                                  agora_localsock_client **out);

void agora_localsock_client_destroy(agora_localsock_client *c);

/**
 * Waits up to timeout_ms for one datagram. On success writes full UDP payload
 * (localsock header + body) into buf and sets *out_len. timeout_ms < 0 means
 * wait indefinitely. Returns -1 on error; EAGAIN if no datagram before timeout
 * (when timeout_ms >= 0). Serialized with send_keepalive/send_data via mutex.
 */
int agora_localsock_client_poll(agora_localsock_client *c, int timeout_ms,
                                  void *buf, size_t cap, size_t *out_len);

/**
 * Sends one keep-alive datagram. Not thread-safe against concurrent destroy;
 * if multiple threads send on the same client, serialize externally.
 */
int agora_localsock_client_send_keepalive(agora_localsock_client *c);

/**
 * Sends one APP datagram (msg_type 2): header + payload. data_len may be 0
 * (data ignored). Same threading notes as send_keepalive.
 */
int agora_localsock_client_send_data(agora_localsock_client *c, const char *data,
                                     int data_len);

/**
 * Sends one datagram with arbitrary msg_type (header + payload). payload may
 * be NULL when payload_len is 0. Serialized with poll/keepalive/send_data.
 */
int agora_localsock_client_send_datagram(agora_localsock_client *c,
                                         uint16_t msg_type, const void *payload,
                                         size_t payload_len);

#endif /* AGORA_LOCALSOCK_H */
