// CODE USED FROM - https://beej.us/guide/bgnet/html/#acceptthank-you-for-calling-port-3490
// Then modified using LLM: https://chatgpt.com/share/69920a0e-e0f4-8002-9ce1-0a8c0bc24621

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

#define PORT "9000"
#define DATAFILE "/var/tmp/aesdsocketdata"
#define BACKLOG 10

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

static pthread_t g_timestamp_thread;
static bool g_timestamp_started = false;

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
    // do NOT set SA_RESTART, we prefer accept/recv to return EINTR sometimes
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

/* ===================== FILE OPERATIONS (no locking inside) ===================== */

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

/* ===================== THREADS ===================== */

struct client_thread_args {
    struct thread_node *node;
    struct sockaddr_in client_addr;
};

static void *client_thread(void *arg)
{
    struct client_thread_args *args = (struct client_thread_args *)arg;
    struct thread_node *node = args->node;
    int clientfd = node->clientfd;

    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &args->client_addr.sin_addr, ip, sizeof(ip));
    syslog(LOG_INFO, "Accepted connection from %s", ip);

    free(args);

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

        char *tmp = realloc(packet, packet_size + (size_t)r);
        if (!tmp) {
            break;
        }
        packet = tmp;
        memcpy(packet + packet_size, buf, (size_t)r);
        packet_size += (size_t)r;

        char *newline;
        while ((newline = memchr(packet, '\n', packet_size)) != NULL) {
            size_t pkt_len = (size_t)(newline - packet) + 1;

            pthread_mutex_lock(&file_mutex);
            int wrc = append_to_file(packet, pkt_len);
            int src = (wrc == 0) ? send_full_file(clientfd) : -1;
            pthread_mutex_unlock(&file_mutex);

            if (wrc != 0 || src != 0) {
                goto out;
            }

            size_t remaining = packet_size - pkt_len;
            memmove(packet, packet + pkt_len, remaining);
            packet_size = remaining;

            if (packet_size == 0) {
                free(packet);
                packet = NULL;
            } else {
                char *shrunk = realloc(packet, packet_size);
                if (shrunk) packet = shrunk;
            }
        }
    }

out:
    free(packet);

    shutdown(clientfd, SHUT_RDWR);
    close(clientfd);

    syslog(LOG_INFO, "Closed connection from %s", ip);

    node->completed = true;
    return NULL;
}

static void *timestamp_thread(void *arg)
{
    (void)arg;

    while (!exit_requested) {
        // sleep 10s but allow quick exit
        for (int i = 0; i < 10 && !exit_requested; i++) {
            sleep(1);
        }
        if (exit_requested) break;

        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);

        char timestr[128];
        // RFC 2822-like format
        strftime(timestr, sizeof(timestr), "%a, %d %b %Y %H:%M:%S %z", &tm_now);

        char line[256];
        int len = snprintf(line, sizeof(line), "timestamp:%s\n", timestr);
        if (len < 0) continue;

        pthread_mutex_lock(&file_mutex);
        (void)append_to_file(line, (size_t)len);
        pthread_mutex_unlock(&file_mutex);
    }

    return NULL;
}

/* Join and free any completed threads (non-blocking) */
static void reap_completed_threads(void)
{
    struct thread_node *cur = SLIST_FIRST(&g_threads);

    while (cur) {
        struct thread_node *next = SLIST_NEXT(cur, entries);

        if (cur->completed) {
            pthread_join(cur->thread, NULL);

            // Portable removal (no SLIST_REMOVE_AFTER required)
            SLIST_REMOVE(&g_threads, cur, thread_node, entries);

            free(cur);
        }

        cur = next;
    }
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

    g_serverfd = create_server_socket();
    if (g_serverfd < 0) {
        syslog(LOG_ERR, "Socket setup failed");
        return -1;
    }

    if (daemon_mode) {
        if (daemonize() != 0) {
            syslog(LOG_ERR, "Daemonize failed");
            close(g_serverfd);
            g_serverfd = -1;
            return -1;
        }
    }

    // start timestamp thread (parent/main)
    if (pthread_create(&g_timestamp_thread, NULL, timestamp_thread, NULL) == 0) {
        g_timestamp_started = true;
    } else {
        syslog(LOG_ERR, "Failed to create timestamp thread");
    }

    while (!exit_requested) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);

        int clientfd = accept(g_serverfd, (struct sockaddr *)&client_addr, &addrlen);
        if (clientfd < 0) {
            if (errno == EINTR) continue;
            if (exit_requested) break;
            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            break;
        }

        struct thread_node *node = calloc(1, sizeof(*node));
        if (!node) {
            close(clientfd);
            continue;
        }

        node->clientfd = clientfd;
        node->completed = false;
        SLIST_INSERT_HEAD(&g_threads, node, entries);

        struct client_thread_args *args = malloc(sizeof(*args));
        if (!args) {
            SLIST_REMOVE_HEAD(&g_threads, entries);
            free(node);
            close(clientfd);
            continue;
        }

        args->node = node;
        args->client_addr = client_addr;

        if (pthread_create(&node->thread, NULL, client_thread, args) != 0) {
            syslog(LOG_ERR, "pthread_create failed");
            free(args);
            SLIST_REMOVE_HEAD(&g_threads, entries);
            free(node);
            close(clientfd);
            continue;
        }

        // valgrind-friendly: only reap after starting a new thread
        reap_completed_threads();
    }

    syslog(LOG_INFO, "Caught signal, exiting");

    // Stop accepting
    if (g_serverfd >= 0) {
        shutdown(g_serverfd, SHUT_RDWR);
        close(g_serverfd);
        g_serverfd = -1;
    }

    // Help client threads exit quickly
    struct thread_node *n;
    SLIST_FOREACH(n, &g_threads, entries) {
        shutdown(n->clientfd, SHUT_RDWR);
    }

    // Join remaining client threads and free nodes
    while (!SLIST_EMPTY(&g_threads)) {
        struct thread_node *node = SLIST_FIRST(&g_threads);
        SLIST_REMOVE_HEAD(&g_threads, entries);
        pthread_join(node->thread, NULL);
        free(node);
    }

    // Stop timestamp thread
    if (g_timestamp_started) {
        pthread_join(g_timestamp_thread, NULL);
    }

    pthread_mutex_destroy(&file_mutex);

    // Keep previous assignment behavior (usually required)
    unlink(DATAFILE);

    closelog();
    return 0;
}