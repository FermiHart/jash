// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_REPO_IDENTITY_H
#define JASH_REPO_IDENTITY_H
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A directory with HEAD, or Git's worktree gitdir indirection. A marker
 * alone is insufficient: an unrelated .git file must not merge evidence. */
static inline int jash_repo_marker_valid(const char* directory, const char* marker) {
    struct stat st;
    if (stat(marker, &st)) return 0;
    char gitdir[PATH_MAX], head[PATH_MAX];
    if (S_ISDIR(st.st_mode)) {
        int n = snprintf(gitdir, sizeof gitdir, "%s", marker);
        if (n < 0 || (size_t)n >= sizeof gitdir) return 0;
    } else if (S_ISREG(st.st_mode)) {
        FILE* file = fopen(marker, "r");
        if (!file) return 0;
        char line[PATH_MAX];
        int valid = fgets(line, sizeof line, file) != NULL;
        int ended = valid && (strchr(line, '\n') != NULL || feof(file));
        if (ferror(file)) valid = 0;
        if (fclose(file)) valid = 0;
        if (!valid || !ended || strncmp(line, "gitdir: ", 8)) return 0;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[8]) return 0;
        int n = line[8] == '/'
            ? snprintf(gitdir, sizeof gitdir, "%s", line + 8)
            : snprintf(gitdir, sizeof gitdir, "%s/%s", directory, line + 8);
        if (n < 0 || (size_t)n >= sizeof gitdir) return 0;
    } else return 0;
    int n = snprintf(head, sizeof head, "%s/HEAD", gitdir);
    return n >= 0 && (size_t)n < sizeof head && !stat(head, &st) &&
           S_ISREG(st.st_mode) && st.st_size > 0;
}

/* Shared by the host and separately linked sigil module. Callers cache the
 * result independently to preserve the established session attribution. */
static inline void jash_repo_identity(char key[18]) {
    key[0] = '-'; key[1] = 0;
    char cwd[PATH_MAX], probe[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) return;
    for (;;) {
        int n = snprintf(probe, sizeof probe, "%s/.git", cwd);
        if (n < 0 || (size_t)n >= sizeof probe) return;
        if (jash_repo_marker_valid(cwd, probe)) {
            unsigned long long hash = 1469598103934665603ull;
            for (const unsigned char* p = (const unsigned char*)cwd; *p; p++) {
                hash ^= *p;
                hash *= 1099511628211ull;
            }
            snprintf(key, 18, "%016llx", hash);
            return;
        }
        if (!strcmp(cwd, "/")) return;
        char* slash = strrchr(cwd, '/');
        if (!slash) return;
        if (slash == cwd) slash[1] = 0;
        else *slash = 0;
    }
}
#endif
