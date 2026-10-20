// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/builtin_breathe — Stuck Agent Detection (ADR-0099 W6.2)
// F E R M I ∞ H A R T
//
// breathe — detect processes whose CPU is frozen: agents stuck in deadlock,
// wedged waits, infinite idle loops. The one ADR-0099 builtin with no
// overlap in the existing differentiators (W2.1).
//
//   breathe status [--json] [--all]              list processes
//   breathe check  [--json] [--all]              detect CPU-frozen processes
//                [--interval N] [--threshold N]
//   breathe kill <pid> [--json]                  SIGTERM a stuck process
//   breathe help
//
// Detection model (honest, portable): `check` persists a sample of
// (pid -> cpu_ticks) in ~/.jash/breathe_sample.tsv and compares against the
// previous sample. A process present in both samples whose cpu_ticks
// advanced <= threshold ticks over >= interval seconds is STUCK. First run
// saves the baseline and reports nothing — it never guesses from one
// sample. Process enumeration goes through OSIF L1 (bear_osinfo_proc_iter),
// never /proc paths or sysctl directly (ADR-0017).

#include <builtins/builtin.h>
#include "discovery_output.h"
#include "value_output.h"
#include <fcntl.h>
#include <inttypes.h>
#include <core/state.h>
#include <theme/io.h>
#include <osif/osinfo.h>
#include <sys/syscalls.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pwd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>

#define BREATHE_MAX_PROCS        512
#define BREATHE_DEFAULT_INTERVAL 5     /* seconds between samples */
#define BREATHE_DEFAULT_THRESH   0     /* max cpu-ticks advance = "frozen" */

typedef struct {
    int           pid;
    int           ppid;
    int           uid;
    char          comm[64];
    char          state;
    unsigned long rss_kb;
    unsigned long vsz_kb;
    unsigned long cpu_ticks;
    long          delta_ticks;   /* check only: advance since last sample */
    int           stuck;
    uint64_t      start_ticks;
    uint32_t      valid;
} breathe_proc_t;

typedef struct {
    breathe_proc_t* procs;
    int             count;
    int             cap;
    int             my_pid;
    int             my_uid;
    int             all;
    int             overflow;
} breathe_collect_ctx_t;

static int breathe_cb(const osif_procinfo_t* info, void* user) {
    breathe_collect_ctx_t* c = (breathe_collect_ctx_t*)user;
    if (c->count >= c->cap) { c->overflow = 1; return 1; }
    if (info->pid <= 1 || info->pid == c->my_pid) return 0;
    if (!c->all && c->my_uid != 0 && info->uid != c->my_uid) return 0;
    breathe_proc_t* p = &c->procs[c->count++];
    memset(p, 0, sizeof *p);
    p->pid       = info->pid;
    p->ppid      = info->ppid;
    p->uid       = info->uid;
    p->state     = info->state;
    p->rss_kb    = info->rss_kb;
    p->vsz_kb    = info->vsz_kb;
    p->cpu_ticks = info->cpu_ticks;
    p->start_ticks = info->start_ticks;
    p->valid = info->valid;
    strlcpy(p->comm, info->comm, sizeof(p->comm));
    return 0;
}

static int breathe_collect(breathe_proc_t* procs, int cap, int all) {
    if (!(bear_osinfo_caps() & OSIF_CAP_PROC)) return -1;
    breathe_collect_ctx_t c = {
        .procs = procs, .count = 0, .cap = cap,
        .my_pid = (int)getpid(), .my_uid = (int)getuid(), .all = all,
    };
    if (bear_osinfo_proc_iter(breathe_cb, &c) != 0) return -1;
    if (c.overflow) { errno = EOVERFLOW; return -1; }
    return c.count;
}

static const char* breathe_user(int uid) {
    struct passwd* pw = getpwuid((uid_t)uid);
    return pw ? pw->pw_name : "?";
}

static void breathe_json_str(jash_writer_t w, void* ctx, const char* s) {
    jash_writes(w, ctx, "\"");
    for (const char* p = s; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        switch (ch) {
            case '"':  jash_writes(w, ctx, "\\\""); break;
            case '\\': jash_writes(w, ctx, "\\\\"); break;
            case '\n': jash_writes(w, ctx, "\\n"); break;
            case '\t': jash_writes(w, ctx, "\\t"); break;
            default:
                if (ch < 0x20) jash_writef(w, ctx, "\\u%04x", ch);
                else { char b[2] = { (char)ch, 0 }; jash_writes(w, ctx, b); }
        }
    }
    jash_writes(w, ctx, "\"");
}

/* ── Sample persistence (~/.jash/breathe_sample.tsv) ────────────────────── */

