// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/agent — agent ergonomics: kv store + doctor (F8, F9).
//
//   kv set KEY VALUE   — persist a value across separate `jash -c` calls
//   kv get KEY         — print the value (empty + rc=1 if absent)
//   kv del KEY         — remove a key
//   kv list            — list all key=value pairs
//   doctor             — structured health: disk/tmp/dirs/modules/keys
//
// The KV store lets an agent stash state without littering temp files (and
// without hitting a full /tmp). doctor surfaces environment health in one
// blob so the agent can self-diagnose (e.g. an exhausted temp filesystem).
// Pure C, no external deps. F E R M I ∞ H A R T

#define _GNU_SOURCE
#include "builtin.h"
#include "discovery_output.h"
#include <core/argv.h>
#include <core/json_writer.h>
#include <theme/io.h>
#include <core/agent_json.h>
#include <core/jash_invoice.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <module/blake3.h>
#include <sys/syscalls.h>

/* ── KV store: ~/.jash/kv.db, one "key\tvalue" per line ──────────────────── */

static int kv_path(char* buf, size_t cap) {
    const char* home = getenv("HOME");
    if (!home || !home[0]) return 0;
    char dir[400];
    snprintf(dir, sizeof(dir), "%s/.jash", home);
    mkdir(dir, 0700);
    snprintf(buf, cap, "%s/.jash/kv.db", home);
    return 1;
}

/* Encode/decode newlines+tabs so values stay single-line. */
static void kv_encode(const char* s, char* out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; s && s[i] && o + 3 < cap; i++) {
        if (s[i] == '\n')      { out[o++]='\\'; out[o++]='n'; }
        else if (s[i] == '\t') { out[o++]='\\'; out[o++]='t'; }
        else if (s[i] == '\\') { out[o++]='\\'; out[o++]='\\'; }
        else out[o++] = s[i];
    }
    out[o] = '\0';
}
static void kv_decode(const char* s, char* out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; s && s[i] && o + 1 < cap; i++) {
        if (s[i] == '\\' && s[i+1]) {
            i++;
            if (s[i]=='n') out[o++]='\n';
            else if (s[i]=='t') out[o++]='\t';
            else out[o++]=s[i];
        } else out[o++] = s[i];
    }
    out[o] = '\0';
}

/* Rewrite the db without `key` (and optionally append key=val). */
static int kv_rewrite(const char* key, const char* enc_val) {
    char path[512];
    if (!kv_path(path, sizeof(path))) return -1;
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE* in = fopen(path, "r");
    FILE* out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); return -1; }
    char line[8192];
    size_t klen = strlen(key);
    if (in) {
        while (fgets(line, sizeof(line), in)) {
            if (!strncmp(line, key, klen) && line[klen] == '\t') continue; /* drop old */
            fputs(line, out);
        }
        fclose(in);
    }
    if (enc_val) fprintf(out, "%s\t%s\n", key, enc_val);
    fclose(out);
    return rename(tmp, path);
}

