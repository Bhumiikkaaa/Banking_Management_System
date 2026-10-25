/*
 * client.c - very small terminal client.
 * Prints everything the server sends and forwards every line you type.
 * usage: ./client [server_ip] [port]
 */
#include "bank.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *ip = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : DEFAULT_PORT;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "bad address: %s\n", ip);
        return 1;
    }
    if (connect(s, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("connect");
        return 1;
    }

    struct pollfd fds[2] = { { s, POLLIN, 0 }, { STDIN_FILENO, POLLIN, 0 } };
    char buf[1024];

    for (;;) {
        if (poll(fds, 2, -1) < 0) { perror("poll"); break; }

        if (fds[0].revents & (POLLIN | POLLHUP)) {      /* text from server */
            ssize_t n = read(s, buf, sizeof buf);
            if (n <= 0) break;                           /* server closed */
            if (write(STDOUT_FILENO, buf, (size_t)n) < 0) break;
        }
        if (fds[1].revents & POLLIN) {                   /* user typed */
            ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
            if (n <= 0) break;                           /* Ctrl-D */
            if (write(s, buf, (size_t)n) < 0) break;
        }
    }
    close(s);
    return 0;
}
