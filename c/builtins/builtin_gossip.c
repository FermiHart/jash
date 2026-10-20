// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/builtin_gossip — Context Sharing (ADR-0099)
// F E R M I ∞ H A R T

#include <builtins/builtin.h>
#include <core/state.h>
#include <core/argv.h>
#include <core/agent_json.h>
#include <theme/io.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <core/json_writer.h>
#include <core/output_budget.h>
#include <parser/builtin_redir.h>
#include "semantic_builtin.h"
#include "discovery_output.h"
#include <ctype.h>

#define GOSSIP_MAX 32

typedef struct {
    char sender[128];
    char message[512];
    long timestamp;
} gossip_entry_t;

static gossip_entry_t messages[GOSSIP_MAX];
static int msg_count = 0;
static int last_idx = -1;

typedef struct { uint64_t sequence; int64_t timestamp; char sender[128], message[512]; } channel_entry_t;
typedef struct { char magic[8]; uint64_t sequence; channel_entry_t entries[GOSSIP_MAX]; } channel_t;

static int channel_number(const char* s, uint64_t* out) {
    if (!s || !*s) return -1;
    uint64_t n = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || n > (UINT64_MAX - (unsigned)(*s - '0')) / 10) return -1;
        n = n * 10 + (unsigned)(*s - '0');
    }
    *out = n; return 0;
}

