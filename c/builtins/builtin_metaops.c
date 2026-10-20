// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/builtin_metaops — mkdir/rmdir/chmod/chown/ln (ADR-0050 W2)
// F E R M I ∞ H A R T
//
// These ops used to run as EXTERNALS (/bin/*) — invisible to the journal,
// so `mkdir`, `chmod`, `rmdir` mutations could never be undone. Native
// builtins mean the shell witnesses every metadata mutation and journals
// the inverse:
//
//   mkdir (-p, -m)   → JASH_UNDO_DIR_CREATE (undo: rmdir, refused if non-empty)
//   rmdir            → JASH_UNDO_DIR_REMOVE (undo: mkdir with old mode)
//   chmod [-R]       → JASH_UNDO_META (undo: old mode back)
//   chown            → JASH_UNDO_META (undo: old uid/gid back)
//   ln [-s]          → JASH_UNDO_LINK_CREATE (undo: unlink; target kept for redo)
//
// chmod -R is capped at 64 journaled entries per invocation — the journal
// stays a safety net, never a second filesystem walk.

#include <builtins/builtin.h>
#include <core/state.h>
#include <core/timeline.h>
#include <core/mutation_report.h>
#include <theme/io.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

static int meta_parse_mode(const char* s, long* out) {
    if (!s || !*s) return -1;
    for (const char* p = s; *p; p++)
        if (*p < '0' || *p > '7') return -1;
    errno = 0;
    *out = strtol(s, NULL, 8);
    if (errno || *out > 07777) return -1;
    return 0;
}

/* JINV-111: symbolic modes. The old code refused them and called that honesty,
   but the builtin SHADOWS the name `chmod`, so refusing left the user with no
   reachable working tool — `chmod +x` simply stopped existing. A refusal is
   only honest when the job can still be done. Per ADR-0044 the builtin owes
   the caller the real semantics, so we implement POSIX symbolic modes here.

   Resolution is per-file on purpose: `+`, `-` and `X` all read the CURRENT
   mode, so a recursive walk must decide each file on its own evidence rather
   than reuse one mode computed from the first.

     spec  := clause[,clause...]
     clause:= [ugoa]* ( [+-=] ([rwxXst]* | [ugo]) )+   */
static int meta_apply_symbolic(const char* spec, mode_t cur, int is_dir,
                               mode_t umask_val, mode_t* out) {
    if (!spec || !*spec) return -1;
    /* A leading digit means the caller meant octal and mistyped it; don't
       silently reinterpret it as symbolic. */
    if (*spec >= '0' && *spec <= '9') return -1;

    mode_t m = cur & 07777;
    const char* p = spec;

    for (;;) {
        mode_t who = 0;
        int who_given = 0;
        for (; *p; p++) {
            if (*p == 'u')      { who |= S_ISUID | S_IRWXU; who_given = 1; }
            else if (*p == 'g') { who |= S_ISGID | S_IRWXG; who_given = 1; }
            else if (*p == 'o') { who |= S_IRWXO;           who_given = 1; }
            else if (*p == 'a') { who |= S_ISUID | S_ISGID | S_IRWXU | S_IRWXG | S_IRWXO;
                                  who_given = 1; }
            else break;
        }
        if (!who_given) {
            /* POSIX: bare `+x` means all three classes, except bits the umask
               withholds. The umask governs only the 9 permission bits. */
            who = (S_IRWXU | S_IRWXG | S_IRWXO) & ~(umask_val & 0777);
            who |= S_ISUID | S_ISGID;
        }

        /* At least one op is required — a bare who like "ug" is malformed. */
        if (*p != '+' && *p != '-' && *p != '=') return -1;

        while (*p == '+' || *p == '-' || *p == '=') {
            char op = *p++;
            mode_t bits = 0;
            mode_t extra_who = 0;

            if (*p == 'u' || *p == 'g' || *p == 'o') {
                /* permcopy: mirror an existing class across the who set. */
                mode_t src = 0;
                if (*p == 'u')      src = (m & S_IRWXU) >> 6;
                else if (*p == 'g') src = (m & S_IRWXG) >> 3;
                else                src = (m & S_IRWXO);
                p++;
                bits = (src << 6) | (src << 3) | src;
            } else {
                for (; *p; p++) {
                    if (*p == 'r')      bits |= S_IRUSR | S_IRGRP | S_IROTH;
                    else if (*p == 'w') bits |= S_IWUSR | S_IWGRP | S_IWOTH;
                    else if (*p == 'x') bits |= S_IXUSR | S_IXGRP | S_IXOTH;
                    else if (*p == 'X') {
                        /* Only for directories, or files already executable by
                           somebody — the whole point of X over x. */
                         if (is_dir || (m & (S_IXUSR | S_IXGRP | S_IXOTH)))
                            bits |= S_IXUSR | S_IXGRP | S_IXOTH;
                    }
                    else if (*p == 's') bits |= S_ISUID | S_ISGID;
                    else if (*p == 't') { bits |= S_ISVTX; extra_who |= S_ISVTX; }
                    else break;
                }
            }

            mode_t mask = who | extra_who;
            if (op == '+')      m |= (bits & mask);
            else if (op == '-') m &= ~(bits & mask);
            else                m = (m & ~mask) | (bits & mask);
        }

        if (*p == ',') { p++; continue; }
        if (*p == '\0') break;
        return -1;  /* trailing garbage — reject rather than half-apply */
    }

    *out = m;
    return 0;
}

