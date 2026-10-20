#define _GNU_SOURCE
// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// ADR-0003 §M1: .modjash loader implementation
//
// M1 strategy: validate header + manifest, then dlopen the body.
// The .modjash file is [header][manifest][body.dylib/so][optional sig].
// Custom mmap-without-dlopen (~140µs) replaces this in a future wave.

#include <module/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>
#include <ed25519.h>   /* Ed25519 signature verification for JMOD_FLAG_SIGNED */
#include <module/blake3.h>
#if defined(__linux__) && !defined(JASH_BLIBC)
#include <linux/memfd.h>
#endif

jmod_table_t g_modules = {0};

#ifdef JASH_LINKED_SIGIL
/* INV-PERF10-SIGIL-IMAGE-001: these symbols are build inputs in the admitted
 * executable, never pointers or code supplied by a mutable module file. */
extern jmod_ops_t jash_linked_sigil_ops;
extern const char jash_linked_sigil_manifest[];
extern const uint64_t jash_linked_sigil_manifest_size;
#define JMOD_LINKED_SIGIL_PATH "image:sigil"
static int jmod_linked_exit_registered;

static int jmod_resolve_linked(const char* name, char* out, size_t capacity)
{
    if (!name || strcmp(name, "sigil") != 0) return 0;
    if (!out || capacity < sizeof(JMOD_LINKED_SIGIL_PATH)) {
        if (out && capacity) out[0] = '\0';
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(out, JMOD_LINKED_SIGIL_PATH, sizeof(JMOD_LINKED_SIGIL_PATH));
    return 1;
}

static void jmod_linked_exit(void)
{
    jmod_entry_t* e = jmod_find("sigil");
    if (e && e->ops == &jash_linked_sigil_ops) (void)jmod_unload("sigil");
}
#endif

/* Trusted Ed25519 public key for module signature verification */
uint8_t jash_module_trusted_pubkey[32] = {0};

enum {
    JMOD_KEY_MISSING = 0,
    JMOD_KEY_READY,
    JMOD_KEY_MALFORMED,
    JMOD_KEY_INSECURE,
    JMOD_KEY_SYMLINK,
};
static int jash_module_trusted_pubkey_status = JMOD_KEY_MISSING;
static int jash_module_security_initialized;
static int jash_module_require_signature;
static int jash_module_require_hash;
static int jash_module_caps_strict;
static int jash_module_trust_home_pinned;
static char jash_module_trust_home[PATH_MAX];
static char jash_module_dev_root[PATH_MAX];
static char* jash_module_policy;
static size_t jash_module_policy_len;
static int jash_module_policy_invalid;

#define JMOD_POLICY_MAX (64u * 1024u)

static const char* jash_module_pin_trust_home(void)
{
    if (jash_module_trust_home_pinned)
        return jash_module_trust_home[0] ? jash_module_trust_home : NULL;

    const char* home = getenv("HOME");
    if (home && home[0]) {
        if (!realpath(home, jash_module_trust_home)) {
            if (home[0] == '/') {
                size_t home_len = strlen(home);
                if (home_len < sizeof jash_module_trust_home)
                    memcpy(jash_module_trust_home, home, home_len + 1);
            } else {
                char cwd[PATH_MAX];
                if (getcwd(cwd, sizeof cwd) &&
                    snprintf(jash_module_trust_home,
                             sizeof jash_module_trust_home, "%s/%s", cwd,
                             home) >= (int)sizeof jash_module_trust_home)
                    jash_module_trust_home[0] = '\0';
            }
        }
    }
    jash_module_trust_home_pinned = 1;
    return jash_module_trust_home[0] ? jash_module_trust_home : NULL;
}

/* Load the trusted Ed25519 public key from ~/.jash/keys/module_signing_pubkey
 * into process-owned memory. Signed loads call this themselves so verification
 * cannot silently use the zero-initialized global. */
static void jash_module_policy_load(const char* home)
{
    if (!home) return;
    char path[PATH_MAX], jash_dir[PATH_MAX], modules_dir[PATH_MAX];
    int jash_len = snprintf(jash_dir, sizeof jash_dir, "%s/.jash", home);
    int modules_len = jash_len < 0 ? -1 :
        snprintf(modules_dir, sizeof modules_dir, "%s/modules", jash_dir);
    int path_len = modules_len < 0 ? -1 :
        snprintf(path, sizeof path, "%s/policy.conf", modules_dir);
    if (jash_len < 0 || jash_len >= (int)sizeof jash_dir ||
        modules_len < 0 || modules_len >= (int)sizeof modules_dir ||
        path_len < 0 || path_len >= (int)sizeof path)
        return;

    struct stat dir, policy_entry;
    if (lstat(path, &policy_entry) != 0) {
        if (errno != ENOENT) jash_module_policy_invalid = 1;
        return;
    }
    jash_module_policy_invalid = 1;
    if (lstat(jash_dir, &dir) != 0 || !S_ISDIR(dir.st_mode) ||
        dir.st_uid != geteuid() || (dir.st_mode & 022) != 0 ||
        lstat(modules_dir, &dir) != 0 || !S_ISDIR(dir.st_mode) ||
        dir.st_uid != geteuid() || (dir.st_mode & 022) != 0)
        return;

    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 022) != 0 || st.st_size < 0 ||
        (uintmax_t)st.st_size > JMOD_POLICY_MAX) {
        close(fd);
        return;
    }
    size_t len = (size_t)st.st_size;
    char* policy = malloc(len + 1);
    if (!policy) {
        close(fd);
        return;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, policy + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        off += (size_t)n;
    }
    close(fd);
    if (off != len) {
        free(policy);
        return;
    }
    policy[len] = '\0';
    jash_module_policy = policy;
    jash_module_policy_len = len;
    jash_module_policy_invalid = 0;
}

