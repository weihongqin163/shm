#ifndef AGORA_LOCALSOCK_INTERNAL_H
#define AGORA_LOCALSOCK_INTERNAL_H

#include "agora_localsock.h"

/* Internal diagnostics; callers serialize with poll/send/destroy. */
short agora_localsock_server_poll_events(const agora_localsock_server *s);
short agora_localsock_client_poll_events(const agora_localsock_client *c);

#endif
