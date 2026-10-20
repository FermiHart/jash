// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash_invoice.c — Token economics implementation
// F E R M I ∞ H A R T

#include "jash_invoice.h"

#include <theme/io.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscalls.h>

#define INVOICE_FILE         "invoice.bin"
#define INVOICE_SESSION_FILE "invoice_session.bin"
#define INVOICE_VERSION      1
#define INVOICE_MAGIC        0x494E564F  // "INVO"

static jash_invoice_stats_t   g_lifetime;
static jash_invoice_session_t g_session;
static char                   g_data_dir[512];
static int                    g_initialized = 0;
static int                    g_observation_only = 0;

static const char* CATEGORY_NAMES[] = {
    "recall", "digest", "state", "guard", "brain", "bypass"
};

static int path_join(char* out, size_t sz, const char* dir, const char* file) {
    int n = snprintf(out, sz, "%s/%s", dir, file);
    if (n < 0 || (size_t)n >= sz) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

static int load_session(void);

static int invoice_close_file(FILE* file, int failed) {
    int saved = failed ? (errno ? errno : EIO) : 0;
    if (fclose(file)) { failed = 1; if (!saved) saved = errno ? errno : EIO; }
    if (failed) { errno = saved; return -1; }
    return 0;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    }
    return 0;
}

static int invoice_store_directory(void) {
    if (mkdir(g_data_dir, 0755) && errno != EEXIST) return -1;
    struct stat st;
    if (stat(g_data_dir, &st) || !S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    return 0;
}

static FILE* invoice_open_read(const char* path, size_t expected) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size != expected) {
        close(fd); errno = EINVAL; return NULL;
    }
    FILE* file = fdopen(fd, "rb");
    if (!file) { int saved = errno; close(fd); errno = saved; }
    return file;
}

static int invoice_initialize(const char* data_dir, int readonly) {
    if (g_initialized) return 0;
    if (data_dir) {
        size_t n = strlen(data_dir);
        if (n >= sizeof(g_data_dir)) { errno = ENAMETOOLONG; return -1; }
        memcpy(g_data_dir, data_dir, n + 1);
    } else {
        const char* home = getenv("HOME");
        if (!home || !home[0]) home = jash_tmpdir();
        int n = snprintf(g_data_dir, sizeof(g_data_dir), "%s/.jash", home);
        if (n < 0 || (size_t)n >= sizeof(g_data_dir)) { errno = ENAMETOOLONG; return -1; }
    }
    if (!readonly && invoice_store_directory()) return -1;
    memset(&g_lifetime, 0, sizeof(g_lifetime));
    memset(&g_session, 0, sizeof(g_session));
    if (jash_invoice_load() && errno != ENOENT) return -1;
    if (load_session() && errno != ENOENT) return -1;
    g_initialized = 1;
    g_observation_only = readonly;
    return 0;
}

int jash_invoice_init(const char* data_dir) { return invoice_initialize(data_dir, 0); }
int jash_invoice_init_readonly(const char* data_dir) { return invoice_initialize(data_dir, 1); }

void jash_invoice_cleanup(void) {
    if (!g_initialized) return;
    if (!g_observation_only) jash_invoice_save();
    g_initialized = 0;
}

int jash_invoice_save(void) {
    if (!g_data_dir[0]) return -1;
    if (invoice_store_directory()) return -1;
    g_observation_only = 0;
    char path[512];
    if (path_join(path, sizeof(path), g_data_dir, INVOICE_FILE)) return -1;
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t magic = INVOICE_MAGIC;
    uint32_t version = INVOICE_VERSION;
    int failed = fwrite(&magic, sizeof(magic), 1, f) != 1 ||
        fwrite(&version, sizeof(version), 1, f) != 1 ||
        fwrite(&g_lifetime, sizeof(g_lifetime), 1, f) != 1;
    return invoice_close_file(f, failed);
}

int jash_invoice_load(void) {
    if (!g_data_dir[0]) return -1;
    char path[512];
    if (path_join(path, sizeof(path), g_data_dir, INVOICE_FILE)) return -1;
    FILE* f = invoice_open_read(path, 2u * sizeof(uint32_t) + sizeof(jash_invoice_stats_t));
    if (!f) return -1;
    uint32_t magic = 0, version = 0;
    jash_invoice_stats_t loaded;
    if (fread(&magic, sizeof(magic), 1, f) != 1 || magic != INVOICE_MAGIC ||
        fread(&version, sizeof(version), 1, f) != 1 || version != INVOICE_VERSION ||
        fread(&loaded, sizeof(loaded), 1, f) != 1 || fgetc(f) != EOF || ferror(f)) {
        errno = EINVAL; return invoice_close_file(f, 1);
    }
    if (invoice_close_file(f, 0)) return -1;
    g_lifetime = loaded;
    return 0;
}

