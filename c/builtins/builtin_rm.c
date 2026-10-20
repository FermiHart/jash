// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: builtins/rm — disruptive rm builtin with batch operations,
// safety checks, dry-run, and undo (trash) functionality.
//
// Features:
//   rm file1 file2 ...    — remove files (batch, optimized)
//   rm --dry-run *.log    — show what would be deleted without deleting
//   rm --safe /           — BLOCKED: root directory protection
//   rm --undo file.txt    — move to ~/.jash/trash (recoverable 24h)
//   rm -r dir             — recursive delete with safety
//
// Safety: blocks deletion of /, /home, /etc, /usr, /bin, /sbin
// Trash: files moved to ~/.jash/trash/ with timestamp, auto-cleanup after 24h

#include "builtin.h"
#include <errno.h>
#include <core/env.h>
#include <core/timeline.h>
#include <core/guard.h>
#include <module/sigil.h>
#include <sys/syscalls.h>
#include <theme/colors.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <core/mutation_report.h>

#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

/* ── Critical path protection ─────────────────────────────────────────────── */

static const char* CRITICAL_PATHS[] = {
    "/",
    "/home",
    "/etc",
    "/usr",
    "/bin",
    "/sbin",
    "/lib",
    "/lib64",
    "/boot",
    "/var",
    "/root",
    NULL
};

/* Lexically resolve . and .. before comparing against protected roots. If the
 * path exists, realpath also resolves symlinked directory components; the final
 * component remains an exact match check, never a prefix of user data. */
static void rm_normalize_path(const char* path, char* out, size_t cap) {
    if (!path || !*path) { snprintf(out, cap, "%s", path ? path : ""); return; }
    char absolute[2048];
    if (path[0] == '/') {
        snprintf(absolute, sizeof(absolute), "%s", path);
    } else {
        char cwd[1024];
        if (!getcwd(cwd, sizeof(cwd))) { snprintf(out, cap, "%s", path); return; }
        snprintf(absolute, sizeof(absolute), "%s/%s", cwd, path);
    }

    char work[2048];
    snprintf(work, sizeof(work), "%s", absolute);
    char* parts[256];
    int np = 0;
    char* save = NULL;
    for (char* tok = strtok_r(work, "/", &save); tok && np < 256;
         tok = strtok_r(NULL, "/", &save)) {
        if (strcmp(tok, ".") == 0) continue;
        if (strcmp(tok, "..") == 0) { if (np > 0) np--; continue; }
        parts[np++] = tok;
    }

    size_t o = 0;
    if (o + 1 < cap) out[o++] = '/';
    for (int i = 0; i < np; i++) {
        int n = snprintf(out + o, cap - o, "%s%s", i ? "/" : "", parts[i]);
        if (n < 0 || (size_t)n >= cap - o) { out[0] = '\0'; return; }
        o += (size_t)n;
    }
    if (o == 0 && cap) out[o++] = '/';
    if (cap) out[o < cap ? o : cap - 1] = '\0';
}

static int rm_is_critical(const char* path) {
    if (!path) return 0;

    char normalized[2048];
    rm_normalize_path(path, normalized, sizeof(normalized));
    if (!normalized[0]) return 1;   /* unrepresentable input is not deletable */

    char* resolved = realpath(normalized, NULL);
    if (resolved) {
        snprintf(normalized, sizeof(normalized), "%s", resolved);
        free(resolved);
    }

    size_t len = strlen(normalized);
    while (len > 1 && normalized[len - 1] == '/') normalized[--len] = '\0';

    for (int i = 0; CRITICAL_PATHS[i]; i++) {
        const char* critical = CRITICAL_PATHS[i];
        if (strcmp(normalized, critical) == 0) return 1;
    }
    return 0;
}

/* ── Trash functionality ──────────────────────────────────────────────────── */

#define TRASH_DIR "~/.jash/trash"
/* ADR-0050 W16: TTL policy lives in builtin_trash.c (single source) —
 * the local 24h constant disagreed with the 48h display for months. */
extern long jash_trash_ttl_for(long size_bytes);

