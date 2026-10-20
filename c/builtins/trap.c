// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Builtin: trap — handle signals + pseudo-signals (DEBUG, RETURN, EXIT)
//
// Beyond note: signal()/sigaction() are stubs (return 0) on Beyond.
// The kernel delivers SIGINT via the NU nervous system when Ctrl-C is
// pressed on serial — sys_read returns -EINTR. jash handles EINTR in
// the stdin reader (main.c) and the readline loop (loop.c) directly.
// The trap table is still maintained for `trap -l` and internal use.

#include "builtin.h"
#include "shell_output.h"
#include <core/argv.h>
#include <parser/eval.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <errno.h>

/* Pseudo-signal constants for DEBUG / RETURN / EXIT (not real OS signals). */
#define JASH_TRAP_DEBUG  -1
#define JASH_TRAP_RETURN -2
#define JASH_TRAP_EXIT   -3
#define JASH_TRAP_ERR    -4

/* We store trap handlers in simple global state so they persist */
#define MAX_TRAPS 16
static struct { int sig; char cmd[128]; } traps[MAX_TRAPS];
static int trap_count = 0;
static volatile sig_atomic_t pending_traps[64];
static const char* const sig_names[] = {
    "EXIT", "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS",
    "FPE", "KILL", "USR1", "SEGV", "USR2", "PIPE", "ALRM", "TERM",
    "STKFLT", "CHLD", "CONT", "STOP", "TSTP", "TTIN", "TTOU", "URG",
    "XCPU", "XFSZ", "VTALRM", "PROF", "WINCH", "IO", "PWR", "SYS",
    NULL
};

static void trap_signal_handler(int sig) {
    if (sig > 0 && sig < 64) pending_traps[sig] = 1;
}

int jash_run_pending_traps(jash_state_t* s, jash_writer_t w, void* ctx) {
    int ran = 0;
    for (int sig = 1; sig < 64; sig++) {
        if (!pending_traps[sig]) continue;
        pending_traps[sig] = 0;
        for (int i = 0; i < trap_count; i++) {
            if (traps[i].sig == sig && traps[i].cmd[0]) {
                ran = 1;
                jash_eval_line(s, w, ctx, traps[i].cmd);
            }
        }
    }
    return ran;
}

/* Run DEBUG trap (before each command).
 * Uses re-entrancy guard to prevent infinite recursion. */
static int debug_reentrant = 0;
int jash_run_trap_debug(jash_state_t* s, jash_writer_t w, void* ctx) {
    if (debug_reentrant) return 0;
    debug_reentrant = 1;
    for (int i = 0; i < trap_count; i++) {
        if (traps[i].sig == JASH_TRAP_DEBUG && traps[i].cmd[0]) {
            jash_eval_line(s, w, ctx, traps[i].cmd);
        }
    }
    debug_reentrant = 0;
    return 0;
}

/* Run RETURN trap (when function/script body finishes).
 * Uses re-entrancy guard to prevent infinite recursion. */
static int return_reentrant = 0;
int jash_run_trap_return(jash_state_t* s, jash_writer_t w, void* ctx) {
    if (return_reentrant) return 0;
    return_reentrant = 1;
    for (int i = 0; i < trap_count; i++) {
        if (traps[i].sig == JASH_TRAP_RETURN && traps[i].cmd[0]) {
            jash_eval_line(s, w, ctx, traps[i].cmd);
        }
    }
    return_reentrant = 0;
    return 0;
}

/* Run EXIT trap (shell is about to exit).
 * Uses re-entrancy guard to prevent infinite recursion. */
static int exit_reentrant = 0;
int jash_run_trap_exit(jash_state_t* s, jash_writer_t w, void* ctx) {
    if (exit_reentrant) return 0;
    exit_reentrant = 1;
    int saved_status = s->last_status.code;
    int saved_exit = s->exit_requested;
    /* The pending shell exit must not stop the handler's command list.
     * A new exit request from the handler, however, overrides its caller. */
    s->exit_requested = 0;
    for (int i = 0; i < trap_count; i++) {
        if (traps[i].sig == JASH_TRAP_EXIT && traps[i].cmd[0]) {
            /* The action may reset/redefine its own slot while executing. */
            char cmd[sizeof(traps[i].cmd)];
            memcpy(cmd, traps[i].cmd, sizeof cmd);
            jash_eval_line(s, w, ctx, cmd);
            break;
        }
    }
    if (!s->exit_requested) s->last_status.code = saved_status;
    s->exit_requested |= saved_exit;
    exit_reentrant = 0;
    return 0;
}