static int save_session(void) {
    if (invoice_store_directory()) return -1;
    g_observation_only = 0;
    char path[512];
    if (path_join(path, sizeof(path), g_data_dir, INVOICE_SESSION_FILE)) return -1;
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    return invoice_close_file(f, fwrite(&g_session, sizeof(g_session), 1, f) != 1);
}

static int load_session(void) {
    char path[512];
    if (path_join(path, sizeof(path), g_data_dir, INVOICE_SESSION_FILE)) return -1;
    FILE* f = invoice_open_read(path, sizeof(jash_invoice_session_t));
    if (!f) return -1;
    jash_invoice_session_t loaded;
    if (fread(&loaded, sizeof(loaded), 1, f) != 1 || fgetc(f) != EOF || ferror(f) ||
        !memchr(loaded.id, 0, sizeof(loaded.id)) || !memchr(loaded.intent, 0, sizeof(loaded.intent)) ||
        (loaded.active != 0 && loaded.active != 1)) {
        errno = EINVAL; return invoice_close_file(f, 1);
    }
    if (invoice_close_file(f, 0)) return -1;
    g_session = loaded;
    return 0;
}

static int ensure_init(void) {
    if (!g_initialized) return jash_invoice_init(NULL);
    if (load_session() && errno != ENOENT) return -1;
    return 0;
}

void jash_invoice_add_saved(jash_invoice_category_t cat, uint64_t tokens) {
    if (ensure_init()) return;
    if (cat < 0 || cat >= JASH_INVOICE_CATEGORY_COUNT) return;
    if (g_session.active) g_session.delta.saved[cat] += tokens;
    else                  g_lifetime.saved[cat] += tokens;
    jash_invoice_save();
    if (g_session.active) save_session();
}

void jash_invoice_add_bypass_waste(uint64_t tokens) {
    if (ensure_init()) return;
    if (g_session.active) g_session.delta.saved[JASH_INVOICE_BYPASS] += tokens;
    else                  g_lifetime.saved[JASH_INVOICE_BYPASS] += tokens;
    jash_invoice_save();
    if (g_session.active) save_session();
}

void jash_invoice_suggestion_offered(void) {
    if (ensure_init()) return;
    if (g_session.active) g_session.delta.suggestions_offered++;
    else                  g_lifetime.suggestions_offered++;
    jash_invoice_save();
    if (g_session.active) save_session();
}

void jash_invoice_suggestion_accepted(void) {
    if (ensure_init()) return;
    if (g_session.active) g_session.delta.suggestions_accepted++;
    else                  g_lifetime.suggestions_accepted++;
    jash_invoice_save();
    if (g_session.active) save_session();
}

void jash_invoice_guard_block(void) {
    if (ensure_init()) return;
    if (g_session.active) g_session.delta.guard_blocks++;
    else                  g_lifetime.guard_blocks++;
    jash_invoice_save();
    if (g_session.active) save_session();
}

void jash_invoice_account_command(const char* cmd, int is_jash_native) {
    if (ensure_init()) return;
    if (!cmd) return;
    // Very rough token estimate: 1 token ~= 4 chars of English text.
    uint64_t naive = strlen(cmd) / 4;
    if (naive < 1) naive = 1;
    uint64_t jash = is_jash_native ? naive / 2 : naive;
    if (g_session.active) {
        g_session.delta.naive_input_tokens += naive;
        g_session.delta.jash_input_tokens += jash;
    } else {
        g_lifetime.naive_input_tokens += naive;
        g_lifetime.jash_input_tokens += jash;
    }
    jash_invoice_save();
    if (g_session.active) save_session();
}

int jash_invoice_begin(const char* intent) {
    if (ensure_init()) return -1;
    if (g_session.active) return -1;
    if (intent && strlen(intent) >= sizeof(g_session.intent)) { errno = E2BIG; return -1; }
    memset(&g_session, 0, sizeof(g_session));
    snprintf(g_session.id, sizeof(g_session.id), "%llu", (unsigned long long)now_ns());
    if (intent) snprintf(g_session.intent, sizeof(g_session.intent), "%s", intent);
    g_session.started_ns = now_ns();
    g_session.active = 1;
    return save_session();
}