static int rm_get_trash_dir(char* buf, size_t bufsz) {
    const char* home = getenv("HOME");
    if (!home) return -1;
    snprintf(buf, bufsz, "%s/.jash/trash", home);
    return 0;
}

static int rm_ensure_trash_dir(void) {
    char trash_dir[512];
    if (rm_get_trash_dir(trash_dir, sizeof(trash_dir)) < 0) {
        return -1;
    }
    
    struct stat st;
    if (stat(trash_dir, &st) != 0) {
        /* Create ~/.jash and ~/.jash/trash */
        char jash_dir[512];
        snprintf(jash_dir, sizeof(jash_dir), "%s/.jash", getenv("HOME"));
        mkdir(jash_dir, 0700);
        mkdir(trash_dir, 0700);
    }
    
    return 0;
}

static int rm_move_to_trash(const char* path, jash_writer_t w, void* ctx, char* destination, size_t capacity) {
    if (rm_ensure_trash_dir() < 0) {
        jash_writes(w, ctx, "rm: cannot create trash directory\n");
        return -1;
    }
    
    char trash_dir[512];
    rm_get_trash_dir(trash_dir, sizeof(trash_dir));
    
    /* Check available space in trash directory (lstat so a dangling symlink is
     * still trashable — stat would fail on a broken link). */
    struct stat st;
    if (lstat(path, &st) != 0) {
        jash_writef(w, ctx, "rm: cannot stat '%s'\n", path);
        return -1;
    }
    int regular = S_ISREG(st.st_mode);
    
    /* For large files (>100MB), warn user */
    if (st.st_size > 100 * 1024 * 1024) {
        long size_mb = st.st_size / (1024 * 1024);
        jash_writef(w, ctx, "rm: warning - large file (%ldMB) will be moved to trash\n", size_mb);
    }
    
    /* Extract basename */
    const char* basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;
    
    /* Create trash filename with timestamp */
    char trash_path[1024];
    time_t now = time(NULL);
    snprintf(trash_path, sizeof(trash_path), "%s/%ld_%s",
             trash_dir, (long)now, basename);
    
    /* Check if trash file already exists (collision) */
    if (stat(trash_path, &st) == 0) {
        /* Add microseconds to avoid collision */
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
            jash_writes(w, ctx, "rm: cannot generate a unique trash name\n");
            return -1;
        }
        snprintf(trash_path, sizeof(trash_path), "%s/%ld_%ld_%s",
                 trash_dir, (long)now, ts.tv_nsec, basename);
    }
    
    /* Save original directory so `undo` can restore exactly like `trash`. */
    char orig_dir[512] = "";
    const char *last_slash = strrchr(path, '/');
    if (last_slash) {
        size_t dir_len = (size_t)(last_slash - path);
        if (dir_len < sizeof(orig_dir)) {
            memcpy(orig_dir, path, dir_len);
            orig_dir[dir_len] = '\0';
        }
    }
    if (orig_dir[0] == '\0' && getcwd(orig_dir, sizeof(orig_dir)) == NULL) {
        strlcpy(orig_dir, ".", sizeof(orig_dir));
    }

    int moved = rename(path, trash_path);
    if (moved != 0 && errno == EXDEV && regular) {
        /* Reuse the durable, no-replace publication and rollback path. The old
         * rm-only rename implementation missed trash's EXDEV fallback. */
        const char* name = strrchr(trash_path, '/') + 1;
        if (jash_trash_move_file(path, trash_dir, name, orig_dir) == 0) return 0;
        jash_writef(w, ctx, "rm: cannot move '%s' to trash: %s\n", path, strerror(errno));
        return -1;
    }
    if (moved != 0) {
        jash_writef(w, ctx, "rm: cannot move '%s' to trash: %s%s\n", path,
                    strerror(errno), errno == EXDEV ?
                    " (cross-device trash currently supports regular files only; source retained)" : "");
        return -1;
    }

    char meta_path[1024];
    snprintf(meta_path, sizeof(meta_path), "%s.meta", trash_path);
    FILE *mf = fopen(meta_path, "w");
    if (mf) {
        fprintf(mf, "%s\n", orig_dir);
        fclose(mf);
    }
    
    /* INV-JASH-CHECKPOINT-BBC-001: forward the caller's checked capacity,
     * including the terminator, without an unknown-size string operation. */
    if (destination && strlen(trash_path) < capacity)
        memcpy(destination, trash_path, strlen(trash_path) + 1);
    return 0;
}