void jash_module_security_init(void)
{
    if (jash_module_security_initialized) return;
    jash_module_security_initialized = 1;
    const char* value = getenv("JASH_MODULE_REQUIRE_SIGNATURE");
    jash_module_require_signature = value && value[0] && value[0] != '0';
    value = getenv("JASH_MODULE_REQUIRE_HASH");
    jash_module_require_hash = value && value[0] && value[0] != '0';
    value = getenv("JASH_MODULE_CAPS_STRICT");
    jash_module_caps_strict = value && value[0] && value[0] != '0';
    value = getenv("JASH_MODULE_DEV_ROOT");
    if (value && value[0] == '/' && !realpath(value, jash_module_dev_root))
        jash_module_dev_root[0] = '\0';
    memset(jash_module_trusted_pubkey, 0, sizeof(jash_module_trusted_pubkey));
    jash_module_trusted_pubkey_status = JMOD_KEY_MISSING;

    const char* home = jash_module_pin_trust_home();
    jash_module_policy_load(home);
    if (!home) return;

    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/.jash/keys/module_signing_pubkey", home) >=
        (int)sizeof(path)) {
        jash_module_trusted_pubkey_status = JMOD_KEY_MALFORMED;
        return;
    }

    struct stat lst;
    if (lstat(path, &lst) != 0) return;
    if (S_ISLNK(lst.st_mode)) {
        jash_module_trusted_pubkey_status = JMOD_KEY_SYMLINK;
        return;
    }

    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ELOOP) jash_module_trusted_pubkey_status = JMOD_KEY_SYMLINK;
        return;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 0077) != 0) {
        jash_module_trusted_pubkey_status = JMOD_KEY_INSECURE;
        close(fd);
        return;
    }
    if (st.st_size != (off_t)sizeof(jash_module_trusted_pubkey)) {
        jash_module_trusted_pubkey_status = JMOD_KEY_MALFORMED;
        close(fd);
        return;
    }

    size_t off = 0;
    while (off < sizeof(jash_module_trusted_pubkey)) {
        ssize_t n = read(fd, jash_module_trusted_pubkey + off,
                         sizeof(jash_module_trusted_pubkey) - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        off += (size_t)n;
    }
    close(fd);

    int nonzero = 0;
    for (size_t i = 0; i < sizeof(jash_module_trusted_pubkey); i++)
        nonzero |= jash_module_trusted_pubkey[i];
    if (off != sizeof(jash_module_trusted_pubkey) || !nonzero) {
        memset(jash_module_trusted_pubkey, 0, sizeof(jash_module_trusted_pubkey));
        jash_module_trusted_pubkey_status = JMOD_KEY_MALFORMED;
        return;
    }
    jash_module_trusted_pubkey_status = JMOD_KEY_READY;
}

void jash_module_trusted_pubkey_load(void)
{
    jash_module_security_init();
}

/* ── Manifest parser ───────────────────────────────────────────────────── */

int jmod_parse_manifest(const char* manifest, size_t manifest_len,
                        char* out_name, size_t name_cap,
                        char* out_version, size_t ver_cap,
                        char* out_description, size_t desc_cap,
                        uint32_t* out_caps)
{
    if (!manifest || manifest_len == 0) return -1;
    if (out_name)        out_name[0] = '\0';
    if (out_version)     out_version[0] = '\0';
    if (out_description) out_description[0] = '\0';
    if (out_caps)        *out_caps = 0;

    const char* p = manifest;
    const char* end = manifest + manifest_len;

    while (p < end) {
        const char* line_end = p;
        while (line_end < end && *line_end != '\n') line_end++;

        /* find '=' */
        const char* eq = p;
        while (eq < line_end && *eq != '=') eq++;
        if (eq >= line_end) { p = line_end + 1; continue; }

        size_t klen = (size_t)(eq - p);
        const char* val = eq + 1;
        size_t vlen = (size_t)(line_end - val);

        /* trim trailing \r */
        while (vlen > 0 && val[vlen-1] == '\r') vlen--;

#define MATCH(key) (klen == sizeof(key)-1 && memcmp(p, key, klen) == 0)
        if (MATCH("name") && out_name) {
            /* SECURITY: the manifest name flows into filesystem paths
             * (~/.jash/modules/<name>.modjash, cache/jmod_<name>_...) and
             * into policy.conf matching — reject anything outside
             * [A-Za-z0-9._-] so it can't traverse or collide. */
            if (vlen == 0 || vlen > 32 || vlen >= name_cap || val[0] == '.')
                return -1;
            for (size_t i = 0; i < vlen; i++) {
                char c = val[i];
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
                    return -1;
            }
            memcpy(out_name, val, vlen); out_name[vlen] = '\0';
        } else if (MATCH("version") && out_version) {
            size_t n = vlen < ver_cap - 1 ? vlen : ver_cap - 1;
            memcpy(out_version, val, n); out_version[n] = '\0';
        } else if (MATCH("description") && out_description) {
            size_t n = vlen < desc_cap - 1 ? vlen : desc_cap - 1;
            memcpy(out_description, val, n); out_description[n] = '\0';
        } else if (MATCH("caps") && out_caps) {
            *out_caps = (uint32_t)strtoul(val, NULL, 0);
        }
#undef MATCH
        p = line_end + 1;
    }
    return 0;
}

/* ── Path resolution ───────────────────────────────────────────────────── */

