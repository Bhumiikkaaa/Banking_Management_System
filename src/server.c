/*
 * server.c - the bank server.
 *
 *   - one listening socket
 *   - fork() a new process for every client that connects   (process mgmt)
 *   - client processes send log lines through a pipe         (IPC)
 *   - a logger thread in the parent writes them to a file    (multithreading)
 *   - all account data lives in files, guarded by fcntl locks (file locking)
 *
 * usage: ./server [port] [data_dir]
 */
#define _GNU_SOURCE
#include "bank.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int log_pipe[2];

/* Reads whatever the client processes put into the pipe and appends it to
 * server.log (and the terminal). Runs for the whole life of the server. */
static void *logger_thread(void *arg)
{
    (void)arg;
    char path[256], buf[1024];
    snprintf(path, sizeof path, "%s/server.log", g_data_dir);
    int lf = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);

    ssize_t n;
    while ((n = read(log_pipe[0], buf, sizeof buf)) > 0) {
        if (lf >= 0) { ssize_t w = write(lf, buf, (size_t)n); (void)w; }
        ssize_t w2 = write(STDOUT_FILENO, buf, (size_t)n);
        (void)w2;
    }
    return NULL;
}

/* parent must reap finished children or they stay as zombies */
static void on_sigchld(int sig)
{
    (void)sig;
    int saved = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) { }
    errno = saved;
}

static const char *MAIN_MENU =
    "\n=========== Online Banking ===========\n"
    "1. Customer login\n"
    "2. Employee login\n"
    "3. Manager login\n"
    "4. Administrator login\n"
    "5. Exit\n";

/* returns when the client disconnects or chooses Exit */
static void serve_client(int fd)
{
    int tries = 0;

    for (;;) {
        net_send(fd, "%s", MAIN_MENU);
        int role = net_ask_choice(fd, "Login as: ");
        if (role < 0) return;
        if (role == 5) { net_send(fd, "Goodbye!\n"); return; }
        if (role < 1 || role > 4) { net_send(fd, "Please choose 1-5.\n"); continue; }

        char name[64], pw[64];
        if (net_ask(fd, "Username: ", name, sizeof name) < 0) return;
        if (net_ask(fd, "Password: ", pw, sizeof pw) < 0) return;

        User u;
        if (user_check_login(role, name, pw, &u) < 0) {
            tries++;
            log_event("failed login for '%s' (%d/%d)", name, tries, MAX_LOGIN_TRIES);
            if (tries >= MAX_LOGIN_TRIES) {
                net_send(fd, "Too many wrong attempts. Disconnecting.\n");
                return;
            }
            net_send(fd, "Wrong username or password.\n");
            continue;
        }
        tries = 0;

        if (session_enter(u.id) < 0) {
            net_send(fd, "This user is already logged in somewhere else.\n");
            continue;
        }

        net_send(fd, "\nWelcome, %s!\n", u.username);
        log_event("%s (%s) logged in", u.username, role_name(role));

        int rc = 0;
        switch (role) {
        case ROLE_CUSTOMER: rc = customer_menu(fd, &u); break;
        case ROLE_EMPLOYEE: rc = employee_menu(fd, &u); break;
        case ROLE_MANAGER:  rc = manager_menu(fd, &u);  break;
        case ROLE_ADMIN:    rc = admin_menu(fd, &u);    break;
        }

        session_leave(u.id);
        log_event("%s logged out", u.username);
        if (rc < 0) return;                  /* client vanished */
        net_send(fd, "\nLogged out.\n");
    }
}

int main(int argc, char **argv)
{
    int port = DEFAULT_PORT;
    if (argc > 1) port = atoi(argv[1]);
    if (argc > 2) snprintf(g_data_dir, sizeof g_data_dir, "%s", argv[2]);

    if (store_init() < 0) {
        fprintf(stderr, "cannot prepare data files in '%s'\n", g_data_dir);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    if (pipe(log_pipe) < 0) { perror("pipe"); return 1; }
    pthread_t tid;
    pthread_create(&tid, NULL, logger_thread, NULL);
    pthread_detach(tid);
    g_log_fd = log_pipe[1];

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("socket"); return 1; }
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);

    if (bind(ls, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(ls, 16) < 0) { perror("listen"); return 1; }

    log_event("server started on port %d, data dir '%s'", port, g_data_dir);

    for (;;) {
        int cfd = accept(ls, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); close(cfd); continue; }

        if (pid == 0) {                       /* ---- child: one client ---- */
            close(ls);
            close(log_pipe[0]);
            if (store_open() < 0) {
                net_send(cfd, "Server error: cannot open data files.\n");
                _exit(1);
            }
            log_event("client connected");
            serve_client(cfd);
            log_event("client disconnected");
            store_close();                    /* also drops any leftover locks */
            close(cfd);
            _exit(0);
        }
        close(cfd);                           /* parent keeps listening */
    }
}
