// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/contract — run a command and assert output contracts (ADR-0026 Tier 2).
//
// Usage:
//   contract --expect rc 0 --expect stdout-contains "ok" -- <cmd>
//   contract --expect stdout-empty --expect stderr-empty -- <cmd>
//
// The command is executed; its stdout/stderr are captured and checked against
// every --expect clause. If any clause fails, contract exits with a non-zero
// status and prints a diagnostic.
//
// Supported expectations:
//   rc N                  exit code must equal N
//   stdout-contains STR   stdout must contain STR
//   stdout-empty          stdout must have zero bytes
//   stdout-matches REGEX  (stub: falls back to contains)
//   stderr-empty          stderr must have zero bytes
//   stderr-contains STR

#include "builtin.h"
#include "analysis_capture.h"
#include <theme/io.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <regex.h>

#define CONTRACT_MAX_CAPTURE (1 << 20)
#define CONTRACT_MAX_ASSERTS 32

typedef enum { EXP_RC, EXP_STDOUT_CONTAINS, EXP_STDOUT_EMPTY, EXP_STDOUT_MATCHES,
               EXP_STDERR_CONTAINS, EXP_STDERR_EMPTY } exp_kind_t;

typedef struct {
    exp_kind_t kind;
    int rc;
    char str[512];
} assert_t;

static int contract_next_token(const char* s, size_t* pos, char* out, size_t cap) {
    const char* p = s + *pos;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) { *pos = p - s; out[0] = '\0'; return 0; }

    char quote = 0;
    if (*p == '"' || *p == '\'') quote = *p++;

    size_t o = 0;
    while (*p) {
        if (quote) {
            if (*p == quote) { p++; quote = 0; break; }
        } else {
            if (*p == ' ' || *p == '\t') break;
        }
        if (o + 1 < cap) out[o++] = *p;
        p++;
    }
    out[o] = '\0';
    if (quote) return -1;                /* unterminated quoted token */
    *pos = p - s;
    return 1;
}

static int contract_capture(char* const argv[], char** out, long* out_len,
                            char** err, long* err_len, int* rc_out) {
    analysis_capture_t capture;
    if (analysis_capture_argv(argv, CONTRACT_MAX_CAPTURE, &capture)) {
        analysis_capture_free(&capture); return -1;
    }
    *out = (char*)capture.bytes[0]; *err = (char*)capture.bytes[1];
    *out_len = (long)capture.size[0]; *err_len = (long)capture.size[1];
    *rc_out = capture.execution.command_status;
    return 0;
}

static int contract_check(const assert_t* a, int rc, const char* out, long out_len,
                          const char* err, long err_len, jash_writer_t w, void* ctx) {
    switch (a->kind) {
        case EXP_RC:
            if (rc != a->rc) {
                jash_writef(w, ctx, "contract failed: expected rc=%d, got rc=%d\n", a->rc, rc);
                return 1;
            }
            break;
        case EXP_STDOUT_CONTAINS:
            if (!memmem(out, out_len, a->str, strlen(a->str))) {
                jash_writef(w, ctx, "contract failed: stdout does not contain '%s'\n", a->str);
                return 1;
            }
            break;
        case EXP_STDOUT_MATCHES: {
            regex_t re;
            int rerr = regcomp(&re, a->str, REG_EXTENDED | REG_NOSUB);
            if (rerr != 0) {
                char ebuf[256];
                regerror(rerr, &re, ebuf, sizeof ebuf);
                jash_writef(w, ctx, "contract failed: invalid regex '%s': %s\n", a->str, ebuf);
                return 1;
            }
            int matched = (regexec(&re, out, 0, NULL, 0) == 0);
            regfree(&re);
            if (!matched) {
                jash_writef(w, ctx, "contract failed: stdout does not match regex '%s'\n", a->str);
                return 1;
            }
            break;
        }
        case EXP_STDOUT_EMPTY:
            if (out_len != 0) {
                jash_writef(w, ctx, "contract failed: expected stdout empty, got %ld bytes\n", out_len);
                return 1;
            }
            break;
        case EXP_STDERR_CONTAINS:
            if (!memmem(err, err_len, a->str, strlen(a->str))) {
                jash_writef(w, ctx, "contract failed: stderr does not contain '%s'\n", a->str);
                return 1;
            }
            break;
        case EXP_STDERR_EMPTY:
            if (err_len != 0) {
                jash_writef(w, ctx, "contract failed: expected stderr empty, got %ld bytes\n", err_len);
                return 1;
            }
            break;
    }
    return 0;
}

