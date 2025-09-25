#ifndef NET_H
#define NET_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET net_socket_t;
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
typedef int net_socket_t;
#endif

int net_init(void);
void net_cleanup(void);
net_socket_t net_connect_hostport(const char *host, const char *port);
int net_set_nonblocking(net_socket_t s);
int net_set_tcp_nodelay_keepalive(net_socket_t s);
int net_send_all(net_socket_t s, const void *buf, int len);
int net_recv_nonblocking(net_socket_t s, void *buf, int cap);
void net_close(net_socket_t s);

#endif // NET_H