static int breathe_sample_path(char* b, size_t cap) {
    const char* h = getenv("HOME");
    if (!h || !*h) return 0;
    int n = snprintf(b, cap, "%s/.jash/breathe_sample.tsv", h);
    return n >= 0 && (size_t)n < cap;
}

typedef struct { int pid; unsigned long ticks; uint64_t start; } breathe_prev_t;

/* v2 binds the sample to the process start counter, avoiding PID reuse. */
static int breathe_sample_load(breathe_prev_t* prev, int cap, int* n, long* epoch) {
    char path[600];
    *n = 0; *epoch = 0;
    if (!breathe_sample_path(path, sizeof path)) return -1;
    FILE* file = fopen(path, "r");
    if (!file) return errno == ENOENT ? 0 : -1;
    char line[256], trailing;
    int version = 0, error = 0;
    if (!fgets(line, sizeof line, file)) error = 1;
    else if (sscanf(line, "# jash-breathe-v2 %ld %c", epoch, &trailing) == 1) version = 2;
    else if (sscanf(line, "# %ld %c", epoch, &trailing) != 1) error = 1;
    while (!error && fgets(line, sizeof line, file)) {
        if (*n == cap) { error = 1; break; }
        breathe_prev_t value = {0};
        int fields = version == 2 ? sscanf(line, "%d %lu %" SCNu64 " %c", &value.pid, &value.ticks, &value.start, &trailing)
                                  : sscanf(line, "%d %lu %c", &value.pid, &value.ticks, &trailing);
        if (value.pid <= 0 || fields != (version == 2 ? 3 : 2)) { error = 1; break; }
        for (int j = 0; j < *n; j++) if (prev[j].pid == value.pid) error = 1;
        if (!error) prev[(*n)++] = value;
    }
    if (ferror(file) || *epoch <= 0) error = 1;
    if (fclose(file)) error = 1;
    return error ? -1 : 1;
}

static int breathe_sample_save_checked(const breathe_proc_t* procs, int count, int* published) {
    if (published) *published = 0;
    char path[600], directory[600], temporary[640];
    if (!breathe_sample_path(path, sizeof path)) { errno = ENAMETOOLONG; return -1; }
    memcpy(directory, path, strlen(path) + 1);
    *strrchr(directory, '/') = 0;
    if (mkdir(directory, 0700) && errno != EEXIST) return -1;
    int dfd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) return -1;
    snprintf(temporary, sizeof temporary, "%s.tmp.XXXXXX", path);
    int fd = mkstemp(temporary);
    if (fd < 0) { close(dfd); return -1; }
    FILE* file = fdopen(fd, "w");
    if (!file) { int saved = errno; close(fd); unlink(temporary); close(dfd); errno = saved; return -1; }
    int error = fprintf(file, "# jash-breathe-v2 %ld\n", (long)time(NULL)) < 0;
    for (int i = 0; i < count && !error; i++)
        if (fprintf(file, "%d\t%lu\t%" PRIu64 "\n", procs[i].pid, procs[i].cpu_ticks, procs[i].start_ticks) < 0) error = 1;
    if (fflush(file) || fsync(fd)) error = 1;
    int saved = error ? (errno ? errno : EIO) : 0;
    if (fclose(file) && !saved) saved = errno;
    if (!saved && rename(temporary, path)) saved = errno;
    else if (!saved) {
        if (published) *published = 1;
        if (fsync(dfd)) saved = errno;
    }
    if (saved) unlink(temporary);
    if (close(dfd) && !saved) saved = errno;
    if (saved) { errno = saved; return -1; }
    return 0;
}
static int breathe_sample_save(const breathe_proc_t* procs, int count) { return breathe_sample_save_checked(procs, count, NULL); }

static unsigned long breathe_prev_ticks(const breathe_prev_t* prev, int n,
                                        int pid, int* found) {
    for (int i = 0; i < n; i++) {
        if (prev[i].pid == pid) { *found = 1; return prev[i].ticks; }
    }
    *found = 0;
    return 0;
}

/* ── Subcommands ─────────────────────────────────────────────────────────── */

static void breathe_usage(jash_writer_t w, void* ctx) {
    jash_writes(w, ctx,
        "usage: breathe <command> [options]\n"
        "\n"
        "Commands:\n"
        "  status          list processes\n"
        "  check           detect CPU-frozen (stuck) processes\n"
        "  kill <pid>      SIGTERM a stuck process (own processes only)\n"
        "  help            this help\n"
        "\n"
        "Options:\n"
        "  --json          JSON envelope output\n"
        "  --all           include all users' processes (default: own)\n"
        "  --interval N    min seconds between check samples (default 5)\n"
        "  --threshold N   max cpu-ticks advance still \"frozen\" (default 0)\n"
        "\n"
        "Environment:\n"
        "  JASH_BREATHE_INTERVAL   default --interval\n"
        "  JASH_BREATHE_THRESHOLD  default --threshold\n"
        "\n"
        "check persists a cpu sample in ~/.jash/breathe_sample.tsv and\n"
        "compares against the previous run; the first run saves the\n"
        "baseline and reports nothing.\n"
    );
}