static int channel_command(jash_writer_t w, void* ctx, int argc, char** argv) {
    const char* channel = NULL; int i = 0;
    while (i < argc) {
        if (!strcmp(argv[i], "--json")) { i++; continue; }
        if (!strcmp(argv[i], "--channel") && i + 1 < argc && !channel) { channel = argv[i + 1]; i += 2; continue; }
        break;
    }
    if (!channel || !*channel || strlen(channel) > 48 || i >= argc) return 2;
    for (const char* p = channel; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return 2;
    const char* operation = argv[i++];
    int sending = !strcmp(operation, "send");
    uint64_t after = 0, limit = GOSSIP_MAX;
    const char* message = NULL;
    if (sending) {
        if (argc - i != 1 || strlen(argv[i]) >= 512) return 2;
        message = argv[i];
    } else if (!strcmp(operation, "receive")) {
        for (; i < argc; i += 2) {
            if (i + 1 >= argc) return 2;
            if (!strcmp(argv[i], "--after")) { if (channel_number(argv[i + 1], &after)) return 2; }
            else if (!strcmp(argv[i], "--limit")) { if (channel_number(argv[i + 1], &limit) || !limit || limit > GOSSIP_MAX) return 2; }
            else return 2;
        }
    } else return 2;
    const char* home = getenv("HOME");
    if (!home || home[0] != '/') return 1;
    int parent = open(home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent < 0) return 1;
    if (mkdirat(parent, ".jash", 0700) && errno != EEXIST) { close(parent); return 1; }
    int dir = openat(parent, ".jash", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); close(parent);
    struct stat st;
    if (dir < 0) return 1;
    if (fstat(dir, &st) || st.st_uid != geteuid() || (st.st_mode & 0022)) { close(dir); return 1; }
    char name[80]; snprintf(name, sizeof(name), "gossip-%s.bin", channel);
    char lockname[88]; snprintf(lockname, sizeof(lockname), "%s.lock", name);
    int lock = openat(dir, lockname, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (lock < 0) { close(dir); return 1; }
    if (fstat(lock, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077) ||
        st.st_nlink != 1 || flock(lock, LOCK_EX)) { close(lock); close(dir); return 1; }
    int fd = openat(dir, name, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (fd < 0) { close(lock); close(dir); return 1; }
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 || (st.st_mode & 0077) || flock(fd, LOCK_EX)) {
        close(fd); close(lock); close(dir); return 1;
    }
    channel_t data = {0}; int rc = 0;
    if (fstat(fd, &st)) rc = 1;
    else if (!st.st_size) memcpy(data.magic, "JASHG001", 8);
    else if (st.st_size != sizeof(data) || read(fd, &data, sizeof(data)) != sizeof(data) || memcmp(data.magic, "JASHG001", 8)) rc = 1;
    if (!rc && data.sequence) {
        uint64_t count = data.sequence < GOSSIP_MAX ? data.sequence : GOSSIP_MAX;
        for (uint64_t index = 0; index < count; index++) {
            uint64_t seq = data.sequence - index;
            channel_entry_t* entry = &data.entries[(seq - 1) % GOSSIP_MAX];
            if (entry->sequence != seq || !memchr(entry->message, 0, sizeof(entry->message))) { rc = 1; break; }
        }
    }
    uint64_t oldest = data.sequence > GOSSIP_MAX ? data.sequence - GOSSIP_MAX + 1 : 1;
    int expired = !sending && after && (after < oldest - 1 || after > data.sequence);
    if (!rc && sending) {
        if (data.sequence == UINT64_MAX) rc = 1;
        else {
            uint64_t seq = ++data.sequence;
            channel_entry_t* entry = &data.entries[(seq - 1) % GOSSIP_MAX];
            memset(entry, 0, sizeof(*entry)); entry->sequence = seq; entry->timestamp = time(NULL);
            snprintf(entry->sender, sizeof(entry->sender), "%ld", (long)getpid());
            memcpy(entry->message, message, strlen(message) + 1);
            char temp[96]; snprintf(temp, sizeof(temp), "%s.tmp.%ld", name, (long)getpid());
            int output = openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (output < 0) rc = 1;
            else {
                if (fchmod(output, 0600) || write(output, &data, sizeof(data)) != sizeof(data) || fsync(output)) rc = 1;
                if (close(output)) rc = 1;
                if (!rc && renameat(dir, temp, dir, name)) rc = 1;
                if (!rc && fsync(dir)) rc = 1;
                if (rc) unlinkat(dir, temp, 0);
            }
        }
    }
    close(fd); close(lock); close(dir);
    jash_budget_unwrap(&w, &ctx); jash_redir_stream_writer(w, ctx);
    jash_json_writer_t out; jash_jsonw_init(&out, w, ctx);
    jash_jsonw_raw(&out, "{\"schema\":\"jash.gossip.v1\",\"channel\":"); jash_jsonw_string(&out, channel);
    jash_jsonw_raw(&out, ",\"error\":");
    if (rc) jash_jsonw_string(&out, "journal_unavailable");
    else if (expired) jash_jsonw_string(&out, "cursor_expired");
    else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"persisted\":"); jash_jsonw_raw(&out, sending && !rc ? "true" : "false");
    jash_jsonw_raw(&out, ",\"sequence\":"); jash_jsonw_safe_u64(&out, data.sequence);
    jash_jsonw_raw(&out, ",\"messages\":[");
    uint64_t next = after, emitted = 0;
    if (!rc && !sending && !expired && after < data.sequence) {
        uint64_t start = after >= oldest ? after + 1 : oldest;
        for (uint64_t seq = start; seq <= data.sequence && emitted < limit; seq++) {
            channel_entry_t* entry = &data.entries[(seq - 1) % GOSSIP_MAX];
            if (entry->sequence != seq || !memchr(entry->message, 0, sizeof(entry->message))) { rc = 1; break; }
            if (emitted++) jash_jsonw_raw(&out, ",");
            jash_jsonw_raw(&out, "{\"sequence\":"); jash_jsonw_safe_u64(&out, seq);
            jash_jsonw_raw(&out, ",\"message\":"); jash_jsonw_string_or_bytes(&out, entry->message);
            jash_jsonw_raw(&out, ",\"timestamp\":"); jash_jsonw_safe_i64(&out, entry->timestamp);
            jash_jsonw_raw(&out, "}"); next = seq;
            if (seq == UINT64_MAX) break;
        }
    }
    jash_jsonw_raw(&out, "],\"next_cursor\":"); jash_jsonw_safe_u64(&out, sending ? data.sequence : next);
    jash_jsonw_raw(&out, "}\n");
    return jash_jsonw_finish(&out) ? 125 : rc ? rc : expired ? 2 : 0;
}

static const char* get_sender(void) {
    static char buf[128];
    const char* host = getenv("HOSTNAME");
    if (!host) host = "local";
    snprintf(buf, sizeof(buf), "%s@%d", host, (int)getpid());
    return buf;
}

#include "gossip_native.inc"

static int gossip_local(jash_writer_t w, void* ctx, int argc, char** argv) {
    int native = gossip_native(w, ctx, argc, argv);
    if (native >= 0) return native;
    for (int k = 0; k < argc && argv[k][0] == '-'; k++)
        if (!strcmp(argv[k], "--channel")) return channel_command(w, ctx, argc, argv);
    int json_out = 0;
    int i = 0;
    if (argc && !strcmp(argv[i], "--json")) { json_out = 1; i++; }
    if (i == argc) {
        jash_writes(w, ctx, "usage: gossip send MESSAGE | gossip receive [--json] | gossip clear\n"
                           "Experimental process-local buffer; no inter-session delivery.\n");
        return 0;
    }
    const char* cmd = argv[i++];
    if (!strcmp(cmd, "send")) {
        if (argc - i != 1 || strlen(argv[i]) >= sizeof(messages[0].message)) {
            jash_writes(jash_stderr_writer, NULL, "gossip: expected one message (up to 511 bytes)\n");
            return 2;
        }
        const char* content = argv[i];
        
        int idx = (last_idx + 1) % GOSSIP_MAX;
        strncpy(messages[idx].sender, get_sender(), 127);
        strncpy(messages[idx].message, content, 511);
        messages[idx].timestamp = time(NULL);
        last_idx = idx;
        if (msg_count < GOSSIP_MAX) msg_count++;
        
        jash_writef(w, ctx, "gossip: stored locally: %s\n", content);
        return 0;
    }
    
    if (!strcmp(cmd, "receive")) {
        if (i < argc && !strcmp(argv[i], "--json")) { json_out = 1; i++; }
        if (i != argc) return 2;
        if (json_out) {
            jash_writes(w, ctx, "[");
            for (int m = 0; m < msg_count; m++) {
                int idx = (last_idx - msg_count + 1 + m + GOSSIP_MAX) % GOSSIP_MAX;
                if (idx < 0) idx += GOSSIP_MAX;
                char sender[769], message[3073];
                jash_json_esc(messages[idx].sender, sender, sizeof(sender));
                jash_json_esc(messages[idx].message, message, sizeof(message));
                jash_writef(w, ctx, "%s{\"sender\":\"%s\",\"message\":\"%s\",\"timestamp\":%ld}",
                            m > 0 ? ", " : "",
                            sender, message,
                            messages[idx].timestamp);
            }
            jash_writes(w, ctx, "]\n");
            return 0;
        }
        
        for (int m = 0; m < msg_count; m++) {
            int idx = (last_idx - msg_count + 1 + m + GOSSIP_MAX) % GOSSIP_MAX;
            if (idx < 0) idx += GOSSIP_MAX;
            jash_writef(w, ctx, "[%ld] %s: %s\n",
                        (long)messages[idx].timestamp,
                        messages[idx].sender,
                        messages[idx].message);
        }
        return 0;
    }
    
    if (!strcmp(cmd, "clear") && i == argc) {
        msg_count = 0;
        last_idx = -1;
        jash_writes(w, ctx, "gossip: cleared\n");
        return 0;
    }
    
    jash_writes(jash_stderr_writer, NULL, "gossip: unknown subcommand or arguments\n");
    return 2;
}

int jash_b_gossip(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    if (!s || !w) return 1;
    jash_argv_t av = {0};
    if (jash_argv_build(args ? args : "", &av) < 0) return 1;
    int rc = gossip_local(w, ctx, av.argc, av.argv);
    jash_argv_free(&av);
    return rc;
}

int jash_b_gossip_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    if (!s || !w) return 1;
    return gossip_local(w, ctx, argc, argv);
}