static void rm_record_trash_undo(const char *path, const char* destination, jash_recovery_ref_t* ref) {
    char desc[200];
    snprintf(desc, sizeof(desc), "rm --undo %s", path);
    if (ref && destination && *destination)
        jash_timeline_record_ref(JASH_UNDO_TRASH, desc, path, destination, ref);
    else jash_timeline_record(JASH_UNDO_TRASH, desc, path, NULL);
}

/* Clean up old files in trash (> 24h) */
static void rm_cleanup_trash(void) {
    char trash_dir[512];
    if (rm_get_trash_dir(trash_dir, sizeof(trash_dir)) < 0) {
        return;
    }
    
    DIR* dir = opendir(trash_dir);
    if (!dir) return;
    jash_recovery_pins_t* pins = jash_recovery_pins_load();
    if (!jash_recovery_pins_complete(pins)) { jash_recovery_pins_free(pins); closedir(dir); return; }
    
    time_t now = time(NULL);
    struct dirent* ent;
    
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        
        char fullpath[1024];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", trash_dir, ent->d_name);
        if (jash_recovery_pins_contains(pins, fullpath)) continue;
        
        struct stat st;
        if (stat(fullpath, &st) == 0) {
            if (now - st.st_mtime > jash_trash_ttl_for((long)st.st_size)) {
                unlink(fullpath);
            }
        }
    }
    
    closedir(dir);
    jash_recovery_pins_free(pins);
}

/* ── Recursive delete ─────────────────────────────────────────────────────── */

static int rm_tree_at(int parent_fd, const char* name, const char* display,
                      jash_writer_t w, void* ctx, int dry_run, jash_mutation_item_t* item) {
    if (jash_mutation_cancel_status()) return jash_mutation_cancel_status();
    struct stat st;
    if (fstatat(parent_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) return 1;

    if (!S_ISDIR(st.st_mode)) {
        /* Regular file or symlink — unlink the entry itself, never follow it. */
        if (dry_run) {
            jash_writef(w, ctx, "  Would delete: %s\n", display);
            return 0;
        }
        int rc = unlinkat(parent_fd, name, 0) == 0 ? 0 : 1;
        if (!rc && item) item->effect = JASH_MUT_CHANGED;
        return rc;
    }

    int fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) {
        /* A directory swapped for a symlink between fstatat and openat is data,
         * not a tree to traverse. */
        if (!dry_run && unlinkat(parent_fd, name, 0) == 0) {
            if (item) item->effect = JASH_MUT_CHANGED;
            return 0;
        }
        jash_writef(w, ctx, "rm: cannot open directory '%s'\n", display);
        return 1;
    }
    DIR* dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        jash_writef(w, ctx, "rm: cannot open directory '%s'\n", display);
        return 1;
    }

    int errors = 0;
    struct dirent* ent;
    for (;;) {
        if (jash_mutation_cancel_status()) { errors = jash_mutation_cancel_status(); break; }
        errno = 0;
        ent = readdir(dir);
        if (!ent) { if (errno) errors = 1; break; }
        if (ent->d_name[0] == '.') {
            if (ent->d_name[1] == '\0') continue;
            if (ent->d_name[1] == '.' && ent->d_name[2] == '\0') continue;
        }
        char child[1024];
        int n = snprintf(child, sizeof(child), "%s/%s", display, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) { errors++; continue; }
        if (rm_tree_at(fd, ent->d_name, child, w, ctx, dry_run, item)) errors = 1;
    }
    closedir(dir);

    if (jash_mutation_cancel_status()) return jash_mutation_cancel_status();
    if (!dry_run) {
        if (unlinkat(parent_fd, name, AT_REMOVEDIR) != 0) {
            jash_writef(w, ctx, "rm: cannot remove directory '%s'\n", display);
            errors++;
        } else if (item) item->effect = JASH_MUT_CHANGED;
    } else {
        jash_writef(w, ctx, "  Would delete directory: %s\n", display);
    }
    return errors;
}

