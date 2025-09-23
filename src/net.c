#include "net.h"
#include <string.h>

int net_init(void) {
#ifdef _WIN32
    WSADATA wsa; return WSAStartup(MAKEWORD(2,2), &wsa);
#else
    return 0;
#endif
}

void net_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

void net_close(net_socket_t s) {
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

int net_set_nonblocking(net_socket_t s) {
#ifdef _WIN32
    u_long mode = 1; return ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0); if (flags < 0) return -1; return fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

net_socket_t net_connect_hostport(const char *host, const char *port) {
    struct addrinfo hints; memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    net_socket_t sock = -1;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        sock = (net_socket_t)socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock < 0) continue;
        if (connect(sock, p->ai_addr, (int)p->ai_addrlen) == 0) { break; }
        net_close(sock); sock = -1;
    }
    freeaddrinfo(res);
    return sock;
}

int net_send_all(net_socket_t s, const void *buf, int len) {
    const char *p = (const char*)buf; int sent = 0;
    while (sent < len) {
#ifdef _WIN32
        int n = send(s, p + sent, len - sent, 0);
#else
        int n = (int)send(s, p + sent, len - sent, 0);
#endif
        if (n <= 0) return n; sent += n;
    }
    return sent;
}

int net_recv_nonblocking(net_socket_t s, void *buf, int cap) {
#ifdef _WIN32
    return recv(s, (char*)buf, cap, 0);
#else
    return (int)recv(s, buf, cap, 0);
#endif
}


