// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — error_exp — inline error explanation (impl).
//
// Pattern-matches common error messages and provides inline help.
// No AI needed — just smart string matching on stderr.

#include "error_exp.h"
#include <string.h>

static int contains(const char* haystack, const char* needle) {
    if (!haystack || !needle) return 0;
    return strstr(haystack, needle) != 0;
}

static void safe_copy(char* dst, const char* src, unsigned cap) {
    if (!dst || !src || cap == 0) return;
    unsigned i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

int jash_error_explain(const char* cmd, int exit_code,
                       const char* stderr_buf,
                       char* out, unsigned out_cap) {
    if (!out || out_cap == 0) return 0;
    out[0] = '\0';

    /* Command not found */
    if (exit_code == 127) {
        safe_copy(out, "Command not found. Check spelling or install it.", out_cap);
        return 1;
    }

    /* Permission denied */
    if (exit_code == 126 || (stderr_buf && contains(stderr_buf, "Permission denied"))) {
        safe_copy(out, "Permission denied. Try: chmod +x ", out_cap);
        if (cmd) {
            unsigned len = strlen(out);
            unsigned i = 0;
            while (cmd[i] && len + i < out_cap - 1) { out[len + i] = cmd[i]; i++; }
            out[len + i] = '\0';
        }
        return 1;
    }

    /* Segfault / signal */
    if (exit_code >= 139 && exit_code <= 143) {
        const char* sigs[] = {
            "Segmentation fault (core dumped)",
            "Illegal instruction",
            "Bus error",
            "User defined signal 1",
            "User defined signal 2",
        };
        int sig = exit_code - 128;
        if (sig >= 1 && sig <= 5) {
            safe_copy(out, sigs[sig - 1], out_cap);
            return 1;
        }
    }

    /* Git-specific */
    if (cmd && strcmp(cmd, "git") == 0) {
        if (stderr_buf && contains(stderr_buf, "not a git repository")) {
            safe_copy(out, "Not a git repo. Run: git init", out_cap);
            return 1;
        }
        if (stderr_buf && contains(stderr_buf, "did you forget to push")) {
            safe_copy(out, "Remote has changes. Try: git pull --rebase", out_cap);
            return 1;
        }
        if (stderr_buf && contains(stderr_buf, "merge conflict")) {
            safe_copy(out, "Merge conflict. Edit conflicted files, then: git add . && git commit", out_cap);
            return 1;
        }
        if (stderr_buf && contains(stderr_buf, "pathspec") && contains(stderr_buf, "did not match")) {
            safe_copy(out, "File not tracked. Did you mean: git add?", out_cap);
            return 1;
        }
    }

    /* Docker-specific */
    if (cmd && strcmp(cmd, "docker") == 0) {
        if (stderr_buf && contains(stderr_buf, "Cannot connect to the Docker daemon")) {
            safe_copy(out, "Docker daemon not running. Start Docker Desktop or: sudo systemctl start docker", out_cap);
            return 1;
        }
        if (stderr_buf && contains(stderr_buf, "No such image")) {
            safe_copy(out, "Image not found. Pull it first: docker pull <image>", out_cap);
            return 1;
        }
    }

    /* npm/node */
    if (cmd && (strcmp(cmd, "npm") == 0 || strcmp(cmd, "node") == 0)) {
        if (stderr_buf && contains(stderr_buf, "MODULE_NOT_FOUND")) {
            safe_copy(out, "Module not found. Run: npm install", out_cap);
            return 1;
        }
        if (stderr_buf && contains(stderr_buf, "EACCES")) {
            safe_copy(out, "Permission error. Try: sudo chown -R $(whoami) ~/.npm", out_cap);
            return 1;
        }
    }

    /* Python */
    if (cmd && (strcmp(cmd, "python") == 0 || strcmp(cmd, "python3") == 0 ||
                strcmp(cmd, "pip") == 0 || strcmp(cmd, "pip3") == 0)) {
        if (stderr_buf && contains(stderr_buf, "No module named")) {
            safe_copy(out, "Module not installed. Try: pip install <module>", out_cap);
            return 1;
        }
        if (stderr_buf && contains(stderr_buf, "SyntaxError")) {
            safe_copy(out, "Syntax error in Python code. Check indentation and syntax.", out_cap);
            return 1;
        }
    }

    /* Cargo/rust */
    if (cmd && strcmp(cmd, "cargo") == 0) {
        if (stderr_buf && contains(stderr_buf, "could not compile")) {
            safe_copy(out, "Compilation failed. Check error messages above.", out_cap);
            return 1;
        }
    }

    /* Generic: exit code non-zero */
    if (exit_code != 0) {
        safe_copy(out, "Command failed with exit code ", out_cap);
        char num[8];
        int v = exit_code, i = 0;
        if (v >= 100) num[i++] = '0' + v / 100;
        if (v >= 10) num[i++] = '0' + (v / 10) % 10;
        num[i++] = '0' + v % 10;
        num[i] = '\0';
        unsigned len = strlen(out);
        unsigned j = 0;
        while (num[j] && len + j < out_cap - 1) { out[len + j] = num[j]; j++; }
        out[len + j] = '\0';
        return 1;
    }

    return 0;
}

int jash_error_suggest_fix(const char* cmd, int exit_code,
                           const char* stderr_buf,
                           char* out, unsigned out_cap) {
    if (!out || out_cap == 0) return 0;
    out[0] = '\0';

    /* Common typos */
    if (cmd && strcmp(cmd, "gt") == 0) {
        safe_copy(out, "Did you mean: git?", out_cap);
        return 1;
    }
    if (cmd && strcmp(cmd, "gi") == 0) {
        safe_copy(out, "Did you mean: git?", out_cap);
        return 1;
    }
    if (cmd && strcmp(cmd, "dkocker") == 0) {
        safe_copy(out, "Did you mean: docker?", out_cap);
        return 1;
    }
    if (cmd && strcmp(cmd, "crgo") == 0) {
        safe_copy(out, "Did you mean: cargo?", out_cap);
        return 1;
    }

    return 0;
}