static int rm_recursive(const char* path, jash_writer_t w, void* ctx, int dry_run,
                         jash_mutation_item_t* item) {
    /* fd-relative traversal keeps each opened directory pinned: a pathname
     * component swapped for a symlink after lstat cannot redirect recursion. */
    int cwd_fd = open(".", O_RDONLY | O_DIRECTORY);
    if (cwd_fd < 0) {
        struct stat st;
        if (lstat(path, &st) != 0) {
            jash_writef(w, ctx, "rm: cannot stat '%s'\n", path);
            return 1;
        }
        if (!S_ISDIR(st.st_mode)) {
            int rc = dry_run ? 0 : (unlink(path) == 0 ? 0 : 1);
            if (!rc && !dry_run && item) item->effect = JASH_MUT_CHANGED;
            return rc;
        }
        jash_writef(w, ctx, "rm: cannot open directory '%s'\n", path);
        return 1;
    }
    int rc = rm_tree_at(cwd_fd, path, path, w, ctx, dry_run, item);
    close(cwd_fd);
    if (rc && !dry_run) jash_writef(w, ctx, "rm: cannot remove '%s'\n", path);
    return rc;
}

/* True if `path` is an empty directory (no entries besides . and ..). Used to
 * enforce -d semantics: it removes an empty directory only, never a populated
 * one — even under trash (where a plain rename would otherwise move the whole
 * non-empty tree). */
static int rm_dir_is_empty(const char* path) {
    DIR* d = opendir(path);
    if (!d) return 0;
    int empty = 1;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' &&
            (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
            continue;
        empty = 0; break;
    }
    closedir(d);
    return empty;
}

/* Prompt on stderr and read a yes/no from stdin. Returns 1 for y/Y, else 0.
 * EOF / non-tty answer → 0 (conservative: an ignored -i is the danger we are
 * fixing, so "no answer" must mean "do not delete"). Drains the rest of the
 * line so a multi-char answer doesn't bleed into the next prompt. */
static int rm_prompt(const char* msg) {
    fputs(msg, stderr);
    fflush(stderr);
    int c = fgetc(stdin);
    int yes = (c == 'y' || c == 'Y');
    while (c != '\n' && c != EOF) c = fgetc(stdin);
    return yes;
}

/* ── Main rm builtin ──────────────────────────────────────────────────────── */