static int kv_cmd(const char* args, jash_writer_t w, void* ctx) {
    const char* a = args ? args : "";
    while (*a == ' ') a++;

    if (!strncmp(a, "set", 3) && (a[3]==' '||a[3]=='\0')) {
        const char* p = a + 3; while (*p==' ') p++;
        char key[256]; int ki = 0;
        while (*p && *p != ' ' && ki < 255) key[ki++] = *p++;
        key[ki] = '\0';
        while (*p == ' ') p++;
        if (!key[0]) { jash_writes(w, ctx, "kv set: need KEY VALUE\n"); return 1; }
        char enc[8192]; kv_encode(p, enc, sizeof(enc));
        if (kv_rewrite(key, enc) != 0) { jash_writes(w, ctx, "kv set: write failed\n"); return 1; }
        return 0;
    }
    if (!strncmp(a, "get", 3) && (a[3]==' '||a[3]=='\0')) {
        const char* p = a + 3; while (*p==' ') p++;
        char path[512]; if (!kv_path(path, sizeof(path))) return 1;
        FILE* f = fopen(path, "r"); if (!f) return 1;
        char line[8192]; size_t plen = strlen(p); int found = 0;
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, p, plen) && line[plen] == '\t') {
                char* v = line + plen + 1;
                size_t vl = strlen(v);
                if (vl && v[vl-1]=='\n') v[vl-1]='\0';
                char dec[8192]; kv_decode(v, dec, sizeof(dec));
                jash_writef(w, ctx, "%s\n", dec);
                found = 1; break;
            }
        }
        fclose(f);
        return found ? 0 : 1;
    }
    if (!strncmp(a, "del", 3) && (a[3]==' '||a[3]=='\0')) {
        const char* p = a + 3; while (*p==' ') p++;
        if (!*p) { jash_writes(w, ctx, "kv del: need KEY\n"); return 1; }
        return kv_rewrite(p, NULL) == 0 ? 0 : 1;
    }
    if (!strcmp(a, "list") || a[0]=='\0') {
        char path[512]; if (!kv_path(path, sizeof(path))) return 1;
        FILE* f = fopen(path, "r"); if (!f) return 0;
        char line[8192];
        while (fgets(line, sizeof(line), f)) {
            char* tab = strchr(line, '\t');
            if (!tab) continue;
            *tab = '\0';
            char* v = tab + 1; size_t vl = strlen(v);
            if (vl && v[vl-1]=='\n') v[vl-1]='\0';
            char dec[8192]; kv_decode(v, dec, sizeof(dec));
            jash_writef(w, ctx, "%s=%s\n", line, dec);
        }
        fclose(f);
        return 0;
    }
    jash_writes(w, ctx, "usage: kv set|get|del|list\n");
    return 1;
}

/* ── doctor: structured environment health ───────────────────────────────── */

static void doc_disk(const char* path, const char* label, jash_writer_t w, void* ctx, int json) {
    struct statvfs vfs;
    if (statvfs(path, &vfs) != 0) return;
    unsigned long long freeb = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
    unsigned long long totb  = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
    int pct = totb ? (int)(100ULL * (totb - freeb) / totb) : 0;
    if (json)
        jash_writef(w, ctx, "  \"%s\":{\"free_mb\":%llu,\"used_pct\":%d},\n",
                    label, freeb >> 20, pct);
    else
        jash_writef(w, ctx, "  %-10s free=%lluMB used=%d%%\n", label, freeb >> 20, pct);
}

static int doc_writable(const char* dir) {
    char probe[PATH_MAX];
    int length = snprintf(probe, sizeof(probe), "%s/.jash_doctor_probe.XXXXXX", dir);
    if (length < 0 || (size_t)length >= sizeof probe) return 0;
    int fd = mkstemp(probe);
    if (fd < 0) return 0;
    int ok = close(fd) == 0;
    if (unlink(probe)) ok = 0;
    return ok;
}

static int doc_exists(const char* path) {
    struct stat st; return stat(path, &st) == 0;
}

/* open entries in ~/.jash/gaps.tsv (field 2 == "open") — the capability-gap
 * backlog is environment health too: it's work the swarm is waiting for. */
static int doc_gaps_open(void) {
    const char* home = getenv("HOME"); if (!home || !home[0]) return 0;
    char p[600]; snprintf(p, sizeof p, "%s/.jash/gaps.tsv", home);
    FILE* f = fopen(p, "r"); if (!f) return 0;
    int n = 0; char line[1600];
    while (fgets(line, sizeof line, f)) {
        char* t = strchr(line, '\t');
        if (t && !strncmp(t + 1, "open\t", 5)) n++;
    }
    fclose(f);
    return n;
}

