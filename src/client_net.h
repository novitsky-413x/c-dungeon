#ifndef CLIENT_NET_H
#define CLIENT_NET_H

#include "net.h"
#include "types.h"
#include "mp.h"

int client_connect(const char *addr_input); // addr_input: host or host:port
void client_disconnect(void);
void client_send_input(int dx, int dy, int shoot);
int client_poll_messages(void); // returns 1 if a snapshot was applied (redraw)
void client_send_bye(void);

#endif // CLIENT_NET_H