static void meta_snapshot(const struct stat* st, char* out, size_t cap) {
    snprintf(out, cap, "%lo:%ld:%ld",
             (unsigned long)(st->st_mode & 07777),
             (long)st->st_uid, (long)st->st_gid);
}

/* ── mkdir ─────────────────────────────────────────────────────────────── */

typedef struct {
    jash_writer_t writer;
    void* context;
    jash_mutation_report_t* report;
    int preview;
    int journaled;
    mode_t mask;
    jash_recovery_ref_t pending_ref;
} meta_run_t;

static int meta_result(meta_run_t* run, const char* source, const char* target,
                       const char* action, jash_mutation_status_t status,
                       jash_mutation_effect_t effect, const char* code) {
    jash_mutation_item_t item = {.source = source, .destination = target, .action = action,
                                .status = status, .effect = effect, .code = code};
    item.undo = run->pending_ref;
    memset(&run->pending_ref, 0, sizeof(run->pending_ref));
    if (!run->report && status == JASH_MUT_PLANNED)
        jash_writef(run->writer, run->context, "would %s '%s'\n", action, target);
    if (jash_mutation_report_item(run->report, &item)) return 125;
    return status == JASH_MUT_FAILED || status == JASH_MUT_PARTIAL ? 1 : 0;
}

static int meta_error(meta_run_t* run, const char* path, const char* action,
                       const char* code, jash_mutation_effect_t effect) {
    if (!strcmp(action, "chown"))
        jash_writef(run->writer, run->context,
                    "chown: cannot chown '%s': %s\n", path, strerror(errno));
    else
        jash_writef(run->writer, run->context,
                    "%s: '%s': %s\n", action, path, strerror(errno));
    return meta_result(run, NULL, path, action, JASH_MUT_FAILED, effect, code);
}

static void meta_journal(meta_run_t* run, const char* path, const char* action, jash_undo_kind_t kind,
                         const char* previous) {
    char description[256];
    snprintf(description, sizeof(description), "%s %s", action, path);
    jash_timeline_record_ref(kind, description, path, previous, run->report ? &run->pending_ref : NULL);
}