/* Run ERR trap (when a command fails with non-zero exit status).
 * Uses re-entrancy guard to prevent infinite recursion. */
static int err_reentrant = 0;
int jash_run_trap_err(jash_state_t* s, jash_writer_t w, void* ctx) {
    if (err_reentrant) return 0;
    err_reentrant = 1;
    for (int i = 0; i < trap_count; i++) {
        if (traps[i].sig == JASH_TRAP_ERR && traps[i].cmd[0]) {
            jash_eval_line(s, w, ctx, traps[i].cmd);
        }
    }
    err_reentrant = 0;
    return 0;
}

static int signal_number(const char* sig_s) {
    int sig = 0;
    if (!sig_s || !*sig_s) return 0;
    if (sig_s[0] >= 'A' && sig_s[0] <= 'Z') {
        const char *n = sig_s;
        if (sig_s[0] == 'S' && sig_s[1] == 'I' && sig_s[2] == 'G') n = sig_s + 3;
        if (strcmp(n, "DEBUG") == 0) sig = JASH_TRAP_DEBUG;
        else if (strcmp(n, "RETURN") == 0) sig = JASH_TRAP_RETURN;
        else if (strcmp(n, "ERR") == 0) sig = JASH_TRAP_ERR;
        else {
            for (int i = 0; sig_names[i]; i++)
                if (strcmp(n, sig_names[i]) == 0) return i ? i : JASH_TRAP_EXIT;
            return 0;
        }
    } else {
        int ci = 0;
        for (; sig_s[ci] >= '0' && sig_s[ci] <= '9'; ci++) {
            sig = sig * 10 + (sig_s[ci] - '0');
            if (sig > 63) return 0;
        }
        /* A command token such as "9;" is not a signal operand. */
        if (sig_s[ci]) return 0;
        if (sig == 0) return JASH_TRAP_EXIT;
    }
    return sig;
}

int jash_trap_resolve(const char* name) { return signal_number(name); }

const char* jash_trap_name(int sig) {
    if (sig == JASH_TRAP_EXIT) return "EXIT";
    if (sig == JASH_TRAP_DEBUG) return "DEBUG";
    if (sig == JASH_TRAP_RETURN) return "RETURN";
    if (sig == JASH_TRAP_ERR) return "ERR";
    return sig > 0 && sig < (int)(sizeof(sig_names) / sizeof(*sig_names)) - 1
        ? sig_names[sig] : NULL;
}

const char* jash_trap_action(int sig) {
    for (int i = 0; i < trap_count; i++)
        if (traps[i].sig == sig) return traps[i].cmd;
    return NULL;
}

/* The table describes a successfully installed disposition. Do not publish a
 * handler before the kernel accepted it or silently truncate its action. */
int jash_trap_update(int sig, const char* action) {
    if (!sig || sig > 63 || sig < JASH_TRAP_ERR) return EINVAL;
    if (sig == SIGKILL || sig == SIGSTOP) return ENOTSUP;
    if (action && strlen(action) >= sizeof(traps[0].cmd)) return E2BIG;
    int slot = -1;
    for (int i = 0; i < trap_count; i++)
        if (traps[i].sig == sig) { slot = i; break; }
    if (action && slot < 0 && trap_count == MAX_TRAPS) return ENOSPC;
    if (sig > 0) {
        struct sigaction disposition;
        memset(&disposition, 0, sizeof disposition);
        disposition.sa_handler = !action ? SIG_DFL : !*action ? SIG_IGN : trap_signal_handler;
        sigemptyset(&disposition.sa_mask);
        if (sigaction(sig, &disposition, NULL)) return errno ? errno : EIO;
    }
    if (!action) {
        if (slot >= 0) {
            for (int i = slot; i + 1 < trap_count; i++) traps[i] = traps[i + 1];
            trap_count--;
        }
    } else {
        if (slot < 0) { slot = trap_count++; traps[slot].sig = sig; }
        memcpy(traps[slot].cmd, action, strlen(action) + 1);
    }
    return 0;
}