static int doctor_cmd(jash_writer_t w, void* ctx) {
    int json = jash_agent_enabled();
    const char* home = getenv("HOME"); if (!home) home = jash_tmpdir();
    const char* tmpdir = getenv("TMPDIR"); if (!tmpdir || !tmpdir[0]) tmpdir = jash_tmpdir();

    char jtmp[400]; snprintf(jtmp, sizeof(jtmp), "%s/.jash/tmp", home);
    char mods[400]; snprintf(mods, sizeof(mods), "%s/.jash/modules", home);
    char gkey[400]; snprintf(gkey, sizeof(gkey), "%s/.config/jash/groq.key", home);

    if (json) jash_writes(w, ctx, "{\"jash_doctor\":1,\n");
    else      jash_writes(w, ctx, "\033[1;36m# jash doctor\033[0m\n");

    doc_disk("/", "root", w, ctx, json);
    doc_disk(tmpdir, "tmpdir", w, ctx, json);

    int tmp_ok = doc_writable(tmpdir);
    int jtmp_ok = doc_exists(jtmp) ? doc_writable(jtmp) : 0;

    if (json) {
        jash_json_writer_t json_writer;
        jash_jsonw_init(&json_writer, w, ctx);
        jash_jsonw_raw(&json_writer, "  \"tmpdir_path\":");
        jash_jsonw_text_or_bytes(&json_writer, tmpdir, strlen(tmpdir));
        jash_jsonw_raw(&json_writer, ",\n");
        if (jash_jsonw_finish(&json_writer)) return 125;
        jash_writef(w, ctx, "  \"tmpdir_writable\":%s,\n", tmp_ok ? "true":"false");
        jash_writef(w, ctx, "  \"jash_tmp_writable\":%s,\n", jtmp_ok ? "true":"false");
        jash_writef(w, ctx, "  \"modules_dir\":%s,\n", doc_exists(mods)?"true":"false");
        jash_writef(w, ctx, "  \"gaps_open\":%d,\n", doc_gaps_open());
        jash_writef(w, ctx, "  \"groq_key\":%s\n", doc_exists(gkey)?"true":"false");
        jash_writes(w, ctx, "}\n");
    } else {
        jash_writef(w, ctx, "  tmpdir     %s (%s)\n", tmpdir, tmp_ok?"writable":"NOT WRITABLE");
        if (!tmp_ok)
            jash_writef(w, ctx, "  \033[33m! set CLAUDE_CODE_TMPDIR=%s\033[0m\n", jtmp);
        jash_writef(w, ctx, "  ~/.jash/tmp %s\n", jtmp_ok?"ok":"missing/ro");
        jash_writef(w, ctx, "  modules    %s\n", doc_exists(mods)?"present":"absent");
        int gopen = doc_gaps_open();
        jash_writef(w, ctx, "  gaps       %d open%s\n", gopen,
                    gopen ? " — `gap list` shows the demand-weighted queue" : "");
        jash_writef(w, ctx, "  groq.key   %s\n", doc_exists(gkey)?"present":"absent");
    }
    return 0;
}

/* Native observations are shared by explicit JSON and JTV2 output. */
static int doctor_row(jash_discovery_rows_t* rows, const char* check, const char* path,
                       int healthy, int available, int has_value, uint64_t value,
                       int has_disk, uint64_t total, uint64_t free_bytes,
                       const char* error, int number) {
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("check"), jash_discovery_string(rows, check)},
        {JDV_LITERAL("path"), jash_discovery_path(rows, path)},
        {JDV_LITERAL("ok"), jdv_value_bool(!error)},
        {JDV_LITERAL("healthy"), healthy < 0 ? jdv_value_nothing() : jdv_value_bool(healthy)},
        {JDV_LITERAL("available"), available < 0 ? jdv_value_nothing() : jdv_value_bool(available)},
        {JDV_LITERAL("value"), has_value ? jdv_value_u64(value) : jdv_value_nothing()},
        {JDV_LITERAL("total_bytes"), has_disk ? jdv_value_filesize(total) : jdv_value_nothing()},
        {JDV_LITERAL("available_bytes"), has_disk ? jdv_value_filesize(free_bytes) : jdv_value_nothing()},
        {JDV_LITERAL("error"), jash_discovery_string(rows, error)},
        {JDV_LITERAL("errno"), number ? jdv_value_u64((unsigned)number) : jdv_value_nothing()},
    };
    if (jash_discovery_add(rows, fields, sizeof fields / sizeof fields[0])) return 1;
    return error != NULL;
}

