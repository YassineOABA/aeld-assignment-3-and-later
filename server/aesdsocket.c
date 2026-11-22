#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define PORT_STR "9000"
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define INITIAL_BUF_SIZE 1024
#define TIMESTAMP_INTERVAL_SEC 10

volatile sig_atomic_t g_caught_signal = 0;
static int g_srvsockfd = -1;

typedef struct thread_node {
    pthread_t thread_id;
    int client_sockfd;
    char client_ip[INET6_ADDRSTRLEN];
    bool thread_complete;
    pthread_mutex_t *file_mutex;
    SLIST_ENTRY(thread_node) entries;
} thread_node_t;

SLIST_HEAD(thread_list_head, thread_node);

typedef struct {
    pthread_mutex_t *file_mutex;
} timestamp_thread_arg_t;

static void signal_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM) {
        g_caught_signal = 1;
        syslog(LOG_INFO, "Caught signal, exiting");
        if (g_srvsockfd != -1) {
            shutdown(g_srvsockfd, SHUT_RDWR);
        }
    }
}

static void daemonize_process(void) {
    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "fork failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
        syslog(LOG_ERR, "setsid failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (chdir("/") < 0) {
        syslog(LOG_ERR, "chdir failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    int fd = open("/dev/null", O_RDWR);
    if (fd != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) {
            close(fd);
        }
    }
}

static bool send_all(int sockfd, const char *buf, size_t len) {
    size_t total_sent = 0;
    while (total_sent < len) {
        ssize_t sent = send(sockfd, buf + total_sent, len - total_sent, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR && !g_caught_signal) {
                continue;
            }
            syslog(LOG_ERR, "send failed: %s", strerror(errno));
            return false;
        }
        if (sent == 0) {
            return false;
        }
        total_sent += (size_t)sent;
    }
    return true;
}

static void *timestamp_thread_func(void *arg) {
    timestamp_thread_arg_t *ts_arg = (timestamp_thread_arg_t *)arg;

    while (!g_caught_signal) {
        for (int elapsed = 0; elapsed < TIMESTAMP_INTERVAL_SEC && !g_caught_signal; elapsed++) {
            sleep(1);
        }
        if (g_caught_signal) {
            break;
        }

        time_t now = time(NULL);
        struct tm tm_info;
        if (localtime_r(&now, &tm_info) == NULL) {
            syslog(LOG_ERR, "localtime_r failed");
            continue;
        }

        char time_str[128];
        if (strftime(time_str, sizeof(time_str), "%a, %d %b %Y %H:%M:%S %z", &tm_info) == 0) {
            syslog(LOG_ERR, "strftime failed");
            continue;
        }

        char line[160];
        int written = snprintf(line, sizeof(line), "timestamp:%s\n", time_str);
        if (written <= 0 || (size_t)written >= sizeof(line)) {
            syslog(LOG_ERR, "snprintf timestamp failed");
            continue;
        }

        if (pthread_mutex_lock(ts_arg->file_mutex) != 0) {
            syslog(LOG_ERR, "pthread_mutex_lock failed in timestamp thread");
            continue;
        }

        FILE *fp = fopen(DATA_FILE, "a");
        if (fp != NULL) {
            fwrite(line, 1, (size_t)written, fp);
            fflush(fp);
            fclose(fp);
        } else {
            syslog(LOG_ERR, "fopen failed for timestamp in %s: %s", DATA_FILE, strerror(errno));
        }

        pthread_mutex_unlock(ts_arg->file_mutex);
    }

    return NULL;
}

