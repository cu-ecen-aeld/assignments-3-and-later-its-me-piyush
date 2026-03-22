// CODE USED FROM - https://beej.us/guide/bgnet/html/#acceptthank-you-for-calling-port-3490
// Then modified using LLM

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "../aesd-char-driver/aesd_ioctl.h"

#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#define PORT "9000"
#define BACKLOG 10

#if USE_AESD_CHAR_DEVICE
#define DATAFILE "/dev/aesdchar"
#else
#define DATAFILE "/var/tmp/aesdsocketdata"
#endif

static volatile sig_atomic_t exit_requested = 0;
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_serverfd = -1;

struct thread_node {
    pthread_t thread;
    int clientfd;
    bool completed;
    SLIST_ENTRY(thread_node) entries;
};

SLIST_HEAD(thread_list_head, thread_node);
static struct thread_list_head g_threads = SLIST_HEAD_INITIALIZER(g_threads);

#if !USE_AESD_CHAR_DEVICE
static pthread_t g_timestamp_thread;
static bool g_timestamp_started = false;
#endif

/* Parse "AESDCHAR_IOCSEEKTO:X,Y" safely */
static bool parse_ioctl_command(const char *buf, size_t len, struct aesd_seekto *seekto)
{
    const char *prefix = "AESDCHAR_IOCSEEKTO:";
    size_t prefix_len = strlen(prefix);

    if (!buf || !seekto) return false;
    if (len <= prefix_len) return false;
    if (strncmp(buf, prefix, prefix_len) != 0) return false;

    const char *start = buf + prefix_len;
    const char *end = buf + len;
    const char *comma = memchr(start, ',', (size_t)(end - start));
    if (!comma) return false;

    char *cmd_end = NULL;
    char *off_end = NULL;

    errno = 0;
    unsigned long cmd = strtoul(start, &cmd_end, 10);
    if (errno != 0 || cmd_end != comma) return false;

    errno = 0;
    unsigned long off = strtoul(comma + 1, &off_end, 10);
    if (errno != 0) return false;

    /* allow optional trailing newline */
    if (!(off_end == end || (off_end + 1 == end && *off_end == '\n')))
        return false;

    /* prevent truncation when casting */
    if (cmd > UINT32_MAX || off > UINT32_MAX)
        return false;

    seekto->write_cmd = (uint32_t)cmd;
    seekto->write_cmd_offset = (uint32_t)off;

    return true;
}

/* send entire contents of fd to client */
static int send_from_fd(int fd, int clientfd)
{
    char buffer[4096];

    while (1) {
        ssize_t r = read(fd, buffer, sizeof(buffer));
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;

        ssize_t sent = 0;
        while (sent < r) {
            ssize_t s = send(clientfd, buffer + sent, (size_t)(r - sent), 0);
            if (s < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            sent += s;
        }
    }

    return 0;
}

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

static int create_server_socket(void)
{
    struct addrinfo hints, *res, *p;
    int sockfd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, PORT, &hints, &res) != 0)
        return -1;

    for (p = res; p; p = p->ai_next) {
        int opt = 1;

        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd < 0) continue;

        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == 0)
            break;

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

static int append_to_file(const char *buf, size_t len)
{
#if USE_AESD_CHAR_DEVICE
    int fd = open(DATAFILE, O_WRONLY);  /* char device handles its own offsets */
#else
    int fd = open(DATAFILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
#endif
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

    close(fd);
    return 0;
}

static int send_full_file(int clientfd)
{
    int fd = open(DATAFILE, O_RDONLY);
    if (fd < 0) return -1;

    lseek(fd, 0, SEEK_SET);
    int rc = send_from_fd(fd, clientfd);
    close(fd);
    return rc;
}

struct client_thread_args {
    struct thread_node *node;
    struct sockaddr_in client_addr;
};

static void *client_thread(void *arg)
{
    struct client_thread_args *args = arg;
    struct thread_node *node = args->node;
    int clientfd = node->clientfd;

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &args->client_addr.sin_addr, ip, sizeof(ip));
    syslog(LOG_INFO, "Accepted connection from %s", ip);
    free(args);

    char *packet = NULL;
    size_t packet_size = 0;

    while (!exit_requested) {
        char buf[1024];
        ssize_t r = recv(clientfd, buf, sizeof(buf), 0);
        if (r <= 0) break;

        char *tmp = realloc(packet, packet_size + r);
        if (!tmp) break;

        packet = tmp;
        memcpy(packet + packet_size, buf, r);
        packet_size += r;

        char *newline;
        while ((newline = memchr(packet, '\n', packet_size))) {
            size_t pkt_len = newline - packet + 1;

            struct aesd_seekto seekto;
            bool is_ioctl = parse_ioctl_command(packet, pkt_len, &seekto);

            if (is_ioctl) {
                pthread_mutex_lock(&file_mutex);

                int devfd = open(DATAFILE, O_RDWR);
                if (devfd >= 0) {
                    /* ioctl changes file position inside driver */
                    ioctl(devfd, AESDCHAR_IOCSEEKTO, &seekto);
                    send_from_fd(devfd, clientfd);  /* read from that position */
                    close(devfd);
                }

                pthread_mutex_unlock(&file_mutex);
            } else {
                pthread_mutex_lock(&file_mutex);
                int wrc = append_to_file(packet, pkt_len);
                int src = (wrc == 0) ? send_full_file(clientfd) : -1;
                syslog(LOG_ERR, "normal path: pkt_len=%zu wrc=%d src=%d errno=%d", pkt_len, wrc, src, errno);
                pthread_mutex_unlock(&file_mutex);

                
            }

            memmove(packet, packet + pkt_len, packet_size - pkt_len);
            packet_size -= pkt_len;
        }
    }

    free(packet);
    shutdown(clientfd, SHUT_RDWR);
    close(clientfd);
    syslog(LOG_INFO, "Closed connection from %s", ip);

    node->completed = true;
    return NULL;
}

int main(int argc, char *argv[])
{
    bool daemon_mode = false;
    if (argc == 2 && strcmp(argv[1], "-d") == 0)
        daemon_mode = true;

    openlog("aesdsocket", LOG_PID, LOG_USER);

    setup_signals();

    g_serverfd = create_server_socket();
    if (g_serverfd < 0)
        return -1;

    /* standard daemonization: fork, detach, drop stdio */
    if (daemon_mode) {
        pid_t pid = fork();
        if (pid < 0) return -1;
        if (pid > 0) exit(0);

        if (setsid() < 0) return -1;
        if (chdir("/") != 0) return -1;

        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
    }

    while (!exit_requested) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);

        int clientfd = accept(g_serverfd, (struct sockaddr *)&client_addr, &addrlen);
        if (clientfd < 0) continue;

        struct thread_node *node = calloc(1, sizeof(*node));
        node->clientfd = clientfd;
        SLIST_INSERT_HEAD(&g_threads, node, entries);

        struct client_thread_args *args = malloc(sizeof(*args));
        args->node = node;
        args->client_addr = client_addr;

        pthread_create(&node->thread, NULL, client_thread, args);
    }

    closelog();
    return 0;
}