static int doctor_disk(jash_discovery_rows_t* rows, const char* check, const char* path) {
    struct statvfs value;
    if (statvfs(path, &value)) return doctor_row(rows, check, path, -1, -1, 0, 0, 0, 0, 0, "statvfs_failed", errno);
    uint64_t unit = value.f_frsize, blocks = value.f_blocks, available = value.f_bavail;
    if ((unit && (blocks > UINT64_MAX / unit || available > UINT64_MAX / unit)) || available > blocks)
        return doctor_row(rows, check, path, -1, -1, 0, 0, 0, 0, 0, "invalid_disk_counters", EOVERFLOW);
    return doctor_row(rows, check, path, available != 0, 1, 0, 0, 1, blocks * unit, available * unit, NULL, 0);
}

static int doctor_write_probe(jash_discovery_rows_t* rows, const char* check, const char* path) {
    char probe[PATH_MAX];
    int length = snprintf(probe, sizeof probe, "%s/.jash_doctor_probe.XXXXXX", path);
    if (length < 0 || (size_t)length >= sizeof probe)
        return doctor_row(rows, check, path, -1, -1, 0, 0, 0, 0, 0, "path_too_long", ENAMETOOLONG);
    int fd = mkstemp(probe);
    if (fd < 0) {
        int number = errno;
        int observed_denial = number == EACCES || number == EPERM || number == ENOENT || number == ENOTDIR || number == EROFS;
        return doctor_row(rows, check, path, 0, 0, 0, 0, 0, 0, 0, observed_denial ? NULL : "probe_open_failed", number);
    }
    int number = 0; const char* error = NULL;
    if (close(fd)) { error = "probe_close_failed"; number = errno; }
    if (unlink(probe)) { error = "probe_cleanup_failed"; number = errno; }
    return doctor_row(rows, check, path, error ? -1 : 1, 1, 0, 0, 0, 0, 0, error, number);
}

static int doctor_presence(jash_discovery_rows_t* rows, const char* check, const char* path, int directory) {
    struct stat st;
    if (!lstat(path, &st)) return doctor_row(rows, check, path,
            directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode), 1, 0, 0, 0, 0, 0, NULL, 0);
    int number = errno;
    if (number == ENOENT || number == ENOTDIR) return doctor_row(rows, check, path, 0, 0, 0, 0, 0, 0, 0, NULL, 0);
    return doctor_row(rows, check, path, -1, -1, 0, 0, 0, 0, 0, "stat_failed", number);
}

static int doctor_gaps(jash_discovery_rows_t* rows, const char* path) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        int number = errno;
        if (number == ENOENT || number == ENOTDIR) return doctor_row(rows, "gaps.open", path, 1, 0, 1, 0, 0, 0, 0, NULL, 0);
        return doctor_row(rows, "gaps.open", path, -1, -1, 0, 0, 0, 0, 0, "gaps_open_failed", number);
    }
    struct stat st;
    const char* error = NULL; int number = 0;
    uint64_t count = 0; size_t bytes = 0, line_bytes = 0, field_length = 0;
    unsigned field = 0; char status[4];
    if (fstat(fd, &st)) { error = "gaps_stat_failed"; number = errno; }
    else if (!S_ISREG(st.st_mode)) { error = "invalid_gaps_file"; number = EINVAL; }
    char buffer[4096];
    while (!error) {
        ssize_t n = read(fd, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { error = "gaps_read_failed"; number = errno; break; }
        if (!n) break;
        bytes += (size_t)n;
        if (bytes > 16u * 1024u * 1024u) { error = "gaps_limit_exceeded"; number = EFBIG; break; }
        for (ssize_t i = 0; i < n; i++) {
            char c = buffer[i];
            if (!c) { error = "invalid_gaps_file"; number = EINVAL; break; }
            if (++line_bytes > 65536u) { error = "gaps_line_limit_exceeded"; number = EFBIG; break; }
            if (c == '\n') { field = 0; field_length = 0; line_bytes = 0; continue; }
            if (c == '\t') {
                if (field == 1 && field_length == 4 && !memcmp(status, "open", 4)) count++;
                if (field < 2) field++;
                field_length = 0;
            } else if (field == 1) {
                if (field_length < sizeof status) status[field_length] = c;
                field_length++;
            }
        }
    }
    if (close(fd) && !error) { error = "gaps_close_failed"; number = errno; }
    return doctor_row(rows, "gaps.open", path, error ? -1 : count == 0, 1, !error, count, 0, 0, 0, error, number);
}

