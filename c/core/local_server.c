// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include "local_server.h"
#include <osif/ipc.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define LOCAL_CLIENTS JASH_LOCAL_CLIENTS
static volatile sig_atomic_t stopping;
static void stop_server(int sig) { (void)sig; stopping = 1; }

static int send_text(int fd, const char* text) {
    size_t left = strlen(text);
    while (left) {
        ssize_t n = send(fd, text, left, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        text += n; left -= (size_t)n;
    }
    return 0;
}

static int scope_read(const char* file, char token[65]) {
    int fd = open(file, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077) ||
        (st.st_size != 64 && st.st_size != 65)) { close(fd); return -1; }
    char bytes[66]; ssize_t n = read(fd, bytes, sizeof(bytes)); close(fd);
    if (n != st.st_size || (n == 65 && bytes[64] != '\n')) return -1;
    for (int i = 0; i < 64; i++) if (!((bytes[i] >= '0' && bytes[i] <= '9') || (bytes[i] >= 'a' && bytes[i] <= 'f'))) return -1;
    memcpy(token, bytes, 64); token[64] = 0; return 0;
}

static int authenticate(int fd, const char* token, int* control, int* data, int shutdown_fd) {
    char greeting[256];
    snprintf(greeting, sizeof(greeting), "{\"jash_local\":1,\"auth\":\"scope-token\",\"framing\":\"jsonl\",\"control\":\"optional-fd-v1\",\"data\":\"optional-fd-v1\",\"idle_timeout_ms\":%d,\"max_clients\":%d}\n",
             JASH_LOCAL_IDLE_MS, JASH_LOCAL_CLIENTS);
    if (send_text(fd, greeting)) return -1;
    /* Canonical authentication frame is deliberately bounded. Subsequent
     * frames use the existing versioned JSONL request parser unchanged. */
    char line[192]; size_t used = 0;
    while (used < sizeof(line)) {
        struct pollfd inputs[2] = {{fd, POLLIN, 0}, {shutdown_fd, POLLIN, 0}};
        int ready = poll(inputs, 2, JASH_LOCAL_IDLE_MS);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return -1;
        if (inputs[1].revents) return -2;
        ssize_t n = read(fd, line + used, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n != 1) return -1;
        if (line[used++] == '\n') break;
    }
    static const char* const suffix[] = {"\"}\n", "\",\"control\":\"fd-v1\"}\n", "\",\"data\":\"fd-v1\"}\n", "\",\"control\":\"fd-v1\",\"data\":\"fd-v1\"}\n"};
    int mode = -1;
    for (int i = 0; i < 4; i++) if (used == 74 + strlen(suffix[i]) && !memcmp(line + 74, suffix[i], strlen(suffix[i]))) mode = i;
    if (mode < 0 || memcmp(line, "{\"scope\":\"", 10)) return -1;
    *control = mode & 1; *data = mode & 2;
    unsigned difference = 0;
    for (size_t i = 0; i < 64; i++) difference |= (unsigned char)line[10 + i] ^ (unsigned char)token[i];
    return difference ? -1 : 0;
}

typedef struct { pid_t pid; int channel; int entered; } local_client_t;

static int reap_client(local_client_t* client) {
    if (!client->pid) return 0;
    char event;
    while (recv(client->channel, &event, 1, MSG_DONTWAIT) == 1)
        if (event == 'R') client->entered = 1;
    int status = 0;
    pid_t reaped = waitpid(client->pid, &status, WNOHANG);
    if (!reaped || (reaped < 0 && errno == EINTR)) return 0;
    if (reaped < 0 && errno != ECHILD) return 0;
    /* Read any final readiness packet after observing the child's exit. */
    while (recv(client->channel, &event, 1, MSG_DONTWAIT) == 1)
        if (event == 'R') client->entered = 1;
    int failed = reaped < 0 || WIFSIGNALED(status) ||
        (client->entered && WIFEXITED(status) && WEXITSTATUS(status) == 125);
    close(client->channel); client->channel = -1; client->pid = 0; client->entered = 0;
    return failed;
}