static int breathe_do_status(jash_writer_t w, void* ctx, int json, int all) {
    static breathe_proc_t procs[BREATHE_MAX_PROCS];
    int n = breathe_collect(procs, BREATHE_MAX_PROCS, all);
    if (n < 0) {
        jash_writes(w, ctx, "breathe: process info unavailable on this OS\n");
        return 1;
    }
    if (json) {
        jash_writef(w, ctx, "{\"count\":%d,\"procs\":[", n);
        for (int i = 0; i < n; i++) {
            if (i) jash_writes(w, ctx, ",");
            jash_writef(w, ctx, "{\"pid\":%d,\"ppid\":%d,\"uid\":%d,\"user\":",
                        procs[i].pid, procs[i].ppid, procs[i].uid);
            breathe_json_str(w, ctx, breathe_user(procs[i].uid));
            jash_writes(w, ctx, ",\"comm\":");
            breathe_json_str(w, ctx, procs[i].comm);
            jash_writef(w, ctx,
                        ",\"state\":\"%c\",\"rss_kb\":%lu,\"vsz_kb\":%lu,"
                        "\"cpu_ticks\":%lu}",
                        procs[i].state ? procs[i].state : '?',
                        procs[i].rss_kb, procs[i].vsz_kb, procs[i].cpu_ticks);
        }
        jash_writes(w, ctx, "]}\n");
        return 0;
    }
    jash_writef(w, ctx, "%-8s %-9s %-20s %-3s %10s %10s %8s\n",
                "PID", "USER", "COMMAND", "ST", "RSS(KB)", "VSZ(KB)", "CPU");
    for (int i = 0; i < n; i++) {
        jash_writef(w, ctx, "%-8d %-9s %-20.20s %-3c %10lu %10lu %8lu\n",
                    procs[i].pid, breathe_user(procs[i].uid), procs[i].comm,
                    procs[i].state ? procs[i].state : '?',
                    procs[i].rss_kb, procs[i].vsz_kb, procs[i].cpu_ticks);
    }
    jash_writef(w, ctx, "%d processes\n", n);
    return 0;
}

static int breathe_do_check(jash_writer_t w, void* ctx, int json, int all,
                            int interval, int threshold) {
    static breathe_proc_t procs[BREATHE_MAX_PROCS];
    static breathe_prev_t prev[BREATHE_MAX_PROCS];
    int n = breathe_collect(procs, BREATHE_MAX_PROCS, all);
    if (n < 0) {
        jash_writes(w, ctx, "breathe: process info unavailable on this OS\n");
        return 1;
    }

    long prev_epoch = 0;
    int prev_n = 0;
    int have_prev = breathe_sample_load(prev, BREATHE_MAX_PROCS,
                                        &prev_n, &prev_epoch);
    if (have_prev < 0) { jash_writes(w, ctx, "breathe: invalid sample\n"); return 1; }
    long now = (long)time(NULL);
    long elapsed = have_prev ? now - prev_epoch : 0;

    if (!have_prev) {
        if (breathe_sample_save(procs, n)) return 1;
        if (json) {
            jash_writef(w, ctx,
                "{\"baseline\":true,\"tracked\":%d,\"stuck\":0,\"procs\":[]}\n", n);
        } else {
            jash_writef(w, ctx,
                "breathe: baseline saved (%d processes tracked) — "
                "run check again in >=%ds to detect stuck processes\n",
                n, interval);
        }
        return 0;
    }

    if (elapsed < interval) {
        if (json) {
            jash_writef(w, ctx,
                "{\"baseline\":false,\"too_soon\":true,\"elapsed_s\":%ld,"
                "\"interval_s\":%d,\"stuck\":0,\"procs\":[]}\n",
                elapsed, interval);
        } else {
            jash_writef(w, ctx,
                "breathe: only %lds since last sample (need %ds) — "
                "baseline kept, nothing reported\n", elapsed, interval);
        }
        return 0;
    }

    int stuck = 0;
    for (int i = 0; i < n; i++) {
        int found = 0;
        unsigned long pt = breathe_prev_ticks(prev, prev_n, procs[i].pid, &found);
        if (!found) continue;
        procs[i].delta_ticks = (long)(procs[i].cpu_ticks - pt);
        if (procs[i].state == 'Z' || procs[i].state == 'I') continue;
        if (procs[i].delta_ticks <= threshold) {
            procs[i].stuck = 1;
            stuck++;
        }
    }
    if (breathe_sample_save(procs, n)) return 1;

    if (json) {
        jash_writef(w, ctx,
            "{\"baseline\":false,\"elapsed_s\":%ld,\"interval_s\":%d,"
            "\"threshold_ticks\":%d,\"tracked\":%d,\"stuck\":%d,\"procs\":[",
            elapsed, interval, threshold, n, stuck);
        int first = 1;
        for (int i = 0; i < n; i++) {
            if (!procs[i].stuck) continue;
            if (!first) jash_writes(w, ctx, ",");
            first = 0;
            jash_writef(w, ctx, "{\"pid\":%d,\"ppid\":%d,\"uid\":%d,\"user\":",
                        procs[i].pid, procs[i].ppid, procs[i].uid);
            breathe_json_str(w, ctx, breathe_user(procs[i].uid));
            jash_writes(w, ctx, ",\"comm\":");
            breathe_json_str(w, ctx, procs[i].comm);
            jash_writef(w, ctx,
                        ",\"state\":\"%c\",\"rss_kb\":%lu,\"cpu_ticks\":%lu,"
                        "\"delta_ticks\":%ld}",
                        procs[i].state ? procs[i].state : '?',
                        procs[i].rss_kb, procs[i].cpu_ticks,
                        procs[i].delta_ticks);
        }
        jash_writes(w, ctx, "]}\n");
        return stuck ? 2 : 0;
    }

    if (!stuck) {
        jash_writef(w, ctx,
            "breathe: no stuck processes (%d tracked over %lds)\n",
            n, elapsed);
        return 0;
    }
    jash_writef(w, ctx,
        "breathe: %d stuck process%s (cpu frozen over %lds, threshold %d ticks)\n",
        stuck, stuck == 1 ? "" : "es", elapsed, threshold);
    jash_writef(w, ctx, "%-8s %-9s %-20s %-3s %10s %6s\n",
                "PID", "USER", "COMMAND", "ST", "RSS(KB)", "DELTA");
    for (int i = 0; i < n; i++) {
        if (!procs[i].stuck) continue;
        jash_writef(w, ctx, "%-8d %-9s %-20.20s %-3c %10lu %6ld\n",
                    procs[i].pid, breathe_user(procs[i].uid), procs[i].comm,
                    procs[i].state ? procs[i].state : '?',
                    procs[i].rss_kb, procs[i].delta_ticks);
    }
    jash_writes(w, ctx, "kill with: breathe kill <pid>\n");
    return 2;
}