int jash_b_doctor_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    (void)s;
    int mode;
    if (jash_discovery_format(argc, argv, &mode) < 0) return jash_discovery_error(w, ctx, "doctor", mode, "conflicting_formats", 2);
    int ended = 0;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        return jash_discovery_error(w, ctx, "doctor", mode, "invalid_options", 2);
    }
    if (mode == JASH_DISCOVERY_TEXT) return doctor_cmd(w, ctx);
    const char* home = getenv("HOME"); if (!home || !home[0]) home = jash_tmpdir();
    const char* temporary = getenv("TMPDIR"); if (!temporary || !temporary[0]) temporary = jash_tmpdir();
    if (strlen(temporary) >= PATH_MAX) return jash_discovery_error(w, ctx, "doctor", mode, "path_too_long", 2);
    const char* suffixes[] = {"/.jash/tmp", "/.jash/modules", "/.config/jash/groq.key", "/.jash/gaps.tsv"};
    char paths[4][PATH_MAX];
    for (size_t i = 0; i < 4; i++) {
        int length = snprintf(paths[i], sizeof paths[i], "%s%s", home, suffixes[i]);
        if (length < 0 || (size_t)length >= sizeof paths[i]) return jash_discovery_error(w, ctx, "doctor", mode, "path_too_long", 2);
    }
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(w, ctx, "doctor", mode, "allocation_failed", 125);
    int admitted = jash_discovery_effects_begin(&rows, w, ctx, "doctor", mode, "jash.doctor.v1");
    if (admitted) { jash_discovery_rows_free(&rows); return admitted; }
    int status = 0;
#define DOCTOR_OBSERVE(expression) do { \
    status |= (expression); \
    if (rows.error || jash_discovery_effects_flush(&rows)) goto emit; \
} while (0)
    DOCTOR_OBSERVE(doctor_disk(&rows, "disk.root", "/"));
    DOCTOR_OBSERVE(doctor_disk(&rows, "disk.tmpdir", temporary));
    DOCTOR_OBSERVE(doctor_write_probe(&rows, "write.tmpdir", temporary));
    DOCTOR_OBSERVE(doctor_write_probe(&rows, "write.jash_tmp", paths[0]));
    DOCTOR_OBSERVE(doctor_presence(&rows, "exists.modules", paths[1], 1));
    DOCTOR_OBSERVE(doctor_presence(&rows, "exists.groq_key", paths[2], 0));
    DOCTOR_OBSERVE(doctor_gaps(&rows, paths[3]));
#undef DOCTOR_OBSERVE
emit: ;
    int rc = jash_discovery_emit_schema(w, ctx, "doctor", mode, &rows, status ? 1 : 0, "jash.doctor.v1");
    jash_discovery_rows_free(&rows); return rc;
}

/* ── F4: recall — query captured output overflow without re-running ──────── */

static int recall_dir(char* buf, size_t cap) {
    const char* home = getenv("HOME");
    if (!home || !home[0]) return 0;
    snprintf(buf, cap, "%s/.jash/overflow", home);
    return 1;
}