static int jmod_valid_short_name(const char* name)
{
    if (!name || !*name || name[0] == '.' || strlen(name) > 32) return 0;
    for (const char* p = name; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

static int jmod_resolve_home(const char* home, const char* name,
                             char* out_path, size_t out_cap)
{
    if (!out_path || out_cap == 0) return 0;
    out_path[0] = '\0';
    if (!home || !*home || !jmod_valid_short_name(name)) return 0;
    int n = snprintf(out_path, out_cap, "%s/.jash/modules/%s.modjash", home, name);
    if (n < 0 || (size_t)n >= out_cap) {
        out_path[0] = '\0';
        errno = ENAMETOOLONG;
        return 0;
    }
    if (access(out_path, R_OK) == 0) return 1;
    out_path[0] = '\0';
    return 0;
}

int jmod_resolve_installed(const char* name, char* out_path, size_t out_cap)
{
    jash_module_security_init();
#ifdef JASH_LINKED_SIGIL
    int linked = jmod_resolve_linked(name, out_path, out_cap);
    if (linked) return linked > 0;
#endif
    if (jmod_resolve_home(jash_module_trust_home, name, out_path, out_cap))
        return 1;
    if (!jash_module_dev_root[0]) return 0;

    int n = snprintf(out_path, out_cap, "%s/%s/%s.modjash",
                     jash_module_dev_root, name, name);
    if (n >= 0 && (size_t)n < out_cap && access(out_path, R_OK) == 0) return 1;
    n = snprintf(out_path, out_cap, "%s/%s.modjash", jash_module_dev_root, name);
    if (n >= 0 && (size_t)n < out_cap && access(out_path, R_OK) == 0) return 1;
    out_path[0] = '\0';
    return 0;
}

int jmod_resolve(const char* name, char* out_path, size_t out_cap)
{
    if (!out_path || out_cap == 0) return 0;
    out_path[0] = '\0';
    if (!jmod_valid_short_name(name)) return 0;

#ifdef JASH_LINKED_SIGIL
    int linked = jmod_resolve_linked(name, out_path, out_cap);
    if (linked) return linked > 0;
#endif
    const char* home = getenv("HOME");
    if (jmod_resolve_home(home, name, out_path, out_cap)) return 1;

    int n = snprintf(out_path, out_cap, "./modules/%s/%s.modjash", name, name);
    if (n < 0 || (size_t)n >= out_cap) goto too_long;
    if (access(out_path, R_OK) == 0) return 1;

    n = snprintf(out_path, out_cap, "./modules/%s.modjash", name);
    if (n < 0 || (size_t)n >= out_cap) goto too_long;
    if (access(out_path, R_OK) == 0) return 1;

    out_path[0] = '\0';
    return 0;

too_long:
    out_path[0] = '\0';
    errno = ENAMETOOLONG;
    return 0;
}

/* ── Loader ────────────────────────────────────────────────────────────── */


/* ── JINV-025: politica de concessao de capacidades ─────────────────────── */

static const struct { const char* name; uint32_t bit; } JMOD_CAP_NAMES[] = {
    { "fs_read",       JMOD_CAP_FS_READ        },
    { "fs_write",      JMOD_CAP_FS_WRITE       },
    { "net",           JMOD_CAP_NET            },
    { "exec",          JMOD_CAP_EXEC           },
    { "state_read",    JMOD_CAP_STATE_READ     },
    { "state_write",   JMOD_CAP_STATE_WRITE    },
    { "tty",           JMOD_CAP_TTY            },
    { "crypto",        JMOD_CAP_CRYPTO         },
    { "module_load",   JMOD_CAP_MODULE_LOAD    },
    { "hook_pre_cmd",  JMOD_CAP_HOOK_PRE_CMD   },
    { "hook_post_cmd", JMOD_CAP_HOOK_POST_CMD  },
    { "hook_exit",     JMOD_CAP_HOOK_EXIT      },
    { "hook_mutation", JMOD_CAP_HOOK_MUTATION  },
};
#define JMOD_CAP_NAMES_N ((int)(sizeof(JMOD_CAP_NAMES)/sizeof(JMOD_CAP_NAMES[0])))

static uint32_t jmod_cap_bit(const char* tok, size_t len) {
    for (int i = 0; i < JMOD_CAP_NAMES_N; i++) {
        const char* n = JMOD_CAP_NAMES[i].name;
        if (strlen(n) == len && strncmp(n, tok, len) == 0)
            return JMOD_CAP_NAMES[i].bit;
    }
    return 0;
}

uint32_t jmod_policy_grant(const char* name, uint32_t caps_declared)
{
    jash_module_security_init();

    /* Base: NUNCA mais do que o modulo declarou. Conceder 0xFFFFFFFF dava a
     * qualquer modulo poderes que ele nem pediu, e tornava o manifesto um
     * documento decorativo. */
    uint32_t grant = jash_module_caps_strict ? 0u : caps_declared;
    if (!name || !jash_module_policy) return grant;

    size_t nlen = strlen(name);
    const char* cursor = jash_module_policy;
    const char* end = jash_module_policy + jash_module_policy_len;
    while (cursor < end) {
        const char* line_end = memchr(cursor, '\n', (size_t)(end - cursor));
        if (!line_end) line_end = end;
        size_t line_len = (size_t)(line_end - cursor);
        if (line_len >= 512) {
            cursor = line_end < end ? line_end + 1 : end;
            continue;
        }
        char line[512];
        memcpy(line, cursor, line_len);
        line[line_len] = '\0';
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || !*p || strncmp(p, name, nlen) != 0) {
            cursor = line_end < end ? line_end + 1 : end;
            continue;
        }
        char* q = p + nlen;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != ':') {
            cursor = line_end < end ? line_end + 1 : end;
            continue;
        }
        q++;
        /* tokens: +cap concede, -cap nega */
        while (*q) {
            while (*q == ' ' || *q == '\t' || *q == '\n') q++;
            if (!*q) break;
            int add = (*q == '+'), sub = (*q == '-');
            if (add || sub) q++;
            const char* tok = q;
            while (*q && *q != ' ' && *q != '\t' && *q != '\n') q++;
            uint32_t bit = jmod_cap_bit(tok, (size_t)(q - tok));
            if (!bit) continue;
            if (sub)      grant &= ~bit;
            else if (add) grant |= (bit & caps_declared);  /* nunca alem do declarado */
        }
        break;
    }
    return grant;
}

static int jmod_cache_fd_matches(int fd, uint64_t body_len,
                                 const uint8_t expected[32])
{
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 0222) != 0 ||
        (uint64_t)st.st_size != body_len || lseek(fd, 0, SEEK_SET) < 0)
        return 0;

    struct blake3 hasher;
    uint8_t buf[8192], got[32];
    uint64_t remaining = body_len;
    blake3_init(&hasher);
    while (remaining > 0) {
        size_t want = remaining < sizeof(buf) ? (size_t)remaining : sizeof(buf);
        ssize_t n = read(fd, buf, want);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        blake3_update(&hasher, buf, (uint64_t)n);
        remaining -= (uint64_t)n;
    }
    blake3_final(&hasher, got, sizeof(got));
    if (lseek(fd, 0, SEEK_SET) < 0) return 0;
    return memcmp(got, expected, sizeof(got)) == 0;
}