int jash_invoice_close(jash_invoice_stats_t* out) {
    if (ensure_init()) return -1;
    if (!g_session.active) return -1;
    jash_invoice_stats_t next = g_lifetime;
    for (int i = 0; i < JASH_INVOICE_CATEGORY_COUNT; i++) {
        if (UINT64_MAX - next.saved[i] < g_session.delta.saved[i]) { errno = EOVERFLOW; return -1; }
        next.saved[i] += g_session.delta.saved[i];
    }
#define INVOICE_ADD_CHECKED(field) do { \
    if (UINT64_MAX - next.field < g_session.delta.field) { errno = EOVERFLOW; return -1; } \
    next.field += g_session.delta.field; \
} while (0)
    INVOICE_ADD_CHECKED(commands_predicted);
    INVOICE_ADD_CHECKED(suggestions_offered);
    INVOICE_ADD_CHECKED(suggestions_accepted);
    INVOICE_ADD_CHECKED(guard_blocks);
    INVOICE_ADD_CHECKED(naive_input_tokens);
    INVOICE_ADD_CHECKED(jash_input_tokens);
#undef INVOICE_ADD_CHECKED
    g_session.closed_ns = now_ns();
    g_session.active = 0;
    g_lifetime = next;
    if (save_session() || jash_invoice_save()) return -1;
    if (out) memcpy(out, &g_session.delta, sizeof(*out));
    return 0;
}

int jash_invoice_session_active(void) {
    ensure_init();
    return g_session.active;
}

const jash_invoice_session_t* jash_invoice_current(void) {
    ensure_init();
    return &g_session;
}

const jash_invoice_stats_t* jash_invoice_lifetime(void) {
    ensure_init();
    return &g_lifetime;
}

void jash_invoice_print_status(jash_writer_t w, void* ctx) {
    if (!w) return;
    const jash_invoice_stats_t* s = jash_invoice_lifetime();
    jash_writes(w, ctx, "token economics (lifetime):\n");
    uint64_t total_saved = 0;
    for (int i = 0; i < JASH_INVOICE_CATEGORY_COUNT; i++) {
        if (s->saved[i] > 0) {
            jash_writef(w, ctx, "  %-8s %llu tokens\n", CATEGORY_NAMES[i], (unsigned long long)s->saved[i]);
            total_saved += s->saved[i];
        }
    }
    jash_writef(w, ctx, "  total saved:  %llu tokens\n", (unsigned long long)total_saved);
    jash_writef(w, ctx, "  naive input:  %llu tokens\n", (unsigned long long)s->naive_input_tokens);
    jash_writef(w, ctx, "  jash input:   %llu tokens\n", (unsigned long long)s->jash_input_tokens);
    jash_writef(w, ctx, "  suggestions:  %llu offered / %llu accepted\n",
                (unsigned long long)s->suggestions_offered,
                (unsigned long long)s->suggestions_accepted);
    jash_writef(w, ctx, "  guard blocks: %llu\n", (unsigned long long)s->guard_blocks);
}

void jash_invoice_print_session(jash_writer_t w, void* ctx,
                                const jash_invoice_session_t* s) {
    if (!w || !s) return;
    jash_writef(w, ctx, "invoice %s: %s\n", s->id, s->intent[0] ? s->intent : "(no intent)");
    const jash_invoice_stats_t* d = &s->delta;
    uint64_t total_saved = 0;
    for (int i = 0; i < JASH_INVOICE_CATEGORY_COUNT; i++) {
        if (d->saved[i] > 0) {
            jash_writef(w, ctx, "  %-8s %llu tokens\n", CATEGORY_NAMES[i], (unsigned long long)d->saved[i]);
            total_saved += d->saved[i];
        }
    }
    jash_writef(w, ctx, "  total saved:  %llu tokens\n", (unsigned long long)total_saved);
    jash_writef(w, ctx, "  naive input:  %llu tokens\n", (unsigned long long)d->naive_input_tokens);
    jash_writef(w, ctx, "  jash input:   %llu tokens\n", (unsigned long long)d->jash_input_tokens);
    jash_writef(w, ctx, "  suggestions:  %llu offered / %llu accepted\n",
                (unsigned long long)d->suggestions_offered,
                (unsigned long long)d->suggestions_accepted);
    uint64_t end_ns = s->active ? now_ns() : s->closed_ns;
    uint64_t dur_ms = (end_ns - s->started_ns) / 1000000ULL;
    jash_writef(w, ctx, "  duration:     %llu ms\n", (unsigned long long)dur_ms);
}