static int breathe_do_kill(jash_writer_t w, void* ctx, int json, int pid) {
    if (pid <= 0) {
        jash_writes(w, ctx, "usage: breathe kill <pid>\n");
        return 2;
    }
    const char* err = 0;
    if (pid <= 1)                       err = "refusing to signal pid <= 1";
    else if (pid == (int)getpid())      err = "refusing to signal myself";
    else if (pid == (int)getppid())     err = "refusing to signal my parent";
    if (!err) {
        osif_procinfo_t info;
        if (bear_osinfo_proc_get(pid, &info) != 0) {
            err = "no such process";
        } else if (getuid() != 0 && info.uid != (int)getuid()) {
            err = "not your process (use --all user or root)";
        }
    }
    if (err) {
        if (json) {
            jash_writef(w, ctx, "{\"pid\":%d,\"ok\":false,\"error\":", pid);
            breathe_json_str(w, ctx, err);
            jash_writes(w, ctx, "}\n");
        } else {
            jash_writef(w, ctx, "breathe: cannot kill %d: %s\n", pid, err);
        }
        return 1;
    }
    if (sys_kill((pid_t)pid, SIGTERM) < 0) {
        int e = errno;
        if (json) {
            jash_writef(w, ctx, "{\"pid\":%d,\"ok\":false,\"error\":", pid);
            breathe_json_str(w, ctx, strerror(e));
            jash_writes(w, ctx, "}\n");
        } else {
            jash_writef(w, ctx, "breathe: kill %d failed: %s\n",
                        pid, strerror(e));
        }
        return 1;
    }
    if (json) {
        jash_writef(w, ctx,
            "{\"pid\":%d,\"signal\":\"TERM\",\"ok\":true}\n", pid);
    } else {
        jash_writef(w, ctx, "breathe: sent SIGTERM to pid %d\n", pid);
    }
    return 0;
}

