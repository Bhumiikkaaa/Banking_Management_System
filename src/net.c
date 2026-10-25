/*
 * net.c - small helpers for talking to a client over a socket.
 *
 * The protocol is deliberately dumb: the server sends text (menus, prompts)
 * and then waits for ONE line from the client.  Every prompt ends with
 * ": " so a client (or the test script) knows when the server is waiting.
 */
#include "bank.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int g_log_fd = -1;       /* write end of the logging pipe (set by server.c) */

int net_send(int fd, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;

    int off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, (size_t)(n - off));
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        off += (int)w;
    }
    return 0;
}

/* reads bytes until '\n'. One byte per read() is slow but our lines are tiny
 * and it means we never swallow part of the *next* line. */
static int read_line(int fd, char *buf, size_t size)
{
    size_t len = 0;
    char c;
    for (;;) {
        ssize_t r = read(fd, &c, 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;               /* client closed or error */
        if (c == '\n') break;
        if (c != '\r' && len < size - 1) buf[len++] = c;
    }
    buf[len] = '\0';
    return 0;
}

int net_ask(int fd, const char *prompt, char *buf, size_t size)
{
    if (net_send(fd, "%s", prompt) < 0) return -1;
    return read_line(fd, buf, size);
}

int net_ask_choice(int fd, const char *prompt)
{
    char line[64];
    if (net_ask(fd, prompt, line, sizeof line) < 0) return -1;
    char *end;
    long v = strtol(line, &end, 10);
    if (end == line || *end != '\0' || v < 1 || v > 1000) return 0;
    return (int)v;
}

int net_ask_number(int fd, const char *prompt, long *out)
{
    char line[64];
    if (net_ask(fd, prompt, line, sizeof line) < 0) return -1;
    char *end;
    long v = strtol(line, &end, 10);
    if (end == line || *end != '\0' || v < 0) {
        if (net_send(fd, "That is not a valid number.\n") < 0) return -1;
        return 1;
    }
    *out = v;
    return 0;
}

/* "250", "250.5", "250.75"  ->  paise.  No signs, no more than 2 decimals. */
int parse_money(const char *s, money *out)
{
    while (*s == ' ') s++;
    if (!isdigit((unsigned char)*s)) return -1;

    long long whole = 0, frac = 0;
    int digits = 0;
    while (isdigit((unsigned char)*s)) {
        whole = whole * 10 + (*s - '0');
        if (++digits > 10) return -1;        /* keeps us far from overflow */
        s++;
    }
    if (*s == '.') {
        s++;
        int d = 0;
        while (isdigit((unsigned char)*s)) {
            if (d >= 2) return -1;
            frac = frac * 10 + (*s - '0');
            d++;
            s++;
        }
        if (d == 0) return -1;
        if (d == 1) frac *= 10;
    }
    while (*s == ' ') s++;
    if (*s != '\0') return -1;

    *out = whole * 100 + frac;
    return 0;
}

int net_ask_money(int fd, const char *prompt, money *out)
{
    char line[64];
    if (net_ask(fd, prompt, line, sizeof line) < 0) return -1;
    money m;
    if (parse_money(line, &m) < 0 || m <= 0) {
        if (net_send(fd, "Enter a positive amount like 250 or 250.75\n") < 0) return -1;
        return 1;
    }
    *out = m;
    return 0;
}

void fmt_money(money m, char *buf, size_t n)
{
    const char *sign = m < 0 ? "-" : "";
    if (m < 0) m = -m;
    snprintf(buf, n, "%sRs. %lld.%02lld", sign, m / 100, m % 100);
}

/* Sends one line to the logger thread in the server through a pipe.
 * A write smaller than PIPE_BUF is atomic, so lines from different client
 * processes never get mixed up. */
void log_event(const char *fmt, ...)
{
    char msg[300], line[400], ts[32];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);

    int n = snprintf(line, sizeof line, "[%s] pid=%d %s\n", ts, (int)getpid(), msg);
    if (n > (int)sizeof line - 1) n = (int)sizeof line - 1;
    if (g_log_fd >= 0) {
        ssize_t w = write(g_log_fd, line, (size_t)n);
        (void)w;
    }
}