static int jmod_write_all(int fd, const void* data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, (const char*)data + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static void jmod_cache_event(const char* event, const char* name)
{
    const char* enabled = getenv("JASH_MODULE_CACHE_EVENTS");
    if (enabled && strcmp(enabled, "1") == 0)
        fprintf(stderr, "jash: module: cache event=%s module=%s\n", event, name);
}

static int jmod_open_owned_dir_at(int parent_fd, const char* name, int create)
{
    if (create && mkdirat(parent_fd, name, 0700) != 0 && errno != EEXIST)
        return -1;
    int fd = openat(parent_fd, name,
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid()) {
        close(fd);
        errno = EPERM;
        return -1;
    }
    return fd;
}

/* The cache is never execution authority. Existing entries are validated but
 * never truncated or removed; new entries are fully written and validated
 * under a unique name, then atomically linked into place without replacement. */
static void jmod_cache_store(const char* home, const char* name,
                             const char* suffix, const void* body,
                             size_t body_len, const uint8_t body_hash[32])
{
    int home_fd = -1, jash_fd = -1, cache_dir_fd = -1, tmp_fd = -1;
    char final_name[128], tmp_name[128] = "";
    char hash_hex[17];
    static unsigned long serial;
    static const char hexd[] = "0123456789abcdef";

    for (int i = 0; i < 8; i++) {
        hash_hex[i * 2] = hexd[(body_hash[i] >> 4) & 0xf];
        hash_hex[i * 2 + 1] = hexd[body_hash[i] & 0xf];
    }
    hash_hex[16] = '\0';
    int final_len = snprintf(final_name, sizeof(final_name),
                             "jmod_%s_body_%s%s", name, hash_hex, suffix);
    if (!home || !*home || final_len < 0 || (size_t)final_len >= sizeof(final_name))
        goto unavailable;

    home_fd = open(home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (home_fd < 0) goto unavailable;
    jash_fd = jmod_open_owned_dir_at(home_fd, ".jash", 1);
    if (jash_fd < 0) goto unavailable;
    cache_dir_fd = jmod_open_owned_dir_at(jash_fd, "cache", 1);
    if (cache_dir_fd < 0) goto unavailable;

    int existing = openat(cache_dir_fd, final_name,
                          O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (existing >= 0) {
        int valid = jmod_cache_fd_matches(existing, (uint64_t)body_len, body_hash);
        close(existing);
        jmod_cache_event(valid ? "hit" : "invalid-preserved", name);
        goto out;
    }
    if (errno != ENOENT) goto unavailable;

    for (int attempt = 0; attempt < 64; attempt++) {
        int n = snprintf(tmp_name, sizeof(tmp_name), ".jmod-%ld-%lu.tmp",
                         (long)getpid(), ++serial);
        if (n < 0 || (size_t)n >= sizeof(tmp_name)) goto unavailable;
        tmp_fd = openat(cache_dir_fd, tmp_name,
                        O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                        0600);
        if (tmp_fd >= 0) break;
        if (errno != EEXIST) goto unavailable;
        tmp_name[0] = '\0';
    }
    if (tmp_fd < 0) goto unavailable;
    if (jmod_write_all(tmp_fd, body, body_len) != 0 || fsync(tmp_fd) != 0 ||
        fchmod(tmp_fd, 0500) != 0 ||
        !jmod_cache_fd_matches(tmp_fd, (uint64_t)body_len, body_hash))
        goto unavailable;

    if (linkat(cache_dir_fd, tmp_name, cache_dir_fd, final_name, 0) == 0) {
        (void)fsync(cache_dir_fd);
        jmod_cache_event("stored", name);
    } else if (errno == EEXIST) {
        int winner = openat(cache_dir_fd, final_name,
                            O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        int valid = winner >= 0 &&
            jmod_cache_fd_matches(winner, (uint64_t)body_len, body_hash);
        if (winner >= 0) close(winner);
        jmod_cache_event(valid ? "race-hit" : "invalid-preserved", name);
    } else {
        goto unavailable;
    }
    goto out;

unavailable:
    jmod_cache_event("unavailable", name);
out:
    if (tmp_fd >= 0) close(tmp_fd);
    if (tmp_name[0] && cache_dir_fd >= 0)
        (void)unlinkat(cache_dir_fd, tmp_name, 0);
    if (cache_dir_fd >= 0) close(cache_dir_fd);
    if (jash_fd >= 0) close(jash_fd);
    if (home_fd >= 0) close(home_fd);
}

/* Linux memfd seals provide a genuinely immutable anonymous execution image.
 * macOS has no equivalent seal for a dlopen-compatible descriptor: another
 * same-UID process can retain a writable descriptor opened before unlink.
 * Refuse security-claimed loads there, while preserving the legacy unsigned,
 * zero-hash behavior without describing it as verified. */
static int jmod_exec_fd_create(const void* body, size_t body_len,
                               const char* private_dir, int require_immutable)
{
#if defined(__linux__) && !defined(JASH_BETHOS)
    (void)private_dir;
    (void)require_immutable;
    int fd = memfd_create("jash-module", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    if (jmod_write_all(fd, body, body_len) != 0 ||
        fchmod(fd, 0500) != 0 || lseek(fd, 0, SEEK_SET) < 0 ||
        fcntl(fd, F_ADD_SEALS,
              F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    struct stat st;
    int seals = fcntl(fd, F_GET_SEALS);
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 0222) != 0 ||
        (uint64_t)st.st_size != (uint64_t)body_len ||
        seals < 0 || (seals & (F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK)) !=
                     (F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK)) {
        close(fd);
        errno = EPERM;
        return -1;
    }
    return fd;
#elif defined(__APPLE__)
    if (require_immutable) {
        errno = ENOTSUP;
        return -1;
    }
    char path[600];
    if (snprintf(path, sizeof(path), "%s/.jmod-exec-XXXXXX", private_dir) >=
        (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int writable = mkstemp(path);
    if (writable < 0) return -1;
    int readonly = -1;
    if (fchmod(writable, 0600) == 0 &&
        jmod_write_all(writable, body, body_len) == 0 &&
        fsync(writable) == 0 && fchmod(writable, 0500) == 0)
        readonly = open(path, O_RDONLY | O_NOFOLLOW);

    struct stat written, opened;
    int valid = readonly >= 0 && fstat(writable, &written) == 0 &&
        fstat(readonly, &opened) == 0 && S_ISREG(opened.st_mode) &&
        written.st_dev == opened.st_dev && written.st_ino == opened.st_ino &&
        opened.st_uid == geteuid() && (opened.st_mode & 0222) == 0 &&
        (uint64_t)opened.st_size == (uint64_t)body_len;
    int saved = valid ? 0 : errno;
    unlink(path);
    close(writable);
    if (!valid) {
        if (readonly >= 0) close(readonly);
        errno = saved ? saved : EPERM;
        return -1;
    }
    return readonly;
#else
    (void)body;
    (void)body_len;
    (void)private_dir;
    (void)require_immutable;
    errno = ENOTSUP;
    return -1;
#endif
}

static int jmod_read_snapshot(int fd, void* out, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = pread(fd, (char*)out + off, len - off, (off_t)off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static int jmod_checked_layout(const jmod_header_t* hdr, size_t source_len,
                               size_t* body_off, size_t* sections_end)
{
    const size_t manifest_off = sizeof(jmod_header_t);
    if (source_len < manifest_off ||
        (size_t)hdr->manifest_len > source_len - manifest_off)
        return -1;

    const size_t checked_body_off = manifest_off + (size_t)hdr->manifest_len;
    if ((size_t)hdr->body_len > source_len - checked_body_off)
        return -1;

    *body_off = checked_body_off;
    *sections_end = checked_body_off + (size_t)hdr->body_len;
    return 0;
}

static int jmod_automatic_source_trusted(const char* path,
                                         const char* expected_name,
                                         const struct stat* source)
{
    const char* home = jash_module_pin_trust_home();
    if (!expected_name || !source || source->st_uid != geteuid() ||
        source->st_nlink != 1 || (source->st_mode & 022) != 0)
        return 0;

    char expected[PATH_MAX], jash_dir[PATH_MAX], modules_dir[PATH_MAX];
    struct stat dir;
    int expected_len = home ?
        snprintf(expected, sizeof expected, "%s/.jash/modules/%s.modjash",
                 home, expected_name) : -1;
    int jash_len = home ?
        snprintf(jash_dir, sizeof jash_dir, "%s/.jash", home) : -1;
    int modules_len = jash_len >= 0 ?
        snprintf(modules_dir, sizeof modules_dir, "%s/modules", jash_dir) : -1;
    if (expected_len >= 0 && expected_len < (int)sizeof expected &&
        jash_len >= 0 && jash_len < (int)sizeof jash_dir &&
        modules_len >= 0 && modules_len < (int)sizeof modules_dir &&
        strcmp(path, expected) == 0)
        return lstat(jash_dir, &dir) == 0 && S_ISDIR(dir.st_mode) &&
               dir.st_uid == geteuid() && (dir.st_mode & 022) == 0 &&
               lstat(modules_dir, &dir) == 0 && S_ISDIR(dir.st_mode) &&
               dir.st_uid == geteuid() && (dir.st_mode & 022) == 0;

    if (!jash_module_dev_root[0] ||
        lstat(jash_module_dev_root, &dir) != 0 || !S_ISDIR(dir.st_mode) ||
        dir.st_uid != geteuid() || (dir.st_mode & 022) != 0)
        return 0;

    char dev_nested[PATH_MAX], dev_flat[PATH_MAX], dev_module_dir[PATH_MAX];
    int nested_len = snprintf(dev_nested, sizeof dev_nested, "%s/%s/%s.modjash",
                              jash_module_dev_root, expected_name, expected_name);
    int flat_len = snprintf(dev_flat, sizeof dev_flat, "%s/%s.modjash",
                            jash_module_dev_root, expected_name);
    if (nested_len >= 0 && nested_len < (int)sizeof dev_nested &&
        strcmp(path, dev_nested) == 0) {
        int dir_len = snprintf(dev_module_dir, sizeof dev_module_dir, "%s/%s",
                               jash_module_dev_root, expected_name);
        return dir_len >= 0 && dir_len < (int)sizeof dev_module_dir &&
               lstat(dev_module_dir, &dir) == 0 && S_ISDIR(dir.st_mode) &&
               dir.st_uid == geteuid() && (dir.st_mode & 022) == 0;
    }
    return flat_len >= 0 && flat_len < (int)sizeof dev_flat &&
           strcmp(path, dev_flat) == 0;
}

#ifdef JASH_LINKED_SIGIL
/* This is an image-bound provider, not a .modjash envelope. Keep explicit
 * envelope requirements fail-closed rather than relabelling an ELF receipt. */
static int jmod_load_linked_sigil(uint32_t granted_caps, const char* expected)
{
    if (jash_module_require_signature || jash_module_require_hash) {
        fprintf(stderr, "jash: module: image:sigil has no separate module envelope; requested envelope policy refuses\n");
        errno = ENOTSUP;
        return -1;
    }
    char name[64] = {0}, version[32] = {0}, description[256] = {0};
    uint32_t declared = 0;
    jmod_ops_t* ops = &jash_linked_sigil_ops;
    if (!jash_linked_sigil_manifest_size || jash_linked_sigil_manifest_size > 4096 ||
        jmod_parse_manifest(jash_linked_sigil_manifest,
            (size_t)jash_linked_sigil_manifest_size, name, sizeof(name),
            version, sizeof(version), description, sizeof(description), &declared) != 0 ||
        strcmp(name, "sigil") != 0 || (expected && strcmp(name, expected) != 0) ||
        !ops->name || strcmp(ops->name, name) != 0 || !ops->version ||
        strcmp(ops->version, version) != 0 || !ops->init || !ops->dispatch ||
        (ops->caps_used & ~declared)) {
        fprintf(stderr, "jash: module: image:sigil manifest/ops identity refused\n");
        errno = EINVAL;
        return -1;
    }
    if (jmod_find(name)) { errno = EEXIST; return -1; }
    if (jash_module_policy_invalid) {
        fprintf(stderr, "jash: module: policy exists but is not owner-controlled; refusing image:sigil\n");
        errno = EPERM;
        return -1;
    }
    granted_caps &= jmod_policy_grant(name, declared);
    if (declared & ~granted_caps) {
        fprintf(stderr, "jash: module: image:sigil pede caps 0x%08x, faltando 0x%08x; inspect ~/.jash/modules/policy.conf\n",
                declared, declared & ~granted_caps);
        errno = EPERM;
        return -1;
    }
    if (!jmod_linked_exit_registered) {
        if (atexit(jmod_linked_exit) != 0) return -1;
        jmod_linked_exit_registered = 1;
    }
    if (ops->init() != 0) {
        fprintf(stderr, "jash: module: image:sigil init failed\n");
        return -1;
    }
    jmod_entry_t entry = {0};
    strlcpy(entry.name, name, sizeof(entry.name));
    strlcpy(entry.path, JMOD_LINKED_SIGIL_PATH, sizeof(entry.path));
    strlcpy(entry.version, version, sizeof(entry.version));
    strlcpy(entry.description, description, sizeof(entry.description));
    entry.caps_declared = declared;
    entry.caps_used = ops->caps_used;
    entry.ops = ops;
    entry.exec_fd = -1;
    entry.active = 1;
    g_modules.entries[g_modules.count++] = entry;
    return 0;
}
#endif

static int jmod_load_impl(const char* path, uint32_t granted_caps,
                          const char* expected_name)
{
    jash_module_security_init();
    if (!path || strlen(path) >= JMOD_PATH_CAP) {
        fprintf(stderr, "jash: module: path too long (maximum %d bytes)\n",
                JMOD_PATH_CAP - 1);
        return -1;
    }
    if (g_modules.count >= JMOD_MAX_MODULES) {
        fprintf(stderr, "jash: module: table full (%d max)\n", JMOD_MAX_MODULES);
        return -1;
    }
#ifdef JASH_LINKED_SIGIL
    if (strcmp(path, JMOD_LINKED_SIGIL_PATH) == 0)
        return jmod_load_linked_sigil(granted_caps, expected_name);
#endif

    /* Copy the complete source into process-owned memory before parsing. A
     * MAP_PRIVATE view can still reflect writes to clean pages of its backing
     * inode, so it is not a stable verification snapshot. */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        fprintf(stderr, "jash: module: cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
        st.st_size < (off_t)sizeof(jmod_header_t) ||
        (uintmax_t)st.st_size > (uintmax_t)SIZE_MAX) {
        fprintf(stderr, "jash: module: '%s' too small to be a .modjash file\n", path);
        close(fd); return -1;
    }
    if (expected_name && !jmod_automatic_source_trusted(path, expected_name, &st)) {
        fprintf(stderr,
                "jash: module: automatic module '%s' is not in an owner-controlled trusted path\n",
                expected_name);
        close(fd);
        return -1;
    }

    size_t file_len = (size_t)st.st_size;
    jmod_header_t preliminary;
    if (jmod_read_snapshot(fd, &preliminary, sizeof(preliminary)) != 0) {
        fprintf(stderr, "jash: module: source changed while reading header\n");
        close(fd); return -1;
    }
    if (memcmp(preliminary.magic, JMOD_MAGIC, JMOD_MAGIC_LEN) != 0) {
        fprintf(stderr, "jash: module: '%s': bad magic (not a .modjash file)\n", path);
        close(fd); return -1;
    }
    if (preliminary.abi_version > JMOD_ABI_VERSION) {
        fprintf(stderr, "jash: module: '%s': ABI version %u > loader %u\n",
                path, preliminary.abi_version, JMOD_ABI_VERSION);
        close(fd); return -1;
    }

    size_t body_off, expected_min;
    if (jmod_checked_layout(&preliminary, file_len,
                            &body_off, &expected_min) != 0) {
        fprintf(stderr, "jash: module: '%s': invalid section layout\n", path);
        close(fd); return -1;
    }

    int signed_module = (preliminary.flags & JMOD_FLAG_SIGNED) != 0;
    int envelope_signature =
        (preliminary.flags & JMOD_FLAG_SIGNED_ENVELOPE) != 0;
    if (envelope_signature && !signed_module) {
        fprintf(stderr,
                "jash: module: '%s' envelope signature flag requires signed module\n",
                path);
        close(fd); return -1;
    }
    size_t source_len = expected_min;
    if (signed_module) {
        if (preliminary.sig_len != ED25519_SIG) {
            fprintf(stderr,
                "jash: module: '%s' invalid sig_len=%u (expected %u)\n",
                path, preliminary.sig_len, (unsigned)ED25519_SIG);
            close(fd); return -1;
        }
        if (ED25519_SIG > file_len - expected_min) {
            fprintf(stderr,
                "jash: module: '%s' file truncated (signature missing)\\n", path);
            close(fd); return -1;
        }
        source_len = expected_min + ED25519_SIG;
        if (file_len != source_len) {
            fprintf(stderr, "jash: module: '%s' invalid signed file framing\n", path);
            close(fd); return -1;
        }
    } else if (preliminary.sig_len != 0 || file_len != expected_min) {
        fprintf(stderr, "jash: module: '%s' invalid unsigned file framing\n", path);
        close(fd); return -1;
    }
    if (jash_module_require_signature &&
        (!signed_module || !envelope_signature)) {
        fprintf(stderr,
                "jash: module: '%s' lacks an envelope signature and "
                "JASH_MODULE_REQUIRE_SIGNATURE is active — refusing\n",
                path);
        close(fd); return -1;
    }

    void* map = malloc(source_len);
    if (!map) {
        fprintf(stderr, "jash: module: source snapshot allocation failed\n");
        close(fd); return -1;
    }
    int snapshot_ok = jmod_read_snapshot(fd, map, source_len) == 0;
    close(fd);
    if (!snapshot_ok) {
        fprintf(stderr, "jash: module: source changed while snapshotting\n");
        free(map);
        return -1;
    }

    /* Validate header */
    const jmod_header_t* hdr = (const jmod_header_t*)map;
    if (memcmp(hdr, &preliminary, sizeof preliminary) != 0) {
        fprintf(stderr, "jash: module: source header changed while snapshotting\n");
        free(map); return -1;
    }
    if (memcmp(hdr->magic, JMOD_MAGIC, JMOD_MAGIC_LEN) != 0) {
        fprintf(stderr, "jash: module: '%s': bad magic (not a .modjash file)\n", path);
        free(map); return -1;
    }
    if (hdr->abi_version > JMOD_ABI_VERSION) {
        fprintf(stderr, "jash: module: '%s': ABI version %u > loader %u\n",
                path, hdr->abi_version, JMOD_ABI_VERSION);
        free(map); return -1;
    }

    if (jmod_checked_layout(hdr, source_len, &body_off, &expected_min) != 0) {
        fprintf(stderr, "jash: module: '%s': invalid section layout\n", path);
        free(map); return -1;
    }
    if (hdr->flags & JMOD_FLAG_SIGNED) {
        if (hdr->sig_len != ED25519_SIG) {
            fprintf(stderr,
                "jash: module: '%s' invalid sig_len=%u (expected %u)\n",
                path, hdr->sig_len, (unsigned)ED25519_SIG);
            free(map); return -1;
        }
        if (ED25519_SIG > source_len - expected_min) {
            fprintf(stderr,
                "jash: module: '%s' file truncated (signature missing)\\n", path);
            free(map); return -1;
        }
    }

    /* Parse manifest */
    const char* manifest = (const char*)map + sizeof(jmod_header_t);
    char   name[64]  = {0};
    char   ver[32]   = {0};
    char   desc[256] = {0};
    uint32_t caps_declared = 0;
    if (jmod_parse_manifest(manifest, hdr->manifest_len,
                            name,  sizeof(name),
                            ver,   sizeof(ver),
                            desc,  sizeof(desc),
                            &caps_declared) != 0) {
        fprintf(stderr, "jash: module: '%s': invalid manifest\n", path);
        free(map); return -1;
    }

    if (name[0] == '\0') {
        fprintf(stderr, "jash: module: '%s': manifest missing 'name'\n", path);
        free(map); return -1;
    }
    if (expected_name && strcmp(name, expected_name) != 0) {
        fprintf(stderr,
                "jash: module: '%s' manifest name '%s' does not match expected '%s'\n",
                path, name, expected_name);
        free(map); return -1;
    }

    /* Check for duplicate */
    if (jmod_find(name)) {
        fprintf(stderr, "jash: module: '%s' already loaded\n", name);
        free(map); return -1;
    }

#ifdef __APPLE__
    const char* suffix = ".dylib";
#else
    const char* suffix = ".so";
#endif

    const void* source_body = (const char*)map + body_off;
    uint8_t body_hash[32];
    blake3_hash(source_body, (uint64_t)hdr->body_len, body_hash);
    int hash_declared = 0;
    for (int i = 0; i < 32; i++)
        hash_declared |= hdr->body_blake3[i];

    if (hash_declared) {
        if (memcmp(body_hash, hdr->body_blake3, sizeof(body_hash)) != 0) {
            fprintf(stderr,
                "jash: module: '%s' body BLAKE3 nao confere — recusando dlopen\n",
                name);
            free(map);
            return -1;
        }
    } else if (jash_module_require_hash) {
        fprintf(stderr,
            "jash: module: '%s' sem body_blake3 e JASH_MODULE_REQUIRE_HASH ativo — recusando\n",
            name);
        free(map);
        return -1;
    }

    if (hdr->flags & JMOD_FLAG_SIGNED) {
        jash_module_trusted_pubkey_load();
        if (jash_module_trusted_pubkey_status != JMOD_KEY_READY) {
            const char* reason = "trusted signing key unavailable";
            if (jash_module_trusted_pubkey_status == JMOD_KEY_MALFORMED)
                reason = "malformed signing key";
            else if (jash_module_trusted_pubkey_status == JMOD_KEY_INSECURE)
                reason = "insecure signing key";
            else if (jash_module_trusted_pubkey_status == JMOD_KEY_SYMLINK)
                reason = "symlink signing key refused";
            fprintf(stderr, "jash: module: '%s' %s — refusing\n", name, reason);
            free(map);
            return -1;
        }

        const uint8_t* sig = (const uint8_t*)map + expected_min;
        int signature_ok;
        if (hdr->flags & JMOD_FLAG_SIGNED_ENVELOPE) {
            /* Envelope mode binds policy metadata and code while fitting the
             * Ed25519 implementation's 4 KiB message limit. */
            uint8_t signed_digest[32];
            blake3_hash(map, (uint64_t)expected_min, signed_digest);
            signature_ok = ed25519_verify(sig, signed_digest,
                                          sizeof(signed_digest),
                                          jash_module_trusted_pubkey) == 0;
        } else {
            signature_ok = ed25519_verify(sig, source_body,
                                          (uint32_t)hdr->body_len,
                                          jash_module_trusted_pubkey) == 0;
        }
        if (!signature_ok) {
            fprintf(stderr,
                "jash: module: '%s' Ed25519 signature verification failed — refusing\n",
                name);
            free(map);
            return -1;
        }
    }

    /* Apply manifest policy only after all declared integrity/signature checks.
     * The caller's historical all-caps value is a ceiling, never authority to
     * grant more than the verified manifest and startup-pinned policy allow. */
    if (jash_module_policy_invalid) {
        fprintf(stderr,
                "jash: module: refusing '%s': module policy exists but is not "
                "an owner-controlled regular file\n", name);
        free(map);
        return -1;
    }
    granted_caps &= jmod_policy_grant(name, caps_declared);
    uint32_t missing = caps_declared & ~granted_caps;
    if (missing) {
        fprintf(stderr,
                "jash: module: '%s' pede caps 0x%08x, concedidas 0x%08x "
                "(faltando 0x%08x)\n"
                "jash: conceda em ~/.jash/modules/policy.conf com uma linha "
                "'%s: +<cap> ...' (caps: fs_read fs_write net exec state_read "
                "state_write tty crypto module_load hook_pre_cmd hook_post_cmd "
                "hook_exit hook_mutation)\n",
                name, caps_declared, granted_caps, missing, name);
        free(map); return -1;
    }

    int require_immutable = hash_declared || (hdr->flags & JMOD_FLAG_SIGNED);
#ifdef __APPLE__
    const char* exec_private_dir = getenv("TMPDIR");
    if (!exec_private_dir || exec_private_dir[0] != '/') exec_private_dir = "/tmp";
#else
    const char* exec_private_dir = NULL;
#endif
    int exec_fd = jmod_exec_fd_create(source_body, (size_t)hdr->body_len,
                                      exec_private_dir, require_immutable);
    if (exec_fd < 0) {
        if (require_immutable && errno == ENOTSUP)
            fprintf(stderr,
                "jash: module: immutable execution image unavailable for verified/signed module\n");
        else
            fprintf(stderr, "jash: module: cannot create execution image: %s\n",
                    strerror(errno));
        free(map); return -1;
    }
    jmod_cache_store(jash_module_trust_home, name, suffix, source_body,
                     (size_t)hdr->body_len, body_hash);
    free(map);

    /* dlopen the immutable descriptor whose bytes were verified above. */
    char fd_path[64];
#if defined(__linux__)
    snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", exec_fd);
#elif defined(__APPLE__)
    snprintf(fd_path, sizeof(fd_path), "/dev/fd/%d", exec_fd);
#else
    fprintf(stderr, "jash: module: descriptor-backed dlopen is unsupported\n");
    close(exec_fd);
    return -1;
#endif
    void* dlh = dlopen(fd_path, RTLD_NOW | RTLD_LOCAL);
    if (!dlh) {
        fprintf(stderr, "jash: module: '%s' load failed: %s\n", name, dlerror());
        close(exec_fd); return -1;
    }

    /* Look up the ops struct */
    jmod_ops_t* ops = (jmod_ops_t*)dlsym(dlh, JMOD_OPS_SYMBOL);
    if (!ops) {
        fprintf(stderr, "jash: module: '%s' missing symbol '%s'\n", name, JMOD_OPS_SYMBOL);
        dlclose(dlh); close(exec_fd); return -1;
    }
    if (!ops->init || !ops->dispatch) {
        fprintf(stderr, "jash: module: '%s' ops missing init or dispatch\n", name);
        dlclose(dlh); close(exec_fd); return -1;
    }

    /* S5 cap enforcement: runtime caps_used must not exceed manifest caps_declared.
     * A module that sneaks in undeclared capabilities bypasses the cap gate. */
    uint32_t undeclared = ops->caps_used & ~caps_declared;
    if (undeclared) {
        fprintf(stderr,
            "jash: module: '%s' BLOCKED — runtime caps 0x%08x exceed manifest 0x%08x"
            " (undeclared: 0x%08x)\n",
            name, ops->caps_used, caps_declared, undeclared);
        dlclose(dlh); close(exec_fd); return -1;
    }

    /* Call init */
    if (ops->init() != 0) {
        fprintf(stderr, "jash: module: '%s' init() failed\n", name);
        dlclose(dlh); close(exec_fd); return -1;
    }

    /* Register */
    jmod_entry_t* e = &g_modules.entries[g_modules.count++];
    strlcpy(e->name, name, sizeof(e->name));
    strlcpy(e->path, path, sizeof(e->path));
    strlcpy(e->version, ver, sizeof(e->version));
    strlcpy(e->description, desc, sizeof(e->description));
    e->caps_declared = caps_declared;
    e->caps_used     = ops->caps_used;
    e->ops           = ops;
    e->dlhandle      = dlh;
    e->exec_fd       = exec_fd;
    e->active        = 1;

    /* Keep the cache artifact for later reuse/inspection. It is never dlopen'd. */
    return 0;
}

int jmod_load(const char* path, uint32_t granted_caps)
{
    return jmod_load_impl(path, granted_caps, NULL);
}

int jmod_load_expected(const char* path, uint32_t granted_caps,
                       const char* expected_name)
{
    if (!jmod_valid_short_name(expected_name)) {
        errno = EINVAL;
        return -1;
    }
    return jmod_load_impl(path, granted_caps, expected_name);
}

/* ── Unload ────────────────────────────────────────────────────────────── */

int jmod_unload(const char* name)
{
    for (int i = 0; i < g_modules.count; i++) {
        jmod_entry_t* e = &g_modules.entries[i];
        if (!e->active) continue;
        if (strcmp(e->name, name) != 0) continue;

        if (e->ops->shutdown) e->ops->shutdown();
        if (e->dlhandle) dlclose(e->dlhandle);
        if (e->exec_fd >= 0) close(e->exec_fd);
        e->active = 0;

        /* Compact table */
        for (int j = i; j < g_modules.count - 1; j++)
            g_modules.entries[j] = g_modules.entries[j + 1];
        g_modules.count--;
        return 0;
    }
    fprintf(stderr, "jash: module: '%s' not loaded\n", name);
    return -1;
}

/* ── Dispatch ──────────────────────────────────────────────────────────── */

int jmod_dispatch(const char* verb, const jmod_call_t* call)
{
    for (int i = 0; i < g_modules.count; i++) {
        jmod_entry_t* e = &g_modules.entries[i];
        if (!e->active) continue;
        jmod_call_t c = *call;
        c.verb = verb;
        int rc = e->ops->dispatch(&c);
        if (rc >= 0) return rc;   /* module claimed the verb — stop */
    }
    return -1;   /* no module handled this verb */
}

/* ── Hooks ─────────────────────────────────────────────────────────────── */

void jmod_hook_pre_cmd(const jmod_call_t* call)
{
    for (int i = 0; i < g_modules.count; i++) {
        jmod_entry_t* e = &g_modules.entries[i];
        if (e->active && e->ops->hook_pre_cmd)
            e->ops->hook_pre_cmd(call);
    }
}

void jmod_hook_post_cmd(const jmod_call_t* call, int exit_code)
{
    for (int i = 0; i < g_modules.count; i++) {
        jmod_entry_t* e = &g_modules.entries[i];
        if (e->active && e->ops->hook_post_cmd)
            e->ops->hook_post_cmd(call, exit_code);
    }
}

void jmod_hook_exit(int exit_code)
{
    for (int i = g_modules.count - 1; i >= 0; i--) {
        jmod_entry_t* e = &g_modules.entries[i];
        if (e->active && e->ops->hook_exit)
            e->ops->hook_exit(exit_code);
    }
}

void jmod_hook_mutation(int kind, const char* desc,
                        const char* a, const char* b)
{
    for (int i = 0; i < g_modules.count; i++) {
        jmod_entry_t* e = &g_modules.entries[i];
        if (e->active && e->ops->hook_mutation)
            e->ops->hook_mutation(kind, desc, a, b);
    }
}

/* ── Lookup ────────────────────────────────────────────────────────────── */

jmod_entry_t* jmod_find(const char* name)
{
    for (int i = 0; i < g_modules.count; i++) {
        if (g_modules.entries[i].active &&
            strcmp(g_modules.entries[i].name, name) == 0)
            return &g_modules.entries[i];
    }
    return NULL;
}

void jmod_each(void (*fn)(const jmod_entry_t* e, void* udata), void* udata)
{
    for (int i = 0; i < g_modules.count; i++)
        if (g_modules.entries[i].active)
            fn(&g_modules.entries[i], udata);
}