static int mkdir_one(meta_run_t* run, const char* path, mode_t mode, int parents, int exact) {
    if (jash_mutation_report_stop(run->report)) return jash_mutation_report_stop(run->report);
    struct stat st;
    if (run->preview) {
        if (stat(path, &st) == 0) {
            if (parents && S_ISDIR(st.st_mode))
                return meta_result(run, NULL, path, "mkdir", JASH_MUT_SKIPPED, JASH_MUT_NONE, "exists");
            errno = EEXIST;
            return meta_error(run, path, "mkdir", "destination_exists", JASH_MUT_NONE);
        }
        if (errno != ENOENT) return meta_error(run, path, "mkdir", "inspect_target", JASH_MUT_NONE);
        return meta_result(run, NULL, path, "mkdir", JASH_MUT_PLANNED, JASH_MUT_NONE, NULL);
    }
    /* Apply explicit modes in the create syscall, including mode 000. Opening
     * a newly unreadable directory just to chmod it would spuriously fail and
     * would introduce a second pathname lookup after creation. */
    mode_t old_mask = 0;
    if (exact) old_mask = umask(0);
    int mkdir_rc = mkdir(path, mode);
    int saved = errno;
    if (exact) umask(old_mask);
    if (mkdir_rc != 0) {
        if (saved == EEXIST && parents && stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            return meta_result(run, NULL, path, "mkdir", JASH_MUT_SKIPPED, JASH_MUT_NONE, "exists");
        errno = saved;
        return meta_error(run, path, "mkdir", "create_directory", JASH_MUT_NONE);
    }
    meta_journal(run, path, "mkdir", JASH_UNDO_DIR_CREATE, NULL);
    return meta_result(run, NULL, path, "mkdir", JASH_MUT_COMPLETED, JASH_MUT_CHANGED, NULL);
}

static int mkdir_parents(meta_run_t* run, const char* path, mode_t mode, int exact) {
    if (!*path) { errno = ENOENT; return meta_error(run, path, "mkdir", "empty_path", JASH_MUT_NONE); }
    char* copy = strdup(path);
    if (!copy) return meta_error(run, path, "mkdir", "out_of_memory", JASH_MUT_NONE);
    size_t len = strlen(copy);
    while (len > 1 && copy[len - 1] == '/') copy[--len] = '\0';
    int rc = 0;
    for (size_t i = 1; i <= len; i++) {
        if (copy[i] != '/' && copy[i]) continue;
        if (copy[i] == '/' && copy[i - 1] == '/') continue;
        char saved = copy[i]; copy[i] = '\0';
        /* Intermediate directories need owner write/search for descendants.
         * -m applies only to the final component. */
        mode_t current = saved ? (0777 & ~run->mask) | 0300 : mode;
        rc = mkdir_one(run, copy, current, 1, saved ? 1 : exact);
        copy[i] = saved;
        if (rc) break;
    }
    free(copy);
    return rc;
}

static int meta_empty_dir(const char* path) {
    DIR* dir = opendir(path);
    if (!dir) return -1;
    int rc = 0;
    for (;;) {
        errno = 0;
        struct dirent* ent = readdir(dir);
        if (!ent) { if (errno) rc = -1; break; }
        if (strcmp(ent->d_name, ".") && strcmp(ent->d_name, "..")) { errno = ENOTEMPTY; rc = -1; break; }
    }
    int saved = errno;
    closedir(dir); errno = saved;
    return rc;
}

static int rmdir_one(meta_run_t* run, const char* path) {
    struct stat st;
    if (lstat(path, &st) != 0) return meta_error(run, path, "rmdir", "inspect_target", JASH_MUT_NONE);
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return meta_error(run, path, "rmdir", "not_directory", JASH_MUT_NONE); }
    if (run->preview) {
        if (meta_empty_dir(path)) return meta_error(run, path, "rmdir", "inspect_directory", JASH_MUT_NONE);
        return meta_result(run, NULL, path, "rmdir", JASH_MUT_PLANNED, JASH_MUT_NONE, NULL);
    }
    if (rmdir(path)) return meta_error(run, path, "rmdir", "remove_directory", JASH_MUT_NONE);
    char mode[16];
    snprintf(mode, sizeof(mode), "%lo", (unsigned long)(st.st_mode & 07777));
    meta_journal(run, path, "rmdir", JASH_UNDO_DIR_REMOVE, mode);
    return meta_result(run, NULL, path, "rmdir", JASH_MUT_COMPLETED, JASH_MUT_CHANGED, NULL);
}

/* Descendant links are skipped. Pin opened directories across the walk so a
 * renamed parent does not redirect child lookup. Depth exhaustion is an error,
 * never a silently completed recursive operation. */