static void *client_thread_func(void *arg) {
    thread_node_t *node = (thread_node_t *)arg;
    char *buffer = NULL;
    char *line = NULL;
    FILE *datafile = NULL;
    bool io_error = false;

    buffer = malloc(INITIAL_BUF_SIZE);
    if (buffer == NULL) {
        syslog(LOG_ERR, "malloc failed");
        io_error = true;
        goto cleanup;
    }

    size_t capacity = INITIAL_BUF_SIZE;
    size_t currentlength = 0;

    while (!g_caught_signal) {
        if (currentlength >= capacity - 1) {
            size_t newcapacity = capacity * 2;
            char *tmp = realloc(buffer, newcapacity);
            if (tmp == NULL) {
                syslog(LOG_ERR, "realloc failed");
                io_error = true;
                goto cleanup;
            }
            capacity = newcapacity;
            buffer = tmp;
        }

        ssize_t receivedbytes = recv(node->client_sockfd, buffer + currentlength, 1, 0);
        if (receivedbytes < 0) {
            if (errno == EINTR && !g_caught_signal) {
                continue;
            }
            syslog(LOG_ERR, "recv failed from %s: %s", node->client_ip, strerror(errno));
            io_error = true;
            goto cleanup;
        }

        if (receivedbytes == 0) {
            if (currentlength > 0) {
                if (pthread_mutex_lock(node->file_mutex) == 0) {
                    datafile = fopen(DATA_FILE, "a+");
                    if (datafile != NULL) {
                        fwrite(buffer, 1, currentlength, datafile);
                        fflush(datafile);
                        fclose(datafile);
                        datafile = NULL;
                    } else {
                        syslog(LOG_ERR, "fopen failed on EOF: %s", strerror(errno));
                        io_error = true;
                    }
                    pthread_mutex_unlock(node->file_mutex);
                }
            }
            break;
        }

        currentlength += (size_t)receivedbytes;
        buffer[currentlength] = '\0';

        if (buffer[currentlength - 1] == '\n') {
            if (pthread_mutex_lock(node->file_mutex) != 0) {
                syslog(LOG_ERR, "pthread_mutex_lock failed");
                io_error = true;
                goto cleanup;
            }

            datafile = fopen(DATA_FILE, "a+");
            if (datafile == NULL) {
                syslog(LOG_ERR, "fopen failed for append: %s", strerror(errno));
                pthread_mutex_unlock(node->file_mutex);
                io_error = true;
                goto cleanup;
            }

            if (fwrite(buffer, 1, currentlength, datafile) != currentlength) {
                syslog(LOG_ERR, "fwrite failed");
                fclose(datafile);
                datafile = NULL;
                pthread_mutex_unlock(node->file_mutex);
                io_error = true;
                goto cleanup;
            }
            fflush(datafile);
            fclose(datafile);
            datafile = NULL;

            FILE *readfile = fopen(DATA_FILE, "r");
            if (readfile == NULL) {
                syslog(LOG_ERR, "fopen failed for read: %s", strerror(errno));
                pthread_mutex_unlock(node->file_mutex);
                io_error = true;
                goto cleanup;
            }

            size_t readlength = 0;
            ssize_t readbytes;
            while ((readbytes = getline(&line, &readlength, readfile)) != -1) {
                if (!send_all(node->client_sockfd, line, (size_t)readbytes)) {
                    io_error = true;
                    break;
                }
            }

            fclose(readfile);
            pthread_mutex_unlock(node->file_mutex);

            shutdown(node->client_sockfd, SHUT_WR);
            currentlength = 0;
            break;
        }
    }

cleanup:
    (void)io_error;
    if (datafile != NULL) {
        fclose(datafile);
    }
    free(line);
    free(buffer);
    if (node->client_sockfd != -1) {
        close(node->client_sockfd);
        node->client_sockfd = -1;
    }
    syslog(LOG_INFO, "Closed connection from %s", node->client_ip);
    node->thread_complete = true;
    return NULL;
}

static void join_completed_threads(struct thread_list_head *head) {
    thread_node_t *curr = SLIST_FIRST(head);

    while (curr != NULL) {
        thread_node_t *next = SLIST_NEXT(curr, entries);
        if (curr->thread_complete) {
            pthread_join(curr->thread_id, NULL);
            SLIST_REMOVE(head, curr, thread_node, entries);
            free(curr);
        }
        curr = next;
    }
}

static void join_all_threads(struct thread_list_head *head) {
    while (!SLIST_EMPTY(head)) {
        thread_node_t *node = SLIST_FIRST(head);
        SLIST_REMOVE_HEAD(head, entries);
        pthread_join(node->thread_id, NULL);
        free(node);
    }
}