int jash_b_breathe(jash_state_t* s, jash_writer_t w, void* ctx,
                    const char* args) {
    if (!s || !w) return 1;

    int json = 0, all = 0;
    int interval = BREATHE_DEFAULT_INTERVAL;
    int threshold = BREATHE_DEFAULT_THRESH;
    const char* env;
    if ((env = getenv("JASH_BREATHE_INTERVAL")) && atoi(env) >= 0)
        interval = atoi(env);
    if ((env = getenv("JASH_BREATHE_THRESHOLD")) && atoi(env) >= 0)
        threshold = atoi(env);

    char subcmd[32] = "";
    int kill_pid = 0;

    if (args) {
        const char* a = args;
        while (*a) {
            while (*a == ' ' || *a == '\t') a++;
            if (!*a) break;
            if (strncmp(a, "--json", 6) == 0)      { json = 1; a += 6; }
            else if (strncmp(a, "--all", 5) == 0)  { all = 1;  a += 5; }
            else if (strncmp(a, "--interval", 10) == 0) {
                a += 10;
                while (*a == ' ' || *a == '\t') a++;
                interval = atoi(a);
                while (*a && *a != ' ' && *a != '\t') a++;
            }
            else if (strncmp(a, "--threshold", 11) == 0) {
                a += 11;
                while (*a == ' ' || *a == '\t') a++;
                threshold = atoi(a);
                while (*a && *a != ' ' && *a != '\t') a++;
            }
            else if (!subcmd[0]) {
                size_t i = 0;
                while (*a && *a != ' ' && *a != '\t' && i < sizeof(subcmd) - 1)
                    subcmd[i++] = *a++;
                subcmd[i] = '\0';
            }
            else {
                kill_pid = atoi(a);
                while (*a && *a != ' ' && *a != '\t') a++;
            }
        }
    }

    if (!subcmd[0] || strcmp(subcmd, "help") == 0) {
        breathe_usage(w, ctx);
        return 0;
    }
    if (strcmp(subcmd, "status") == 0)
        return breathe_do_status(w, ctx, json, all);
    if (strcmp(subcmd, "check") == 0)
        return breathe_do_check(w, ctx, json, all, interval, threshold);
    if (strcmp(subcmd, "kill") == 0)
        return breathe_do_kill(w, ctx, json, kill_pid);

    jash_writef(w, ctx, "breathe: unknown command '%s'\n", subcmd);
    breathe_usage(w, ctx);
    return 2;
}

static jdv_value_t breathe_record(jash_discovery_rows_t* rows, const breathe_proc_t* process) {
    struct passwd* user = (process->valid & OSIF_PROC_VALID_UID) ? getpwuid((uid_t)process->uid) : NULL;
    char state[2] = {process->state ? process->state : '?', 0};
#define BF(key, value) {JDV_LITERAL(key), (value)}
    jdv_record_input_t fields[] = {
        BF("pid", jdv_value_u64((uint64_t)process->pid)), BF("ppid", jdv_value_u64((uint64_t)process->ppid)),
        BF("uid", (process->valid & OSIF_PROC_VALID_UID) ? jdv_value_u64((uint64_t)process->uid) : jdv_value_nothing()),
        BF("user", jash_discovery_string(rows, user ? user->pw_name : NULL)), BF("comm", jash_discovery_string(rows, process->comm)),
        BF("state", jash_discovery_string(rows, state)), BF("rss_kb", (process->valid & OSIF_PROC_VALID_RSS) ? jdv_value_u64(process->rss_kb) : jdv_value_nothing()),
        BF("vsz_kb", (process->valid & OSIF_PROC_VALID_VSZ) ? jdv_value_u64(process->vsz_kb) : jdv_value_nothing()),
        BF("cpu_ticks", (process->valid & OSIF_PROC_VALID_CPU) ? jdv_value_u64(process->cpu_ticks) : jdv_value_nothing()),
        BF("start_ticks", (process->valid & OSIF_PROC_VALID_START) ? jdv_value_u64(process->start_ticks) : jdv_value_nothing()),
        BF("delta_ticks", process->stuck ? jdv_value_i64(process->delta_ticks) : jdv_value_nothing())
    };
    jdv_value_t value = jdv_value_nothing();
    if (jdv_value_record(&rows->arena, fields, sizeof fields / sizeof *fields, &value)) rows->error = 1;
    return value;
#undef BF
}
static int breathe_number(const char* text, int* output) {
    if (!text || !*text || *text < '0' || *text > '9') return -1;
    char* end; errno = 0; unsigned long value = strtoul(text, &end, 10);
    if (errno || *end || value > INT_MAX) return -1;
    *output = (int)value; return 0;
}

typedef enum {
    BREATHE_COMPAT_STATUS, BREATHE_COMPAT_BASELINE, BREATHE_COMPAT_TOO_SOON,
    BREATHE_COMPAT_CHECK, BREATHE_COMPAT_KILL, BREATHE_COMPAT_KILL_ERROR,
    BREATHE_COMPAT_SOURCE_ERROR
} breathe_compat_shape_t;

/* Preserve the historical trailing --json envelopes without re-running the
 * legacy effect engine. The values below come from the same checked collector,
 * PID/start-counter comparison and publication/signal result as canonical JSON
 * and DATA. Field order is retained for existing line-oriented consumers. */