static int chmod_one(meta_run_t* run, int parent, const char* name, const char* path,
                      long mode, const char* sym, int recurse, unsigned depth) {
    if (jash_mutation_report_stop(run->report)) return jash_mutation_report_stop(run->report);
    struct stat st;
    if (fstatat(parent, name, &st, depth ? AT_SYMLINK_NOFOLLOW : 0))
        return meta_error(run, path, "chmod", "inspect_target", JASH_MUT_NONE);
    if (depth && S_ISLNK(st.st_mode))
        return meta_result(run, NULL, path, "chmod", JASH_MUT_SKIPPED, JASH_MUT_NONE, "symlink_not_followed");
    mode_t desired = (mode_t)mode;
    if (sym && meta_apply_symbolic(sym, st.st_mode, S_ISDIR(st.st_mode), run->mask, &desired)) {
        errno = EINVAL;
        return meta_error(run, path, "chmod", "invalid_mode", JASH_MUT_NONE);
    }
    int fd = -1;
    if (recurse && S_ISDIR(st.st_mode)) {
        if (depth >= 64) { errno = ELOOP; return meta_error(run, path, "chmod", "depth_limit", JASH_MUT_NONE); }
        fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | (depth ? O_NOFOLLOW : 0));
        /* Opening may fail until chmod grants read/search permission. Retry
         * below after applying, but propagate failure rather than skip a tree. */
    }
    int rc = 0;
    if (!run->preview) {
        rc = fd >= 0 ? fchmod(fd, desired) : fchmodat(parent, name, desired, depth ? AT_SYMLINK_NOFOLLOW : 0);
        if (rc) {
            if (fd >= 0) close(fd);
            return meta_error(run, path, "chmod", "set_mode", JASH_MUT_NONE);
        }
        if (run->journaled < 64) {
            char previous[64]; meta_snapshot(&st, previous, sizeof(previous));
            meta_journal(run, path, "chmod", JASH_UNDO_META, previous);
            run->journaled++;
        }
    }
    rc = meta_result(run, NULL, path, "chmod", run->preview ? JASH_MUT_PLANNED : JASH_MUT_COMPLETED,
                     run->preview ? JASH_MUT_NONE : JASH_MUT_CHANGED, NULL);
    if (rc) { if (fd >= 0) close(fd); return rc; }
    if (!recurse || !S_ISDIR(st.st_mode)) return 0;
    if (fd < 0) fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | (depth ? O_NOFOLLOW : 0));
    if (fd < 0) return meta_error(run, path, "chmod", "open_directory", JASH_MUT_NONE);
    DIR* dir = fdopendir(fd);
    if (!dir) { close(fd); return meta_error(run, path, "chmod", "open_directory", JASH_MUT_NONE); }
    for (;;) {
        if (jash_mutation_report_stop(run->report)) { rc = jash_mutation_report_stop(run->report); break; }
        errno = 0;
        struct dirent* ent = readdir(dir);
        if (!ent) {
            if (errno) rc = meta_error(run, path, "chmod", "read_directory", JASH_MUT_NONE);
            break;
        }
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        size_t len = strlen(path) + strlen(ent->d_name) + 2;
        char* child = malloc(len);
        if (!child) { rc = meta_error(run, path, "chmod", "out_of_memory", JASH_MUT_NONE); break; }
        snprintf(child, len, "%s/%s", path, ent->d_name);
        int child_rc = chmod_one(run, fd, ent->d_name, child, mode, sym, recurse, depth + 1);
        free(child);
        if (child_rc) rc = child_rc;
        if (rc == 125) break;
    }
    closedir(dir);
    return rc;
}

static int meta_id(const char* text, size_t n, uintmax_t maximum, uintmax_t* out) {
    if (!n) return -1;
    uintmax_t value = 0;
    for (size_t i = 0; i < n; i++) {
        if (text[i] < '0' || text[i] > '9') return -1;
        unsigned digit = (unsigned)(text[i] - '0');
        if (value > (maximum - digit) / 10) return -1;
        value = value * 10 + digit;
    }
    *out = value;
    return 0;
}

static int chown_one(meta_run_t* run, const char* path, uid_t uid, gid_t gid) {
    struct stat st;
    if (stat(path, &st)) return meta_error(run, path, "chown", "inspect_target", JASH_MUT_NONE);
    if (run->preview) return meta_result(run, NULL, path, "chown", JASH_MUT_PLANNED, JASH_MUT_NONE, NULL);
    if (chown(path, uid, gid)) return meta_error(run, path, "chown", "set_owner", JASH_MUT_NONE);
    char previous[64]; meta_snapshot(&st, previous, sizeof(previous));
    meta_journal(run, path, "chown", JASH_UNDO_META, previous);
    return meta_result(run, NULL, path, "chown", JASH_MUT_COMPLETED, JASH_MUT_CHANGED, NULL);
}