static int recall_list(jash_writer_t w, void* ctx) {
    char dir[512];
    if (!recall_dir(dir, sizeof(dir))) return 1;
    DIR* d = opendir(dir);
    if (!d) { jash_writes(w, ctx, "(no overflow captures)\n"); return 0; }
    struct dirent* e; int n = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1100]; snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st; if (stat(p, &st) != 0) continue;
        jash_writef(w, ctx, "%s  (%lld bytes)\n", e->d_name, (long long)st.st_size);
        n++;
    }
    closedir(d);
    if (!n) jash_writes(w, ctx, "(no overflow captures)\n");
    return 0;
}

/* newest file whose name contains `handle` (or newest overall if NULL/empty) */
static int recall_find(const char* handle, char* out, size_t cap) {
    char dir[512];
    if (!recall_dir(dir, sizeof(dir))) return 0;
    DIR* d = opendir(dir);
    if (!d) return 0;
    struct dirent* e; long best = -1; char bestname[512] = "";
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (handle && handle[0] && !strstr(e->d_name, handle)) continue;
        char p[1100]; snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st; if (stat(p, &st) != 0) continue;
        if ((long)st.st_mtime >= best) {
            best = (long)st.st_mtime;
            snprintf(bestname, sizeof(bestname), "%s", e->d_name);
        }
    }
    closedir(d);
    if (best < 0) return 0;
    snprintf(out, cap, "%s/%s", dir, bestname);
    return 1;
}

#define RECALL_MAX (4 << 20)   /* 4 MB read cap */

static int recall_emit(const char* path, const char* op, int narg,
                       const char* pat, jash_writer_t w, void* ctx) {
    FILE* f = fopen(path, "r");
    if (!f) { jash_writef(w, ctx, "recall: cannot open %s\n", path); return 1; }
    char* buf = (char*)malloc(RECALL_MAX + 1);
    if (!buf) { fclose(f); return 1; }
    size_t total = fread(buf, 1, RECALL_MAX, f);
    fclose(f);
    buf[total] = '\0';

    if (!op || !op[0]) {                       /* whole file */
        ssize_t wr = write(1, buf, total); (void)wr;
        free(buf); return 0;
    }
    if (!strcmp(op, "grep")) {
        char* line = strtok(buf, "\n");
        while (line) {
            if (pat && strstr(line, pat)) jash_writef(w, ctx, "%s\n", line);
            line = strtok(NULL, "\n");
        }
        free(buf); return 0;
    }
    if (!strcmp(op, "head")) {
        int shown = 0; char* line = strtok(buf, "\n");
        while (line && shown < narg) { jash_writef(w, ctx, "%s\n", line); shown++; line = strtok(NULL, "\n"); }
        free(buf); return 0;
    }
    if (!strcmp(op, "tail")) {
        /* count lines, then print the last narg */
        int nlines = 0;
        for (size_t i = 0; i < total; i++) if (buf[i] == '\n') nlines++;
        int start = nlines - narg; if (start < 0) start = 0;
        int idx = 0; char* line = strtok(buf, "\n");
        while (line) { if (idx >= start) jash_writef(w, ctx, "%s\n", line); idx++; line = strtok(NULL, "\n"); }
        free(buf); return 0;
    }
    free(buf);
    jash_writes(w, ctx, "recall: op must be head N | tail N | grep PAT\n");
    return 1;
}