static int breathe_compat_object(jash_json_writer_t* out, const jdv_value_t* value,
                                 const char* const* keys, size_t count, int opened,
                                 int check_rows, const char* error_text) {
    static const char* const status_keys[] = {
        "pid", "ppid", "uid", "user", "comm", "state", "rss_kb", "vsz_kb", "cpu_ticks"
    };
    static const char* const check_keys[] = {
        "pid", "ppid", "uid", "user", "comm", "state", "rss_kb", "cpu_ticks", "delta_ticks"
    };
    if (!opened) jash_jsonw_raw(out, "{");
    for (size_t i = 0; i < count && !out->error; i++) {
        const jdv_value_t* field = jash_discovery_field(value, keys[i]);
        if (!field) { out->error = EINVAL; return -1; }
        if (i) jash_jsonw_raw(out, ",");
        jash_jsonw_string(out, keys[i]);
        jash_jsonw_raw(out, ":");
        if (!strcmp(keys[i], "procs")) {
            if (field->kind != JDV_LIST) { out->error = EINVAL; return -1; }
            jash_jsonw_raw(out, "[");
            for (size_t j = 0; j < field->as.list.len && !out->error; j++) {
                if (j) jash_jsonw_raw(out, ",");
                if (breathe_compat_object(out, &field->as.list.items[j],
                        check_rows ? check_keys : status_keys, 9, 0, 0, NULL)) return -1;
            }
            jash_jsonw_raw(out, "]");
        } else if (error_text && !strcmp(keys[i], "error")) jash_jsonw_string(out, error_text);
        else if (jdv_json_write(out, field, &JDV_DEFAULT_LIMITS)) return -1;
    }
    jash_jsonw_raw(out, "}");
    return out->error ? -1 : 0;
}

static int breathe_compat_write(jash_json_writer_t* out, const jdv_value_t* value,
                                breathe_compat_shape_t shape, int opened,
                                const char* error_text) {
    static const char* const status[] = {"count", "procs"};
    static const char* const baseline[] = {"baseline", "tracked", "stuck", "procs"};
    static const char* const too_soon[] = {"baseline", "too_soon", "elapsed_s", "interval_s", "stuck", "procs"};
    static const char* const check[] = {"baseline", "elapsed_s", "interval_s", "threshold_ticks", "tracked", "stuck", "procs"};
    static const char* const killed[] = {"pid", "signal", "ok"};
    static const char* const kill_error[] = {"pid", "ok", "error"};
    static const char* const source_error[] = {"ok", "error", "errno", "applied", "durable"};
    const char* const* keys = NULL;
    size_t count = 0;
    switch (shape) {
        case BREATHE_COMPAT_STATUS: keys = status; count = sizeof status / sizeof *status; break;
        case BREATHE_COMPAT_BASELINE: keys = baseline; count = sizeof baseline / sizeof *baseline; break;
        case BREATHE_COMPAT_TOO_SOON: keys = too_soon; count = sizeof too_soon / sizeof *too_soon; break;
        case BREATHE_COMPAT_CHECK: keys = check; count = sizeof check / sizeof *check; break;
        case BREATHE_COMPAT_KILL: keys = killed; count = sizeof killed / sizeof *killed; break;
        case BREATHE_COMPAT_KILL_ERROR: keys = kill_error; count = sizeof kill_error / sizeof *kill_error; break;
        case BREATHE_COMPAT_SOURCE_ERROR: keys = source_error; count = sizeof source_error / sizeof *source_error; break;
    }
    return breathe_compat_object(out, value, keys, count, opened, shape == BREATHE_COMPAT_CHECK, error_text);
}

static int breathe_compat_emit(jash_writer_t writer, void* context, const jdv_value_t* value,
                               breathe_compat_shape_t shape, const char* error_text, int status) {
    const size_t limit = 32u * 1024u * 1024u;
    jash_structured_buffer_t buffer;
    jash_structured_buffer_init(&buffer, limit);
    jash_json_writer_t out;
    jash_jsonw_init(&out, jash_structured_buffer_sink, &buffer);
    int rc = breathe_compat_write(&out, value, shape, 0, error_text);
    jash_jsonw_raw(&out, "\n");
    if (rc || jash_jsonw_finish(&out)) { jash_structured_buffer_free(&buffer); return 125; }
    long budget = jash_budget_limit();
    rc = jash_structured_document(writer, context, "breathe", buffer.data,
        buffer.used, buffer.required, budget > 0 ? budget : (long)limit, status);
    jash_structured_buffer_free(&buffer);
    return rc;
}

