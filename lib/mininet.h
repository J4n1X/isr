#ifndef MININET_H
#define MININET_H

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef SOCKET net_sock_t;
    #define NET_INVALID_SOCKET INVALID_SOCKET
    #define net_error() WSAGetLastError()
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <errno.h>
    typedef int net_sock_t;
    #define NET_INVALID_SOCKET (-1)
    #define net_error() errno
#endif

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- API Declarations --- */

/* Initialize networking (Required on Windows, no-op on POSIX). Returns 0 on success. */
int net_init(void);

/* Cleanup networking resources. */
void net_cleanup(void);

/* Connect to a remote host and port (IPv4/IPv6 agnositc). Returns socket or NET_INVALID_SOCKET. */
net_sock_t net_connect(const char* host, const char* port);

/* Bind and listen on a local port. Returns listening socket or NET_INVALID_SOCKET. */
net_sock_t net_listen(const char* port);

/* Accept an incoming connection. Returns client socket or NET_INVALID_SOCKET. */
net_sock_t net_accept(net_sock_t server_sock);

/* Send data. Returns bytes sent, or -1 on error. */
int net_send(net_sock_t sock, const void* data, size_t len);

/* Receive data. Returns bytes received, 0 on disconnect, or -1 on error. */
int net_recv(net_sock_t sock, void* buf, size_t len);

/* Close a socket. */
void net_close(net_sock_t sock);

#ifdef __cplusplus
}
#endif

/* --- Implementation --- */

#ifdef NET_IMPLEMENTATION

int net_init(void) {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) != 0 ? -1 : 0;
#else
    return 0; /* POSIX requires no init */
#endif
}

void net_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

net_sock_t net_connect(const char* host, const char* port) {
    struct addrinfo hints, *res, *p;
    net_sock_t sockfd = NET_INVALID_SOCKET;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;     /* Allow IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM; /* TCP stream sockets */

    if (getaddrinfo(host, port, &hints, &res) != 0) {
        return NET_INVALID_SOCKET;
    }

    for (p = res; p != NULL; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd == NET_INVALID_SOCKET) continue;

        if (connect(sockfd, p->ai_addr, (int)p->ai_addrlen) == -1) {
            net_close(sockfd);
            sockfd = NET_INVALID_SOCKET;
            continue;
        }
        break; /* Connected successfully */
    }

    freeaddrinfo(res);
    return sockfd;
}

net_sock_t net_listen(const char* port) {
    struct addrinfo hints, *res, *p;
    net_sock_t sockfd = NET_INVALID_SOCKET;
    int yes = 1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; /* Fill in my IP for me */

    if (getaddrinfo(NULL, port, &hints, &res) != 0) {
        return NET_INVALID_SOCKET;
    }

    for (p = res; p != NULL; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd == NET_INVALID_SOCKET) continue;

        /* Avoid "Address already in use" error message */
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(int));

        if (bind(sockfd, p->ai_addr, (int)p->ai_addrlen) == -1) {
            net_close(sockfd);
            sockfd = NET_INVALID_SOCKET;
            continue;
        }
        break;
    }

    freeaddrinfo(res);

    if (sockfd == NET_INVALID_SOCKET) return NET_INVALID_SOCKET;

    if (listen(sockfd, 10) == -1) { /* Backlog of 10 */
        net_close(sockfd);
        return NET_INVALID_SOCKET;
    }

    return sockfd;
}

net_sock_t net_accept(net_sock_t server_sock) {
    struct sockaddr_storage their_addr;
    socklen_t addr_size = sizeof(their_addr);
    return accept(server_sock, (struct sockaddr *)&their_addr, &addr_size);
}

int net_send(net_sock_t sock, const void* data, size_t len) {
    return send(sock, (const char*)data, (int)len, 0);
}

int net_recv(net_sock_t sock, void* buf, size_t len) {
    return recv(sock, (char*)buf, (int)len, 0);
}

void net_close(net_sock_t sock) {
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

#endif /* NET_IMPLEMENTATION */
#endif /* MININET_H */