static void print_trap(int slot, jash_writer_t w, void* ctx) {
    jash_writes(w, ctx, "trap -- '");
    for (const char* p = traps[slot].cmd; *p; p++) {
        if (*p == '\'') jash_writes(w, ctx, "'\\''");
        else w(ctx, p, 1);
    }
    jash_writes(w, ctx, "' ");
    int sig = traps[slot].sig;
    const char* name = sig == JASH_TRAP_EXIT ? "EXIT" :
                       sig == JASH_TRAP_DEBUG ? "DEBUG" :
                       sig == JASH_TRAP_RETURN ? "RETURN" :
                       sig == JASH_TRAP_ERR ? "ERR" : NULL;
    if (name) jash_writes(w, ctx, name);
    else if (sig > 0 && sig < (int)(sizeof(sig_names) / sizeof(sig_names[0])) - 1)
        jash_writef(w, ctx, "SIG%s", sig_names[sig]);
    else jash_writef(w, ctx, "%d", sig);
    jash_writes(w, ctx, "\n");
}

size_t jash_trap_count(void) { return (size_t)trap_count; }
int jash_trap_at(size_t index, int* signal_number_out, const char** action) {
    if (index >= (size_t)trap_count) return 0;
    if (signal_number_out) *signal_number_out = traps[index].sig;
    if (action) *action = traps[index].cmd;
    return 1;
}

int jash_b_trap(jash_state_t* s, jash_writer_t w, void* ctx,
                const char* args) {
    int structured = jash_shell_state_try(s, w, ctx, "trap", args);
    if (structured != JASH_SHELL_TEXT) return structured;
    (void)s;
    /* Dispatchers must preserve quoting: the action is ONE word, regardless
     * of spaces, signal names or numbers within its command text. */
    jash_argv_t words;
    if (jash_argv_build(args, &words) < 0) {
        jash_writes(jash_stderr_writer, 0, "trap: invalid quoting or out of memory\n");
        return 2;
    }
    int argi = 0, list = 0, print = 0, rc = 0;
    while (argi < words.argc && words.argv[argi][0] == '-' && words.argv[argi][1]) {
        const char* opt = words.argv[argi++];
        if (strcmp(opt, "--") == 0) break;
        for (int k = 1; opt[k]; k++) {
            if (opt[k] == 'l') list = 1;
            else if (opt[k] == 'p') print = 1;
            else {
                jash_writef(jash_stderr_writer, 0, "trap: %s: invalid option\n", opt);
                rc = 2;
                goto done;
            }
        }
    }
    if (list) {
        for (int i = 0; sig_names[i]; i++)
            jash_writef(w, ctx, "%2d) %s\n", i, sig_names[i]);
        goto done;
    }
    if (argi == words.argc) {
        for (int i = 0; i < trap_count; i++) print_trap(i, w, ctx);
        goto done;
    }

    const char* action = NULL;
    if (!print) {
        const char* first = words.argv[argi];
        int sig = signal_number(first);
        /* An omitted action resets signals: `trap EXIT` or `trap 2 15`. */
        if (!(sig && (argi + 1 == words.argc ||
                      (first[0] >= '0' && first[0] <= '9')))) {
            action = words.argv[argi++];
            if (argi == words.argc) {
                jash_writes(jash_stderr_writer, 0,
                            "trap: usage: trap [-lp] [[action] signal ...]\n");
                rc = 2;
                goto done;
            }
            if (strcmp(action, "-") == 0) action = NULL;
        }
        if (action && strlen(action) >= sizeof(traps[0].cmd)) {
            jash_writes(jash_stderr_writer, 0, "trap: action too long -- refusing\n");
            rc = 1;
            goto done;
        }
    }

    for (; argi < words.argc; argi++) {
        int sig = signal_number(words.argv[argi]);
        if (!sig) {
            jash_writef(jash_stderr_writer, 0, "trap: %s: invalid signal\n", words.argv[argi]);
            rc = 1;
            continue;
        }
        int slot = -1;
        for (int i = 0; i < trap_count; i++) {
            if (traps[i].sig == sig) { slot = i; break; }
        }
        if (print) {
            if (slot >= 0) print_trap(slot, w, ctx);
            continue;
        }
        if (sig == 9) continue;  /* can't trap KILL */
        if (!action) {
            if (slot >= 0) {
                for (int i = slot; i + 1 < trap_count; i++) traps[i] = traps[i + 1];
                trap_count--;
                if (sig > 0) signal(sig, SIG_DFL);
            }
        } else {
            if (slot < 0) {
                if (trap_count == MAX_TRAPS) {
                    jash_writes(jash_stderr_writer, 0, "trap: too many handlers -- refusing\n");
                    rc = 1;
                    continue;
                }
                slot = trap_count++;
                traps[slot].sig = sig;
            }
            memcpy(traps[slot].cmd, action, strlen(action) + 1);
            if (sig > 0) signal(sig, *action ? trap_signal_handler : SIG_IGN);
        }
    }
done:
    jash_argv_free(&words);
    return rc;
}