int jash_b_breathe_argv(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    int mode, selected = jash_discovery_format(argc, argv, &mode);
    if (!selected) return jash_discovery_text(jash_b_breathe, state, writer, context, argc, argv);
    if (selected < 0) return jash_discovery_error(writer, context, "breathe", mode, "conflicting_formats", 2);
    int legacy_json = mode == JASH_DISCOVERY_JSON && argc > 0 && strcmp(argv[0], "--json");
    const char* command = NULL;
    int all = 0, interval = BREATHE_DEFAULT_INTERVAL, threshold = BREATHE_DEFAULT_THRESH, pid = 0;
    const char* setting = getenv("JASH_BREATHE_INTERVAL");
    if (setting && breathe_number(setting, &interval)) return jash_discovery_error(writer, context, "breathe", mode, "invalid_interval", 2);
    setting = getenv("JASH_BREATHE_THRESHOLD");
    if (setting && breathe_number(setting, &threshold)) return jash_discovery_error(writer, context, "breathe", mode, "invalid_threshold", 2);
    for (int i = 0; i < argc; i++) {
        if (jash_discovery_format_flag(argv[i])) continue;
        if (!strcmp(argv[i], "--all")) all = 1;
        else if (!strcmp(argv[i], "--interval") || !strcmp(argv[i], "--threshold")) {
            int* value = !strcmp(argv[i], "--interval") ? &interval : &threshold;
            if (++i == argc || breathe_number(argv[i], value)) return jash_discovery_error(writer, context, "breathe", mode, "invalid_options", 2);
        } else if (!command) command = argv[i];
        else if (strcmp(command, "kill") || pid || breathe_number(argv[i], &pid) || !pid)
            return jash_discovery_error(writer, context, "breathe", mode, "invalid_options", 2);
    }
    if (!command || (strcmp(command, "status") && strcmp(command, "check") && strcmp(command, "kill")))
        return jash_discovery_error(writer, context, "breathe", mode, "expected_status_check_kill", 2);
    int checking = !strcmp(command, "check"), killing = !strcmp(command, "kill");
    if (killing && !pid) return jash_discovery_error(writer, context, "breathe", mode, "missing_pid", 2);
    breathe_proc_t procs[BREATHE_MAX_PROCS]; breathe_prev_t previous[BREATHE_MAX_PROCS];
    int count = killing ? 0 : breathe_collect(procs, BREATHE_MAX_PROCS, all), prior_count = 0;
    long epoch = 0;
    int have_previous = checking ? breathe_sample_load(previous, BREATHE_MAX_PROCS, &prior_count, &epoch) : 0;
    if (count < 0 || have_previous < 0) return jash_discovery_error(writer, context, "breathe", mode, count < 0 ? "process_observation_failed" : "invalid_sample", 1);
    long now = (long)time(NULL), elapsed = have_previous ? now - epoch : 0;
    int too_soon = checking && have_previous && elapsed < interval, stuck = 0;
    for (int i = 0; checking && have_previous && !too_soon && i < count; i++) {
        if (!(procs[i].valid & OSIF_PROC_VALID_CPU) || !(procs[i].valid & OSIF_PROC_VALID_START)) continue;
        for (int j = 0; j < prior_count; j++) {
            if (previous[j].pid != procs[i].pid || !previous[j].start || previous[j].start != procs[i].start_ticks ||
                previous[j].ticks > procs[i].cpu_ticks || procs[i].state == 'Z' || procs[i].state == 'I') continue;
            uint64_t delta = procs[i].cpu_ticks - previous[j].ticks;
            if (delta <= (uint64_t)threshold) { procs[i].stuck = 1; procs[i].delta_ticks = (long)delta; stuck++; }
        }
    }
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(writer, context, "breathe", mode, "allocation_failed", 125);
    jdv_value_t items[BREATHE_MAX_PROCS]; size_t selected_count = 0;
    for (int i = 0; i < count && !rows.error; i++)
        if (!checking || procs[i].stuck) items[selected_count++] = breathe_record(&rows, &procs[i]);
    jdv_value_t processes = jdv_value_nothing();
    if (jdv_value_list(&rows.arena, items, selected_count, &processes)) rows.error = 1;
    int status = stuck ? 2 : 0, applied = 0, durable = 0, failure = 0;
    const char* error = NULL;
    osif_procinfo_t target;
    if (killing) {
        if (pid <= 1 || pid == getpid() || pid == getppid()) error = "protected_process";
        else if (bear_osinfo_proc_get(pid, &target)) error = "process_observation_failed";
        else if (getuid() != 0 && (!(target.valid & OSIF_PROC_VALID_UID) || target.uid != (int)getuid())) error = "not_process_owner";
    }
    int mutating = !rows.error && !error && (killing || (checking && !too_soon));
    jash_structured_output_t checked;
    if (mutating) {
        if (mode == JASH_DISCOVERY_DATA) {
            int admission = jash_discovery_effects_begin(&rows, writer, context, "breathe", mode, "jash.breathe.v1");
            if (admission) { jash_discovery_rows_free(&rows); return admission; }
        } else {
            if (jash_structured_begin(&checked, writer, context)) { jash_discovery_rows_free(&rows); return 125; }
            /* A JSON object prefix admits the historical receipt before the
             * effect without adding whitespace to its established framing. */
            jash_jsonw_raw(&checked.json, legacy_json ? "{" : " ");
            if (jash_jsonw_finish(&checked.json)) { jash_discovery_rows_free(&rows); return jash_structured_finish(&checked, 125); }
        }
        if (killing) {
            if (sys_kill(pid, SIGTERM)) { failure = errno; error = "signal_failed"; }
            else applied = 1;
        } else {
            if (breathe_sample_save_checked(procs, count, &applied)) { failure = errno; error = "sample_save_failed"; }
            else durable = 1;
        }
    }
    if (error) status = 1;
#define BF(key, value) {JDV_LITERAL(key), (value)}
    jdv_record_input_t fields[] = {
        BF("schema", jash_discovery_string(&rows, "jash.breathe.v1")), BF("operation", jash_discovery_string(&rows, command)),
        BF("count", jdv_value_u64((uint64_t)count)), BF("tracked", jdv_value_u64((uint64_t)count)),
        BF("baseline", jdv_value_bool(checking && !have_previous)), BF("too_soon", jdv_value_bool(too_soon)),
        BF("elapsed_s", jdv_value_i64(elapsed)), BF("interval_s", jdv_value_u64((uint64_t)interval)),
        BF("threshold_ticks", jdv_value_u64((uint64_t)threshold)), BF("stuck", jdv_value_u64((uint64_t)stuck)),
        BF("procs", processes), BF("pid", killing ? jdv_value_u64((uint64_t)pid) : jdv_value_nothing()),
        BF("ok", jdv_value_bool(!error && !rows.error)), BF("applied", jdv_value_bool(applied)), BF("durable", jdv_value_bool(durable)),
        BF("signal", jash_discovery_string(&rows, killing ? "TERM" : NULL)), BF("identity", jash_discovery_string(&rows, "pid-and-start-ticks")),
        BF("error", jash_discovery_string(&rows, error)), BF("errno", failure ? jdv_value_i64(failure) : jdv_value_nothing())
    };
#undef BF
    jash_discovery_add(&rows, fields, sizeof fields / sizeof *fields);
    breathe_compat_shape_t shape = killing ? (error ? BREATHE_COMPAT_KILL_ERROR : BREATHE_COMPAT_KILL)
        : error ? BREATHE_COMPAT_SOURCE_ERROR : !checking ? BREATHE_COMPAT_STATUS
        : !have_previous ? BREATHE_COMPAT_BASELINE : too_soon ? BREATHE_COMPAT_TOO_SOON : BREATHE_COMPAT_CHECK;
    const char* legacy_error = error;
    if (killing && error) {
        if (!strcmp(error, "protected_process")) legacy_error = pid <= 1 ? "refusing to signal pid <= 1"
            : pid == getpid() ? "refusing to signal myself" : "refusing to signal my parent";
        else if (!strcmp(error, "process_observation_failed")) legacy_error = "no such process";
        else if (!strcmp(error, "not_process_owner")) legacy_error = "not your process (use --all user or root)";
        else if (!strcmp(error, "signal_failed")) legacy_error = strerror(failure);
    }
    if (mode == JASH_DISCOVERY_DATA) status = jash_discovery_emit_schema(writer, context, "breathe", mode, &rows, status, "jash.breathe.v1");
    else if (mutating) {
        if (rows.error || !rows.count || (legacy_json
                ? breathe_compat_write(&checked.json, &rows.items[0], shape, 1, legacy_error)
                : jdv_json_write(&checked.json, &rows.items[0], &JDV_DEFAULT_LIMITS))) {
            if (!checked.json.error) {
                if (!legacy_json) jash_jsonw_raw(&checked.json, "{");
                jash_jsonw_raw(&checked.json, "\"complete\":false,\"error\":{\"code\":\"collection_limit_exceeded\",\"effects_may_have_applied\":true}}");
            }
            status = 75;
        }
        jash_jsonw_raw(&checked.json, "\n"); status = jash_structured_finish(&checked, status);
    } else status = rows.error ? jash_discovery_error(writer, context, "breathe", mode, "collection_limit", 75)
        : legacy_json ? breathe_compat_emit(writer, context, &rows.items[0], shape, legacy_error, status)
        : value_output_json(writer, context, "breathe", &rows.items[0], &JDV_DEFAULT_LIMITS, 32u * 1024u * 1024u, status);
    jash_discovery_rows_free(&rows); return status;
}