int jash_local_server(const char* path, const char* scope_file, int (*session)(int, int)) {
    const char* cancel = getenv("JASH_AGENT_CANCEL_FD");
    if (cancel && *cancel) {
        fputs("jash-agentd: --listen cannot share JASH_AGENT_CANCEL_FD between workers\n", stderr);
        return 2;
    }
    char token[65];
    if (scope_read(scope_file, token)) { fputs("jash-agentd: invalid owner-only scope file\n", stderr); return 2; }
    int listener = jash_ipc_listen(path, LOCAL_CLIENTS);
    if (listener < 0) { perror("jash-agentd: local listener"); return 1; }
    struct stat bound;
    if (lstat(path, &bound)) { close(listener); return 1; }
    struct sigaction action = {0}, old_int, old_term;
    stopping = 0;
    action.sa_handler = stop_server; sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &old_int)) { close(listener); unlink(path); return 1; }
    if (sigaction(SIGTERM, &action, &old_term)) { sigaction(SIGINT, &old_int, NULL); close(listener); unlink(path); return 1; }
    local_client_t clients[LOCAL_CLIENTS] = {0};
    for (int i = 0; i < LOCAL_CLIENTS; i++) clients[i].channel = -1;
    int rc = 0;
    while (!stopping) {
        int slot = -1;
        for (int i = 0; i < LOCAL_CLIENTS; i++) {
            if (reap_client(&clients[i])) rc = 1;
            if (!clients[i].pid && slot < 0) slot = i;
        }
        struct pollfd poller = {.fd = listener, .events = POLLIN};
        int ready = poll(&poller, 1, 250);
        if (ready < 0) { if (errno == EINTR) continue; rc = 1; break; }
        if (stopping) break;
        if (!ready) continue;
        uid_t peer;
        int fd = jash_ipc_accept(listener, &peer);
        if (fd < 0) { if (errno == EINTR) continue; rc = 1; break; }
        if (stopping) { close(fd); break; }
        if (peer != geteuid() || slot < 0) {
            send_text(fd, "{\"error\":\"peer_or_capacity_denied\"}\n"); close(fd); continue;
        }
        int lifecycle[2];
        if (jash_ipc_shutdown_pair(lifecycle)) {
            send_text(fd, "{\"error\":\"worker_control_unavailable\"}\n"); close(fd); continue;
        }
        pid_t child = fork();
        if (child == 0) {
            close(listener);
            close(lifecycle[1]);
            for (int i = 0; i < LOCAL_CLIENTS; i++)
                if (clients[i].channel >= 0) close(clients[i].channel);
            sigaction(SIGINT, &old_int, NULL); sigaction(SIGTERM, &old_term, NULL);
            struct timeval timeout = {.tv_sec = JASH_LOCAL_IDLE_MS / 1000};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            int wants_control = 0, wants_data = 0, data_fd = -1;
            int auth = authenticate(fd, token, &wants_control, &wants_data, lifecycle[0]);
            if (auth) {
                if (auth != -2) send_text(fd, "{\"error\":\"scope_denied\"}\n");
                close(fd); _exit(auth == -2 ? 0 : 125);
            }
            memset(token, 0, sizeof(token));
            if (wants_control) {
                int endpoints[2];
                if (jash_ipc_control_pair(endpoints)) { send_text(fd, "{\"error\":\"control_unavailable\"}\n"); close(fd); _exit(125); }
                char value[32]; snprintf(value, sizeof(value), "%d", endpoints[0]);
                int failed = setenv("JASH_AGENT_CANCEL_FD", value, 1) ||
                    send_text(fd, "{\"jash_control\":1,\"transport\":\"fd-jsonl\",\"attachment\":\"scm-rights-one-byte-v1\"}\n") ||
                    jash_ipc_send_control(fd, endpoints[1]);
                close(endpoints[1]);
                if (failed) { close(endpoints[0]); close(fd); _exit(125); }
                /* endpoints[0] belongs only to this worker. It survives into
                 * session(), whose startup pins it and advertises cancellation. */
            }
            if (wants_data) {
                int endpoints[2];
                if (jash_ipc_data_pair(endpoints)) { send_text(fd, "{\"error\":\"data_unavailable\"}\n"); close(fd); _exit(125); }
                int failed = send_text(fd, "{\"jash_data\":1,\"transport\":\"seqpacket-fd-v1\",\"attachment\":\"scm-rights-one-byte-v1\"}\n") || jash_ipc_send_data_endpoint(fd, endpoints[1]);
                close(endpoints[1]);
                if (failed) { close(endpoints[0]); close(fd); _exit(125); }
                data_fd = endpoints[0];
            }
            if (dup2(fd, STDIN_FILENO) < 0 || dup2(fd, STDOUT_FILENO) < 0) _exit(125);
            if (fd > STDERR_FILENO) close(fd);
            /* Each scope-authenticated connection owns a separate runtime.
             * cwd/env memory cannot leak between clients. */
            if (send_text(lifecycle[0], "R")) _exit(0);
            _exit(session(lifecycle[0], data_fd));
        }
        close(fd);
        close(lifecycle[0]);
        if (child < 0) { close(lifecycle[1]); rc = 1; break; }
        clients[slot] = (local_client_t){child, lifecycle[1], 0};
    }
    close(listener);
    for (int i = 0; i < LOCAL_CLIENTS; i++) if (clients[i].pid)
        if (send_text(clients[i].channel, "S") && errno != EPIPE && errno != ECONNRESET) rc = 1;
    for (;;) {
        int active = 0;
        for (int i = 0; i < LOCAL_CLIENTS; i++) {
            if (reap_client(&clients[i])) rc = 1;
            if (clients[i].pid) active++;
        }
        if (!active) break;
        (void)poll(NULL, 0, 20);
    }
    struct stat current;
    if (!lstat(path, &current) && current.st_dev == bound.st_dev && current.st_ino == bound.st_ino) unlink(path);
    sigaction(SIGINT, &old_int, NULL); sigaction(SIGTERM, &old_term, NULL);
    return rc;
}