static int ln_one(meta_run_t* run, const char* target, const char* path, int symbolic) {
    if (jash_mutation_report_stop(run->report)) return jash_mutation_report_stop(run->report);
    const char* action = symbolic ? "symlink" : "hardlink";
    int rc = 0;
    if (run->preview) {
        struct stat st;
        if (!lstat(path, &st)) { errno = EEXIST; rc = -1; }
        else if (errno != ENOENT) rc = -1;
        if (!rc && !symbolic) {
            rc = lstat(target, &st);
            if (!rc && S_ISDIR(st.st_mode)) { errno = EPERM; rc = -1; }
        }
    } else rc = symbolic ? symlink(target, path) : link(target, path);
    if (rc) {
        jash_writef(run->writer, run->context, "ln: '%s': %s\n", path, strerror(errno));
        return meta_result(run, target, path, action, JASH_MUT_FAILED, JASH_MUT_NONE, "create_link");
    }
    if (!run->preview) {
        size_t len = strlen(target) + 3;
        char* previous = malloc(len);
        if (previous) {
            snprintf(previous, len, "%c:%s", symbolic ? 's' : 'h', target);
            meta_journal(run, path, "ln", JASH_UNDO_LINK_CREATE, previous);
            free(previous);
        }
    }
    return meta_result(run, target, path, action, run->preview ? JASH_MUT_PLANNED : JASH_MUT_COMPLETED,
                       run->preview ? JASH_MUT_NONE : JASH_MUT_CHANGED, NULL);
}

static int meta_run(const char* operation, meta_run_t* run, int argc, char** argv) {
    int is_mkdir = !strcmp(operation, "mkdir"), is_chmod = !strcmp(operation, "chmod");
    int is_chown = !strcmp(operation, "chown"), is_ln = !strcmp(operation, "ln");
    int parents = 0, recurse = 0, symbolic = 0, exact = 0;
    long mode = 0777;
    int i = 0;
    for (; i < argc; i++) {
        const char* arg = argv[i];
        if (!strcmp(arg, "--")) { i++; break; }
        if (!strcmp(arg, "--dry-run")) { run->preview = 1; if (run->report) run->report->preview = 1; continue; }
        if (is_mkdir && (!strcmp(arg, "-p") || !strcmp(arg, "--parents"))) { parents = 1; continue; }
        if (is_mkdir && (!strcmp(arg, "-m") || !strcmp(arg, "--mode") ||
                         !strncmp(arg, "--mode=", 7) || (!strncmp(arg, "-m", 2) && arg[2]))) {
            const char* value = !strncmp(arg, "--mode=", 7) ? arg + 7 :
                                arg[1] == 'm' && arg[2] ? arg + 2 : ++i < argc ? argv[i] : NULL;
            if (meta_parse_mode(value, &mode)) goto invalid;
            exact = 1; continue;
        }
        if (is_chmod && (!strcmp(arg, "-R") || !strcmp(arg, "--recursive"))) { recurse = 1; continue; }
        if (is_ln && (!strcmp(arg, "-s") || !strcmp(arg, "--symbolic"))) { symbolic = 1; continue; }
        /* chmod accepts a symbolic mode beginning with '-' (e.g. -w). */
        if (arg[0] == '-' && arg[1] && !is_chmod) goto invalid;
        break;
    }
    const char* sym = NULL;
    uid_t uid = (uid_t)-1;
    gid_t gid = (gid_t)-1;
    if (is_chmod) {
        if (i >= argc) goto invalid;
        if (meta_parse_mode(argv[i], &mode)) {
            mode_t probe;
            if (meta_apply_symbolic(argv[i], 0644, 0, run->mask, &probe)) goto invalid;
            sym = argv[i];
        }
        i++;
    } else if (is_chown) {
        if (i >= argc) goto chown_usage;
        const char* owner = argv[i++];
        const char* colon = strchr(owner, ':');
        uintmax_t value;
        if (meta_id(owner, colon ? (size_t)(colon - owner) : strlen(owner),
                    (uintmax_t)((uid_t)-1) - 1, &value)) {
            jash_writef(run->writer, run->context,
                        "chown: invalid uid '%.*s'\n",
                        (int)(colon ? (size_t)(colon - owner) : strlen(owner)), owner);
            if (run->report) run->report->error_code = "invalid_arguments";
            return 2;
        }
        uid = (uid_t)value;
        if (colon) {
            if (meta_id(colon + 1, strlen(colon + 1),
                        (uintmax_t)((gid_t)-1) - 1, &value)) {
                jash_writef(run->writer, run->context,
                            "chown: invalid gid '%s'\n", colon + 1);
                if (run->report) run->report->error_code = "invalid_arguments";
                return 2;
            }
            gid = (gid_t)value;
        }
        if (i >= argc) goto chown_usage;
    }
    if (i >= argc || (is_ln && argc - i != 2)) goto invalid;
    if (is_ln) return ln_one(run, argv[i], argv[i + 1], symbolic);
    int overall = 0;
    for (; i < argc; i++) {
        if (jash_mutation_report_stop(run->report)) return jash_mutation_report_stop(run->report);
        int rc;
        if (is_mkdir) rc = parents ? mkdir_parents(run, argv[i], (mode_t)mode, exact) : mkdir_one(run, argv[i], (mode_t)mode, 0, exact);
        else if (is_chmod) rc = chmod_one(run, AT_FDCWD, argv[i], argv[i], mode, sym, recurse, 0);
        else if (is_chown) rc = chown_one(run, argv[i], uid, gid);
        else rc = rmdir_one(run, argv[i]);
        if (rc) overall = rc;
        if (rc == 125) return rc;
    }
    return overall;
invalid:
    jash_writef(run->writer, run->context, "%s: invalid options or missing operands\n", operation);
    if (run->report) run->report->error_code = "invalid_arguments";
    return 2;
 chown_usage:
    jash_writef(run->writer, run->context,
                "usage: chown UID[:GID] PATH...\n");
    if (run->report) run->report->error_code = "invalid_arguments";
    return 2;
}