static int rm_run(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv,
                   jash_mutation_report_t* report) {
    (void)s;

    /* ADR-0036 W2 / BUG-165 root-cause fix: rm is now an argv_fn builtin, so the
     * dispatcher hands us a quote-aware, already-tokenized argv (`argv[argc]` is
     * NULL). We NO LONGER re-split a joined string on whitespace — that old
     * strtok(" \t") pass shredded a quoted filename with a space (`rm 'a b'`) into
     * two words and could delete the WRONG file. Word boundaries now survive. */
    if (argc == 0) {
        jash_writes(w, ctx, "Usage: rm [-r] [--dry-run] [--undo] <file1> [file2] ...\n");
        return 1;
    }

    /* Parse arguments */
    int recursive = 0;
    int force = 0;
    int dry_run = 0;
    /* ADR-0023 E2 safety net (BUG-027): rm is RECOVERABLE (trash + journaled
     * inverse, so undo/rewind/checkpoint cover it) when protection is on. The
     * user controls this with `trash on|off`; default is protected while
     * recording (JASH_SIGIL>=1). Plain mode keeps bash-parity permanent delete.
     * `rm --undo` still forces trash regardless. */
    int use_trash = jash_trash_protected();
    int verbose = 0;
    int interactive = 0;      /* 0 none · 1 -i (each) · 2 -I (once) */
    int remove_empty_dir = 0; /* -d: rmdir an empty directory without -r */

    /* Extract flags */
    int file_start = 0;
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            /* `--` ends option processing; every following token is a file,
             * even one beginning with '-' (`rm -- -foo`). */
            if (strcmp(argv[i], "--") == 0) { file_start = i + 1; break; }
            /* Handle combined short flags like -rf, -fr, -rfi, etc */
            if (argv[i][1] != '-') {
                for (int j = 1; argv[i][j]; j++) {
                    switch (argv[i][j]) {
                        case 'r':
                        case 'R':
                            recursive = 1;
                            break;
                        case 'f':
                            /* -f and -i override each other; last one wins
                             * (GNU). Processing left-to-right makes that fall
                             * out naturally. */
                            force = 1; interactive = 0;
                            break;
                        case 'i':
                            interactive = 1; force = 0;
                            break;
                        case 'I':
                            interactive = 2; force = 0;
                            break;
                        case 'd':
                            remove_empty_dir = 1;
                            break;
                        case 'v':
                            verbose = 1;
                            break;
                        default:
                            jash_writef(w, ctx, "rm: unknown option '-%c'\n", argv[i][j]);
                            return 1;
                    }
                }
                file_start = i + 1;
            } else {
                /* Long options */
                if (strcmp(argv[i], "--recursive") == 0) {
                    recursive = 1;
                    file_start = i + 1;
                } else if (strcmp(argv[i], "--force") == 0) {
                    force = 1; interactive = 0;
                    file_start = i + 1;
                } else if (strcmp(argv[i], "--dir") == 0) {
                    remove_empty_dir = 1;
                    file_start = i + 1;
                } else if (strcmp(argv[i], "--dry-run") == 0) {
                    dry_run = 1;
                    if (report) report->preview = 1;
                    file_start = i + 1;
                } else if (strcmp(argv[i], "--undo") == 0) {
                    use_trash = 1;
                    file_start = i + 1;
                } else if (strcmp(argv[i], "--verbose") == 0) {
                    verbose = 1;
                    file_start = i + 1;
                } else if (strcmp(argv[i], "--interactive") == 0 ||
                           strncmp(argv[i], "--interactive=", 14) == 0) {
                    if (strncmp(argv[i], "--interactive=", 14) == 0) {
                        const char* mode = argv[i] + 14;
                        if (strcmp(mode, "never") == 0) {
                            interactive = 0;
                        } else if (strcmp(mode, "once") == 0) {
                            interactive = 2; force = 0;
                        } else if (strcmp(mode, "always") == 0) {
                            interactive = 1; force = 0;
                        } else {
                            jash_writef(w, ctx, "rm: invalid interactive mode '%s'\n", mode);
                            return 1;
                        }
                    } else {
                        interactive = 1; force = 0;
                    }
                    file_start = i + 1;
                } else {
                    jash_writef(w, ctx, "rm: unknown option '%s'\n", argv[i]);
                    return 1;
                }
            }
        } else {
            file_start = i;
            break;
        }
    }
    
    if (file_start >= argc) {
        if (force) return 0;
        jash_writes(w, ctx, "rm: no files specified\n");
        return 1;
    }
    
    /* Clean up old trash entries - only when using trash mode or periodically */
    static int rm_call_count = 0;
    if (!dry_run) rm_call_count++;
    /* A preview never performs housekeeping. Explicit manifests describe only
     * requested operands, so leave unrelated expiration to trash maintenance. */
    if (!dry_run && !report && (use_trash || rm_call_count % 10 == 0)) {
        rm_cleanup_trash();
    }
    
    /* Automatic safety floor: when recording is on, drop an `auto` checkpoint
     * BEFORE deleting, so `rewind auto` (or bare `rewind`) undoes this rm even if
     * the agent never thought to checkpoint. The win is transparent — the agent
     * runs plain `rm` and gets undo for free. One-shot hint per process so the
     * agent learns the verb without noise on every rm. */
    if (!dry_run && jash_sigil_get() >= SIGIL_LOG) {
        jash_checkpoint_auto();
        static int hinted = 0;
        if (!hinted) {
            hinted = 1;
            jash_writes(jash_stderr_writer, 0,
                "\033[90m[jash] auto-checkpoint set — `rewind auto` to undo this\033[0m\n");
        }
    }

    /* Process files */
    int errors = 0;
    int deleted = 0;

    if (dry_run) {
        jash_writes(w, ctx, "Dry run (no files will be deleted):\n");
    }

    /* -I: a single up-front confirmation when removing more than three files or
     * removing recursively (GNU). Less intrusive than -i, but still stops the
     * catastrophic `rm -rf` from being silent. */
    int declined_all = 0;
    if (interactive == 2 && !dry_run) {
        int nfiles = argc - file_start;
        if (recursive || nfiles > 3) {
            char msg[128];
            snprintf(msg, sizeof msg,
                     "rm: remove %d argument%s%s? ", nfiles,
                     nfiles == 1 ? "" : "s",
                     recursive ? " recursively" : "");
            if (!rm_prompt(msg)) declined_all = 1;
        }
    }

    for (int i = file_start; i < argc; i++) {
        if (jash_mutation_report_stop(report)) return jash_mutation_report_stop(report);
        const char* path = argv[i];
        char trashed[1024] = "";
        jash_mutation_item_t item = {.source = path, .action = use_trash ? "trash" : "remove",
                                    .code = "remove_failed"};
        int rc = 1;
        if (declined_all) { item.code = "declined"; rc = 2; goto item_done; }
        
        /* Safety check */
        if (rm_is_critical(path)) {
            jash_writef(w, ctx, "rm: BLOCKED - cannot delete critical path '%s'\n", path);
            item.code = "critical_path";
            goto item_done;
        }

        /* Feature 3 guard: warn/block based on JASH_GUARD or JASH_SIGIL. */
        if (jash_guard_rm(path, recursive, w, ctx)) {
            item.code = "guard_blocked";
            goto item_done;
        }
        
        struct stat st;
        /* lstat: a symlink (even to a directory) is the entry itself — GNU rm
         * removes the LINK, and does not need -r to do so. */
        if (lstat(path, &st) != 0) {
            /* -f / --force: ignore nonexistent files silently (GNU/bash
             * semantics) instead of erroring — this also keeps `rm -rf x &&
             * ...` chains from short-circuiting just because x was already gone. */
            if (force && (errno == ENOENT || errno == ENOTDIR)) {
                rc = 2; item.code = "absent";
            } else {
                jash_writef(w, ctx, "rm: cannot access '%s'\n", path);
                item.code = "inspect_source";
            }
            goto item_done;
        }

        /* -i: confirm each removal; -I: confirmed once up front (below). -f
         * overrides (force zeroes interactive at parse time). Never in dry-run.
         * A non-tty / EOF answer is treated as "no" — -i must never silently
         * delete, which was the whole bug. */
        if (interactive == 1 && !dry_run) {
            const char* what = S_ISLNK(st.st_mode) ? "symbolic link"
                             : S_ISDIR(st.st_mode) ? "directory"
                             : "regular file";
            char msg[1200];
            snprintf(msg, sizeof msg, "rm: remove %s '%s'? ", what, path);
            if (!rm_prompt(msg)) { item.code = "declined"; rc = 2; goto item_done; }
        }

        if (S_ISDIR(st.st_mode)) {
            /* Directory */
            if (!recursive) {
                /* -d: remove an EMPTY directory (like rmdir), without -r.
                 * Refuse a populated directory even under trash (whose rename
                 * would otherwise move the whole non-empty tree). */
                if (remove_empty_dir) {
                    if (!rm_dir_is_empty(path)) {
                        jash_writef(w, ctx, "rm: cannot remove '%s': Directory not empty\n", path);
                        item.code = "directory_not_empty";
                        goto item_done;
                    }
                    if (dry_run) {
                        jash_writef(w, ctx, "  Would remove empty directory: %s\n", path);
                        rc = 0;
                    } else if (use_trash) {
                        item.effect = JASH_MUT_POSSIBLE;
                        rc = rm_move_to_trash(path, w, ctx, trashed, sizeof(trashed)) == 0 ? 0 : 1;
                        if (!rc) {
                            deleted++; rm_record_trash_undo(path, trashed, report ? &item.undo : NULL);
                            if (verbose) jash_writef(w, ctx, "removed directory '%s' (moved to trash)\n", path);
                        }
                    } else if (rmdir(path) == 0) {
                        rc = 0;
                        deleted++;
                        if (verbose) jash_writef(w, ctx, "removed directory '%s'\n", path);
                    } else {
                        jash_writef(w, ctx, "rm: cannot remove '%s': Directory not empty\n", path);
                    }
                    goto item_done;
                }
                jash_writef(w, ctx, "rm: '%s' is a directory (use -r for recursive)\n", path);
                item.code = "recursive_required";
                goto item_done;
            }

            if (dry_run) {
                rc = rm_recursive(path, w, ctx, dry_run, report ? &item : NULL) ? 1 : 0;
            } else if (use_trash) {
                item.effect = JASH_MUT_POSSIBLE;
                rc = rm_move_to_trash(path, w, ctx, trashed, sizeof(trashed)) == 0 ? 0 : 1;
                if (!rc) {
                    deleted++;
                    rm_record_trash_undo(path, trashed, report ? &item.undo : NULL);
                    if (verbose) {
                        jash_writef(w, ctx, "removed directory '%s' (moved to trash)\n", path);
                    }
                }
            } else {
                rc = rm_recursive(path, w, ctx, 0, report ? &item : NULL) ? 1 : 0;
                if (!rc) deleted++;
                if (verbose && !rc) {
                    jash_writef(w, ctx, "removed directory '%s'\n", path);
                }
            }
        } else {
            /* Regular file */
            if (dry_run) {
                long size_kb = st.st_size / 1024;
                jash_writef(w, ctx, "  Would delete: %s (%ld K)\n",
                           path, size_kb);
                rc = 0;
            } else if (use_trash) {
                item.effect = JASH_MUT_POSSIBLE;
                rc = rm_move_to_trash(path, w, ctx, trashed, sizeof(trashed)) == 0 ? 0 : 1;
                if (!rc) {
                    deleted++;
                    rm_record_trash_undo(path, trashed, report ? &item.undo : NULL);
                    if (verbose) {
                        jash_writef(w, ctx, "removed '%s' (moved to trash)\n", path);
                    }
                }
            } else {
                if (unlink(path) == 0) {
                    rc = 0;
                    deleted++;
                    if (verbose) {
                        jash_writef(w, ctx, "removed '%s'\n", path);
                    }
                } else {
                    jash_writef(w, ctx, "rm: cannot delete '%s'\n", path);
                }
            }
        }
item_done:
        if (!rc) {
            item.status = dry_run ? JASH_MUT_PLANNED : JASH_MUT_COMPLETED;
            item.effect = dry_run ? JASH_MUT_NONE : JASH_MUT_CHANGED;
            item.code = NULL;
        } else if (rc == 2) item.status = JASH_MUT_SKIPPED;
        else errors = 1;
        if (jash_mutation_report_item(report, &item) != 0) return 125;
    }
    
    /* Summary - POSIX: rm is silent on success unless -v (verbose) is specified */
    if (verbose && !dry_run && deleted > 0) {
        if (use_trash) {
            jash_writef(w, ctx, "rm: moved %d file(s) to trash (recoverable 24h)\n", deleted);
        } else {
            jash_writef(w, ctx, "rm: deleted %d file(s)\n", deleted);
        }
    }
    
    return errors ? 1 : 0;
}

int jash_b_rm(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    int filtered_count;
    char** filtered;
    jash_mutation_format_t format;
    int json = jash_mutation_args_mode(argc, argv, NULL, &filtered_count, &filtered, &format);
    int args_error = errno;
    if (!json) return rm_run(s, w, ctx, argc, argv, NULL);
    jash_mutation_report_t report;
    if (jash_mutation_report_begin_mode(&report, "rm", "operand_tree", format, w, ctx) != 0) { free(filtered); return 125; }
    if (json < 0) { report.error_code = args_error == ENOMEM ? "out_of_memory" : "invalid_format_options"; return jash_mutation_report_end(&report, 2); }
    int rc = rm_run(s, jash_stderr_writer, NULL, filtered_count, filtered, &report);
    free(filtered);
    return jash_mutation_report_end(&report, rc);
}
