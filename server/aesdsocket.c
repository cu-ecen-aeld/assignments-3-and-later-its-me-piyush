#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#define PORT "9000"
#define DATAFILE "/var/tmp/aesdsocketdata"
#define BACKLOG 10

static volatile sig_atomic_t exit_requested = 0;

/* ===================== SIGNAL HANDLING ===================== */

static void signal_handler(int signo)
{
    (void)signo;
    exit_requested = 1;
}

static int setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;

    if (sigaction(SIGINT, &sa, NULL) != 0) return -1;
    if (sigaction(SIGTERM, &sa, NULL) != 0) return -1;
    return 0;
}

/* ===================== SOCKET SETUP ===================== */

static int create_server_socket(void)
{
    struct addrinfo hints, *res, *p;
    int sockfd = -1;
    int status;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    status = getaddrinfo(NULL, PORT, &hints, &res);
    if (status != 0) return -1;

    for (p = res; p != NULL; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd < 0) continue;

        int opt = 1;
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
            close(sockfd);
            sockfd = -1;
            continue;
        }

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == 0) break;

        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(res);

    if (sockfd < 0) return -1;
    if (listen(sockfd, BACKLOG) != 0) {
        close(sockfd);
        return -1;
    }

    return sockfd;
}

/* ===================== DAEMONIZE ===================== */

static int daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) exit(EXIT_SUCCESS);

    if (setsid() < 0) return -1;

    pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) exit(EXIT_SUCCESS);

    umask(0);

    if (chdir("/") != 0) return -1;

    int fd = open("/dev/null", O_RDWR);
    if (fd < 0) return -1;

    if (dup2(fd, STDIN_FILENO) < 0) return -1;
    if (dup2(fd, STDOUT_FILENO) < 0) return -1;
    if (dup2(fd, STDERR_FILENO) < 0) return -1;

    if (fd > 2) {
        if (close(fd) != 0) return -1;
    }

    return 0;
}

/* ===================== FILE OPERATIONS ===================== */

static int append_to_file(const char *buf, size_t len)
{
    int fd = open(DATAFILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return -1;

    size_t written = 0;
    while (written < len) {
        ssize_t rc = write(fd, buf + written, len - written);
        if (rc < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        written += (size_t)rc;
    }

    if (close(fd) != 0) return -1;
    return 0;
}

static int send_full_file(int clientfd)
{
    int fd = open(DATAFILE, O_RDONLY);
    if (fd < 0) return -1;

    char buffer[4096];
    while (1) {
        ssize_t r = read(fd, buffer, sizeof(buffer));
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (r == 0) break;

        ssize_t sent = 0;
        while (sent < r) {
            ssize_t s = send(clientfd, buffer + sent, (size_t)(r - sent), 0);
            if (s < 0) {
                if (errno == EINTR) continue;
                close(fd);
                return -1;
            }
            sent += s;
        }
    }

    if (close(fd) != 0) return -1;
    return 0;
}

/* ===================== MAIN ===================== */

int main(int argc, char *argv[])
{
    bool daemon_mode = false;
    if (argc == 2 && strcmp(argv[1], "-d") == 0) daemon_mode = true;

    openlog("aesdsocket", LOG_PID, LOG_USER);

    if (setup_signals() != 0) {
        syslog(LOG_ERR, "Signal setup failed");
        return -1;
    }

    int serverfd = create_server_socket();
    if (serverfd < 0) {
        syslog(LOG_ERR, "Socket setup failed");
        return -1;
    }

    if (daemon_mode) {
        if (daemonize() != 0) {
            syslog(LOG_ERR, "Daemonize failed");
            close(serverfd);
            return -1;
        }
    }

    while (!exit_requested) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);

        int clientfd = accept(serverfd, (struct sockaddr *)&client_addr, &addrlen);
        if (clientfd < 0) {
            if (errno == EINTR && exit_requested) break;
            if (errno == EINTR) continue;
            syslog(LOG_ERR, "accept failed");
            break;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        syslog(LOG_INFO, "Accepted connection from %s", ip);

        char *packet = NULL;
        size_t packet_size = 0;

        while (!exit_requested) {
            char buf[1024];
            ssize_t r = recv(clientfd, buf, sizeof(buf), 0);
            if (r < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (r == 0) break;

            char *tmp = realloc(packet, packet_size + r);
            if (!tmp) {
                free(packet);
                packet = NULL;
                packet_size = 0;
                break;
            }
            packet = tmp;
            memcpy(packet + packet_size, buf, r);
            packet_size += (size_t)r;

            char *newline;
            while ((newline = memchr(packet, '\n', packet_size)) != NULL) {
                size_t pkt_len = (newline - packet) + 1;

                if (append_to_file(packet, pkt_len) != 0) break;
                if (send_full_file(clientfd) != 0) break;

                size_t remaining = packet_size - pkt_len;
                memmove(packet, packet + pkt_len, remaining);
                packet_size = remaining;

                char *shrunk = realloc(packet, packet_size);
                if (shrunk || packet_size == 0) packet = shrunk;
            }
        }

        free(packet);

        shutdown(clientfd, SHUT_RDWR);
        close(clientfd);

        syslog(LOG_INFO, "Closed connection from %s", ip);
    }

    syslog(LOG_INFO, "Caught signal, exiting");

    close(serverfd);
    unlink(DATAFILE);
    closelog();

    return 0;
}