static int meta_dispatch(const char* operation, jash_writer_t writer, void* context, int argc, char** argv) {
    static const char* const values[] = {"-m", "--mode", NULL};
    int count;
    char** args;
    int json;
    jash_mutation_format_t format = JASH_MUTATION_TEXT;
    if (!strcmp(operation, "chmod")) {
        /* A negative symbolic MODE is an operand, not an option prefix. Keep
         * `chmod -w --json` pointed at the literal file named --json. */
        int prefix = 0;
        while (prefix < argc) {
            const char* arg = argv[prefix];
            if (strcmp(arg, "--json") && strcmp(arg, "--data") && strcmp(arg, "--data-v2") &&
                strcmp(arg, "-R") && strcmp(arg, "--recursive") && strcmp(arg, "--dry-run")) break;
            prefix++;
        }
        json = jash_mutation_args_mode(prefix, argv, NULL, &count, &args, &format);
        if (json > 0) {
            char** full = realloc(args, ((size_t)argc + 1) * sizeof(*args));
            if (!full) { free(args); args = NULL; json = -1; }
            else {
                args = full;
                for (int i = prefix; i < argc; i++) args[count++] = argv[i];
                args[count] = NULL;
            }
        }
    } else json = jash_mutation_args_mode(argc, argv, !strcmp(operation, "mkdir") ? values : NULL, &count, &args, &format);
    int args_error = errno;
    meta_run_t run = {.writer = writer, .context = context};
    run.mask = umask(0); umask(run.mask);
    if (!json) return meta_run(operation, &run, argc, argv);
    jash_mutation_report_t report;
    if (jash_mutation_report_begin_mode(&report, operation, "filesystem_action", format, writer, context)) { free(args); return 125; }
    if (json < 0) { report.error_code = args_error == ENOMEM ? "out_of_memory" : "invalid_format_options"; return jash_mutation_report_end(&report, 2); }
    run.report = &report; run.writer = jash_stderr_writer; run.context = NULL;
    int rc = meta_run(operation, &run, count, args);
    free(args);
    return jash_mutation_report_end(&report, rc);
}

int jash_b_mkdir(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    (void)s; return meta_dispatch("mkdir", w, ctx, argc, argv);
}
int jash_b_rmdir(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    (void)s; return meta_dispatch("rmdir", w, ctx, argc, argv);
}
int jash_b_chmod(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    (void)s; return meta_dispatch("chmod", w, ctx, argc, argv);
}
int jash_b_chown(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    (void)s; return meta_dispatch("chown", w, ctx, argc, argv);
}
int jash_b_ln(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    (void)s; return meta_dispatch("ln", w, ctx, argc, argv);
}