static int recall_cmd(const char* args, jash_writer_t w, void* ctx) {
    /* ADR-0023 E1: chain (command/outcome) facet. `recall cmds|did|history [pat]
     * [--failed] [--last N]` searches the attested chain instead of the captured
     * output store. Peek the first word before the (destructive) tokenizer. */
    if (args) {
        const char* q = args;
        while (*q == ' ') q++;
        size_t kw = 0; while (q[kw] && q[kw] != ' ') kw++;
        if ((kw == 4 && !strncmp(q, "cmds", 4)) ||
            (kw == 3 && !strncmp(q, "did", 3)) ||
            (kw == 7 && !strncmp(q, "history", 7))) {
            const char* rest = q + kw;
            while (*rest == ' ') rest++;
            return jash_recall_chain(w, ctx, rest);
        }
        if (kw == 5 && !strncmp(q, "stats", 5))
            return jash_recall_chain_stats(w, ctx);
        if (kw == 8 && !strncmp(q, "adoption", 8)) {
            const char* rest = q + kw; while (*rest == ' ') rest++;
            return jash_recall_chain_adoption(w, ctx, !strncmp(rest, "--json", 6));
        }
        if (kw == 6 && !strncmp(q, "failed", 6)) {
            /* ADR-0051 W2: forward trailing flags too — `recall failed --json`
             * used to hardcode "--failed" and silently drop --json/--cwd/etc. */
            const char* rest = q + kw; while (*rest == ' ') rest++;
            char forwarded[1024];
            snprintf(forwarded, sizeof forwarded, "--failed %s", rest);
            return jash_recall_chain(w, ctx, forwarded);
        }
        if (kw == 3 && !strncmp(q, "why", 3)) {
            /* ADR-0051 W5: causal chain — last WORKED vs first BROKE + delta */
            const char* rest = q + kw; while (*rest == ' ') rest++;
            extern int jash_recall_why(jash_writer_t, void*, const char*);
            return jash_recall_why(w, ctx, rest);
        }
    }

    char a[1024];
    snprintf(a, sizeof(a), "%s", args ? args : "");
    /* tokenize (max 4) */
    char* tok[4] = {0,0,0,0}; int nt = 0;
    char* p = a;
    while (*p && nt < 4) {
        while (*p == ' ') p++;
        if (!*p) break;
        tok[nt++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }

    if (nt == 0 || (tok[0] && !strcmp(tok[0], "list"))) return recall_list(w, ctx);

    const char* handle = NULL; const char* op = NULL; const char* arg = NULL;
    int ti = 0;
    /* if first token is an op, handle defaults to newest */
    if (tok[0] && (!strcmp(tok[0],"head")||!strcmp(tok[0],"tail")||!strcmp(tok[0],"grep"))) {
        op = tok[0]; arg = tok[1];
    } else {
        handle = tok[0]; ti = 1;
        if (tok[ti] && (!strcmp(tok[ti],"head")||!strcmp(tok[ti],"tail")||!strcmp(tok[ti],"grep"))) {
            op = tok[ti]; arg = tok[ti+1];
        }
    }

    char path[1100];
    if (!recall_find(handle, path, sizeof(path))) {
        jash_writes(w, ctx, "recall: no matching capture (try: recall list)\n");
        return 1;
    }
    int narg = arg ? atoi(arg) : 0;
    const char* pat = (op && !strcmp(op,"grep")) ? arg : NULL;
    return recall_emit(path, op, narg, pat, w, ctx);
}

/* ── F2 (re-scoped): verify — sovereign file-integrity checkpoint ────────── */
/* `verify FILE`        → print blake3 hex (checkpoint)
 * `verify FILE HEX`    → recompute and compare; rc=1 + drift report on mismatch
 * BLAKE3 is vendored in core (c/module/blake3.c) — no external dep. */

#include "verify_native.inc"

static void invoice_recall(const char* args) {
    if (!args) args = "";
    uint64_t n = strlen(args) / 4;
    if (n < 1) n = 1;
    jash_invoice_account_command(args, 1);
    jash_invoice_add_saved(JASH_INVOICE_RECALL, n);
}

/* ── dispatch (registered as `kv`, `doctor`, `recall`, `verify`) ─────────── */

int jash_b_kv(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)s; return kv_cmd(args, w, ctx);
}
int jash_b_doctor(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    jash_argv_t av;
    if (jash_argv_build(args, &av) < 0) return 2;
    int rc = jash_b_doctor_argv(s, w, ctx, av.argc, av.argv);
    jash_argv_free(&av); return rc;
}
int jash_b_recall(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)s; invoice_recall(args); return recall_cmd(args, w, ctx);
}
int jash_b_verify(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    jash_argv_t av;
    if (jash_argv_build(args, &av) < 0) return 2;
    int rc = jash_b_verify_argv(s, w, ctx, av.argc, av.argv);
    jash_argv_free(&av); return rc;
}

#include "kv_native.inc"