int main(int argc, char **argv) {
    bool daemon_mode = false;
    struct addrinfo hints;
    struct addrinfo *srvaddrinfo = NULL;
    struct sigaction sa;
    pthread_mutex_t file_mutex;
    struct thread_list_head head;
    SLIST_INIT(&head);

    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);

    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = true;
    }

    signal(SIGPIPE, SIG_IGN);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
        syslog(LOG_ERR, "sigaction failed: %s", strerror(errno));
        closelog();
        return EXIT_FAILURE;
    }

    if (pthread_mutex_init(&file_mutex, NULL) != 0) {
        syslog(LOG_ERR, "pthread_mutex_init failed");
        closelog();
        return EXIT_FAILURE;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int status = getaddrinfo(NULL, PORT_STR, &hints, &srvaddrinfo);
    if (status != 0) {
        syslog(LOG_ERR, "getaddrinfo failed: %s", gai_strerror(status));
        pthread_mutex_destroy(&file_mutex);
        closelog();
        return EXIT_FAILURE;
    }

    g_srvsockfd = socket(srvaddrinfo->ai_family, srvaddrinfo->ai_socktype, srvaddrinfo->ai_protocol);
    if (g_srvsockfd == -1) {
        syslog(LOG_ERR, "socket failed: %s", strerror(errno));
        freeaddrinfo(srvaddrinfo);
        pthread_mutex_destroy(&file_mutex);
        closelog();
        return EXIT_FAILURE;
    }

    int optval = 1;
    setsockopt(g_srvsockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    if (bind(g_srvsockfd, srvaddrinfo->ai_addr, srvaddrinfo->ai_addrlen) != 0) {
        syslog(LOG_ERR, "bind failed: %s", strerror(errno));
        close(g_srvsockfd);
        freeaddrinfo(srvaddrinfo);
        pthread_mutex_destroy(&file_mutex);
        closelog();
        return EXIT_FAILURE;
    }

    freeaddrinfo(srvaddrinfo);

    if (daemon_mode) {
        daemonize_process();
    }

    if (listen(g_srvsockfd, 10) == -1) {
        syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        close(g_srvsockfd);
        pthread_mutex_destroy(&file_mutex);
        closelog();
        return EXIT_FAILURE;
    }
    syslog(LOG_INFO, "Listening on port %s", PORT_STR);

    pthread_t ts_thread;
    timestamp_thread_arg_t ts_arg = { .file_mutex = &file_mutex };
    bool ts_thread_created = false;
    if (pthread_create(&ts_thread, NULL, timestamp_thread_func, &ts_arg) == 0) {
        ts_thread_created = true;
    } else {
        syslog(LOG_ERR, "pthread_create failed for timestamp thread");
    }

    while (!g_caught_signal) {
        struct sockaddr_storage addrcl;
        socklen_t addrlen = sizeof(addrcl);
        int clsockfd = accept(g_srvsockfd, (struct sockaddr *)&addrcl, &addrlen);
        if (clsockfd == -1) {
            if (g_caught_signal) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            continue;
        }

        thread_node_t *node = calloc(1, sizeof(thread_node_t));
        if (node == NULL) {
            syslog(LOG_ERR, "calloc failed for thread node");
            close(clsockfd);
            continue;
        }

        node->client_sockfd = clsockfd;
        node->thread_complete = false;
        node->file_mutex = &file_mutex;

        struct sockaddr_in *s = (struct sockaddr_in *)&addrcl;
        inet_ntop(AF_INET, &s->sin_addr, node->client_ip, sizeof(node->client_ip));
        syslog(LOG_INFO, "Accepted connection from %s", node->client_ip);

        if (pthread_create(&node->thread_id, NULL, client_thread_func, node) != 0) {
            syslog(LOG_ERR, "pthread_create failed for client %s", node->client_ip);
            close(clsockfd);
            free(node);
            continue;
        }

        SLIST_INSERT_HEAD(&head, node, entries);
        join_completed_threads(&head);
    }

    if (ts_thread_created) {
        pthread_join(ts_thread, NULL);
    }
    join_all_threads(&head);

    if (g_srvsockfd != -1) {
        close(g_srvsockfd);
        g_srvsockfd = -1;
    }
    remove(DATA_FILE);
    pthread_mutex_destroy(&file_mutex);
    closelog();
    return EXIT_SUCCESS;
}