int jash_b_contract(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)s;
    assert_t asserts[CONTRACT_MAX_ASSERTS];
    int nasserts = 0;
    memset(asserts, 0, sizeof(asserts));

    char* argv[256];
    int argc = 0;
    for (int i = 0; i < 256; i++) argv[i] = NULL;

    int dashdash = 0;
    size_t pos = 0;
    char tok[4096];
    while (contract_next_token(args, &pos, tok, sizeof(tok)) > 0) {
        if (!dashdash && strcmp(tok, "--") == 0) { dashdash = 1; continue; }
        if (!dashdash && strcmp(tok, "--expect") == 0) {
            char kind[256];
            if (contract_next_token(args, &pos, kind, sizeof(kind)) <= 0) {
                jash_writes(w, ctx, "contract: --expect requires a kind\n");
                return 1;
            }
            if (nasserts >= CONTRACT_MAX_ASSERTS) {
                jash_writes(w, ctx, "contract: too many --expect clauses\n");
                return 1;
            }
            assert_t* a = &asserts[nasserts++];
            if (strcmp(kind, "rc") == 0) {
                char val[64];
                if (contract_next_token(args, &pos, val, sizeof(val)) <= 0) {
                    jash_writes(w, ctx, "contract: --expect rc requires a number\n");
                    return 1;
                }
                a->kind = EXP_RC; a->rc = atoi(val);
            } else if (strcmp(kind, "stdout-contains") == 0) {
                char val[4096];
                if (contract_next_token(args, &pos, val, sizeof(val)) <= 0) {
                    jash_writes(w, ctx, "contract: --expect stdout-contains requires a string\n");
                    return 1;
                }
                a->kind = EXP_STDOUT_CONTAINS; strlcpy(a->str, val, sizeof(a->str));
            } else if (strcmp(kind, "stdout-empty") == 0) {
                a->kind = EXP_STDOUT_EMPTY;
            } else if (strcmp(kind, "stdout-matches") == 0) {
                char val[4096];
                if (contract_next_token(args, &pos, val, sizeof(val)) <= 0) {
                    jash_writes(w, ctx, "contract: --expect stdout-matches requires a regex\n");
                    return 1;
                }
                a->kind = EXP_STDOUT_MATCHES; strlcpy(a->str, val, sizeof(a->str));
            } else if (strcmp(kind, "stderr-contains") == 0) {
                char val[4096];
                if (contract_next_token(args, &pos, val, sizeof(val)) <= 0) {
                    jash_writes(w, ctx, "contract: --expect stderr-contains requires a string\n");
                    return 1;
                }
                a->kind = EXP_STDERR_CONTAINS; strlcpy(a->str, val, sizeof(a->str));
            } else if (strcmp(kind, "stderr-empty") == 0) {
                a->kind = EXP_STDERR_EMPTY;
            } else {
                jash_writef(w, ctx, "contract: unknown expectation kind '%s'\n", kind);
                return 1;
            }
            continue;
        }
        if (argc >= 255) {
            jash_writes(w, ctx, "contract: too many command arguments\n");
            return 1;
        }
        argv[argc] = strdup(tok);
        if (!argv[argc]) {
            for (int i = 0; i < argc; i++) free(argv[i]);
            return 1;
        }
        argc++;
    }
    argv[argc] = NULL;

    if (argc == 0) {
        for (int i = 0; i < argc; i++) free(argv[i]);
        jash_writes(w, ctx,
            "contract — assert command output contracts\n"
            "Usage: contract --expect <kind> [value] [--expect ...] [--] <command>\n"
            "Kinds: rc N, stdout-contains STR, stdout-empty, stdout-matches REGEX, "
            "stderr-contains STR, stderr-empty\n");
        return 1;
    }

    char *out = NULL, *err = NULL;
    long out_len = 0, err_len = 0;
    int rc = 0;
    if (contract_capture(argv, &out, &out_len, &err, &err_len, &rc) < 0) {
        for (int i = 0; i < argc; i++) free(argv[i]);
        jash_writes(w, ctx, "contract: failed to run command\n");
        return 1;
    }

    int failed = 0;
    for (int i = 0; i < nasserts; i++) {
        if (contract_check(&asserts[i], rc, out, out_len, err, err_len, w, ctx)) {
            failed = 1;
            break;
        }
    }

    free(out); free(err);
    for (int i = 0; i < argc; i++) free(argv[i]);
    if (!failed) jash_writes(w, ctx, "contract: ok\n");
    return failed ? 2 : 0;
}

#include "contract_native.inc"
