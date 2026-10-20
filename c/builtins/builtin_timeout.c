// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: builtins/timeout — run a command under a wall-clock limit.
//
// Why: stock macOS ships NO `timeout` at all (it's GNU coreutils; macOS
// needs `brew install coreutils` for `gtimeout`), so an agent that
// reflexively writes `timeout 30 some-command` — a very common habit — gets
// "command not found" on a fresh machine. This is a real, frequent
// portability gap, not a hypothetical one (ADR-0044).
//
// Scope: DURATION [s/m/h/d suffix, fractional] COMMAND [ARGS...]. Argv is
// NEVER permuted — every flag `timeout` recognizes must appear BEFORE
// DURATION; everything from COMMAND onward (including anything that looks
// like a flag, e.g. `timeout 5 ls -la`) is handed to execve() untouched.
//
// The wrapped command becomes its own process-group leader (setpgid before
// execve) and the timeout/kill-after signals target the WHOLE group, not
// just the direct child — otherwise `timeout 5 sh -c 'sleep 100'` would
// orphan the grandchild `sleep` instead of actually stopping the work.
//
// v1 boundary (deliberate, not a silent gap): the wrapped command's stdout/
// stderr are the OS-level fds it inherits directly from jash — they are NOT
// routed through this builtin's `w` writer, so the generic per-builtin
// output-budget governor (which only wraps a builtin's own `w` writes) does
// not cap them. This matches every other directly-forked external command
// in this codebase, and matches real GNU `timeout` (also does zero output
// processing). `timeout`'s OWN messages (usage errors, --verbose notices,
// --json summary) DO go through `w` and ARE governed normally.

#include "builtin.h"
#include "discovery_output.h"
#include <core/structured_output.h>
#include <core/data_json.h>
#include <math.h>
#include <limits.h>
#include <core/agent_json.h>   /* jash_json_esc (JINV-058) */
#include <core/env.h>
#include <core/agent_cancel.h>
#include <parser/exec.h>
#include <sys/syscalls.h>
#include <signal.h>
#include <strings.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/socket.h>

/* ── Signal name table (small local subset, mirrors builtin_kill.c's table —
 * self-contained per this codebase's existing per-file-helper convention). */
typedef struct { int num; const char* name; } to_sig_entry_t;
static const to_sig_entry_t TO_SIGNALS[] = {
    { SIGHUP, "HUP" }, { SIGINT, "INT" }, { SIGQUIT, "QUIT" },
    { SIGKILL, "KILL" }, { SIGUSR1, "USR1" }, { SIGUSR2, "USR2" },
    { SIGTERM, "TERM" }, { SIGALRM, "ALRM" }, { SIGCONT, "CONT" },
    { 0, NULL }
};

static int to_sig_by_name(const char* name) {
    const char* n = name;
    if (strncasecmp(n, "SIG", 3) == 0) n += 3;
    for (int i = 0; TO_SIGNALS[i].name; i++)
        if (strcasecmp(TO_SIGNALS[i].name, n) == 0) return TO_SIGNALS[i].num;
    return -1;
}

static const char* to_sig_name(int sig) {
    for (int i = 0; TO_SIGNALS[i].name; i++)
        if (TO_SIGNALS[i].num == sig) return TO_SIGNALS[i].name;
    return "?";
}

/* Parse "-s SIGNAME" / "-s 9" style values. Returns -1 on failure. */
static int to_parse_signal_arg(const char* a) {
    if (!a || !*a) return -1;
    int all_digits = 1;
    for (const char* p = a; *p; p++) if (*p < '0' || *p > '9') { all_digits = 0; break; }
    if (all_digits) { int n = atoi(a); return (n > 0 && n < 64) ? n : -1; }
    return to_sig_by_name(a);
}

/* DURATION: numeric part (decimal allowed) + optional trailing unit
 * s(default)/m/h/d, GNU semantics. Returns 0 on success, -1 on failure. */
static int to_parse_duration(const char* s, double* out) {
    if (!s || !*s) return -1;
    char* end = NULL;
    double v = strtod(s, &end);
    if (end == s || v < 0 || !isfinite(v)) return -1;
    double mult = 1.0;
    if (*end) {
        if (end[1] != '\0') return -1;  /* only a single trailing unit char */
        switch (*end) {
            case 's': mult = 1.0; break;
            case 'm': mult = 60.0; break;
            case 'h': mult = 3600.0; break;
            case 'd': mult = 86400.0; break;
            default: return -1;
        }
    }
    if (!isfinite(v * mult) || v * mult > (double)(LONG_MAX / 1000000L)) return -1;
    *out = v * mult;
    return 0;
}

/* Decode a wait4 status into a shell-style exit code — same 3-line
 * convention as c/parser/exec.c's (static, unexported) decode_wait_status. */
static int to_decode_status(int status) {
    if ((status & 0x7F) == 0) return (status >> 8) & 0xFF;      /* exited */
    if ((status & 0x7F) != 0x7F) return 128 + (status & 0x7F);  /* signaled */
    return 128;                                                  /* stopped */
}

/* Build an envp array from jash's OWN tracked env table (s->env), NOT the
 * raw OS environ — jash keeps these separate (see c/parser/exec.c's
 * build_envp), so a wrapped command must see the same environment a normal
 * external command would, including anything the session `export`ed that
 * never touched the OS-level environ. */
typedef struct { char* buffer; char** envp; } to_envp_t;
static int to_build_envp(to_envp_t* out, const jash_state_t* state) {
    memset(out, 0, sizeof(*out));
    uint32_t count = jash_env_count(state);
    size_t bytes = 0;
    for (uint32_t i = 0; i < count; i++) {
        const char *key, *value;
        if (!jash_env_at(state, i, &key, &value)) return -1;
        size_t n = strlen(key) + strlen(value) + 2;
        if (n > 32u * 1024u * 1024u - bytes) return -1;
        bytes += n;
    }
    out->buffer = malloc(bytes ? bytes : 1);
    out->envp = calloc((size_t)count + 1, sizeof(*out->envp));
    if (!out->buffer || !out->envp) { free(out->buffer); free(out->envp); return -1; }
    size_t used = 0;
    for (uint32_t i = 0; i < count; i++) {
        const char *key, *value;
        if (!jash_env_at(state, i, &key, &value)) { free(out->buffer); free(out->envp); return -1; }
        size_t a = strlen(key), b = strlen(value);
        out->envp[i] = out->buffer + used;
        memcpy(out->buffer + used, key, a); used += a;
        out->buffer[used++] = '=';
        memcpy(out->buffer + used, value, b); used += b;
        out->buffer[used++] = 0;
    }
    return 0;
}

static int timeout_format(int argc, char** argv) {
    int mode = 0;
    for (int i = 0; i < argc; i++) {
        const char* arg = argv[i];
        if (!strcmp(arg, "--")) break;
        if (jash_discovery_format_flag(arg)) {
            if (mode) return -1;
            mode = !strcmp(arg, "--json") ? JASH_DISCOVERY_JSON : JASH_DISCOVERY_DATA;
        } else if (!strcmp(arg, "-s") || !strcmp(arg, "--signal") || !strcmp(arg, "-k") || !strcmp(arg, "--kill-after")) i++;
        else if (arg[0] != '-') break;
    }
    return mode;
}
static int timeout_error(jash_writer_t writer, void* context, int mode, const char* code, int status) {
    return mode ? jash_discovery_error(writer, context, "timeout", mode, code, status) : status;
}

static volatile sig_atomic_t g_to_fired = 0;
static void to_alarm_handler(int sig) { (void)sig; g_to_fired = 1; }

static int to_arm_timer(double seconds) {
    struct itimerval it;
    memset(&it, 0, sizeof it);
    it.it_value.tv_sec = (long)seconds;
    it.it_value.tv_usec = (long)((seconds - (long)seconds) * 1000000.0);
    if (it.it_value.tv_sec == 0 && it.it_value.tv_usec == 0) it.it_value.tv_usec = 1;
    return setitimer(ITIMER_REAL, &it, NULL);
}

static int to_disarm_timer(void) {
    struct itimerval it;
    memset(&it, 0, sizeof it);
    return setitimer(ITIMER_REAL, &it, NULL);
}

static long to_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000 + (long)(tv.tv_usec / 1000);
}

/* Block until `pid` exits OR the armed timer fires. Loops past any spurious
 * EINTR not caused by our own SIGALRM handler (defensive — no other handler
 * is installed anywhere in this call, but a future signal source, or a rare
 * one like SIGWINCH during a long timeout, must not be misread as either
 * "child exited with status 0" or "timed out"). Returns 2 = cancelled,
 * 1 = timed out, 0 = child reaped (status filled), -1 = waitpid error. */
static int to_wait_or_timeout(jash_state_t* s, long pid, int* status) {
    for (;;) {
        long w_rc = sys_waitpid(pid, status, WNOHANG);
        if (w_rc == pid) return 0;
        if (w_rc == 0) {
            if (jash_agent_cancel_poll(s)) return 2;
            if (g_to_fired) return 1;
            struct pollfd pfd = { .fd = s ? s->agent_cancel_fd : -1,
                                  .events = POLLIN };
            (void)poll(pfd.fd >= 0 ? &pfd : NULL, pfd.fd >= 0 ? 1 : 0, 10);
            continue;
        }
        if (errno == EINTR) {
            if (g_to_fired) return 1;
            continue;  /* spurious — not our timer, keep waiting */
        }
        return -1;
    }
}

static int to_kill_group_and_confirm(long leader) {
    if (sys_kill(-leader, SIGKILL) != 0 && errno == ESRCH) return 0;
    for (int i = 0; i < 2000; i++) {
        if (sys_kill(-leader, 0) != 0 && errno == ESRCH) return 0;
        (void)poll(NULL, 0, 1);
    }
    return -1;
}


/* -- JINV-058: onde ele estava quando morreu -------------------------------
 *
 * Um comando que trava e indistinguivel de um comando lento, e o `timeout`
 * resolvia metade do problema: mata, e devolve 124. O agente fica sabendo QUE
 * travou e nada sobre ONDE -- que e a unica informacao que muda o que ele faz
 * em seguida.
 *
 * Nesta sessao isso custou caro: duas correcoes travaram o shell, tres
 * hipoteses foram refutadas em sequencia, e o ponto real (`split_statements`,
 * sem consciencia de aspas) so apareceu quando um `sample` foi rodado a mao
 * contra o PID. O `timeout` estava a um passo dessa resposta e nao deu.
 *
 * `--diagnose` fecha isso: no instante do estouro, com o processo AINDA VIVO,
 * tira uma amostra de pilha antes de sinalizar.
 *
 * A disciplina que nao pode cair (JINV-045): a flag NUNCA devolve silencio.
 * Sem amostrador na plataforma, ela DIZ que nao pode -- porque uma flag que
 * retorna zero sem fazer nada e o defeito, nao a degradacao elegante.
 */
#define TO_WHERE_MAX 1400

/* Preenche `out` com as frames de topo do processo `pid`, ou com a razao pela
 * qual nao foi possivel. Devolve 1 se ha amostra real, 0 se ha so a razao. */
static int to_sample_where(pid_t pid, char* out, size_t cap) {
    out[0] = '\0';
    const char* tool = 0;
    char cmd[256];

    if (access("/usr/bin/sample", X_OK) == 0) {          /* macOS */
        tool = "sample";
        snprintf(cmd, sizeof cmd,
                 "/usr/bin/sample %d 1 -mayDie 2>/dev/null | "
                 "grep -E '^ +[0-9]+ ' | head -14", (int)pid);
    } else if (access("/usr/bin/eu-stack", X_OK) == 0) { /* Linux, elfutils */
        tool = "eu-stack";
        snprintf(cmd, sizeof cmd,
                 "/usr/bin/eu-stack -p %d 2>/dev/null | head -14", (int)pid);
    } else if (access("/proc/self/stat", R_OK) == 0) {   /* Linux minimo */
        tool = "/proc";
        snprintf(cmd, sizeof cmd,
                 "cat /proc/%d/stack 2>/dev/null | head -10; "
                 "cat /proc/%d/wchan 2>/dev/null", (int)pid, (int)pid);
    }

    if (!tool) {
        snprintf(out, cap,
                 "amostrador indisponivel nesta plataforma "
                 "(procurei: sample, eu-stack, /proc/<pid>/stack)");
        return 0;
    }

    FILE* f = popen(cmd, "r");
    if (!f) {
        snprintf(out, cap, "%s presente mas nao pude executa-lo", tool);
        return 0;
    }
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = '\0';
    pclose(f);

    /* Vazio nao pode virar silencio: e uma resposta, e precisa dizer isso. */
    size_t k = 0;
    while (out[k] == ' ' || out[k] == '\n' || out[k] == '\t') k++;
    if (out[k] == '\0') {
        snprintf(out, cap,
                 "%s rodou mas nao devolveu frames "
                 "(processo pode ter saido, ou faltam permissoes)", tool);
        return 0;
    }
    return 1;
}

int jash_b_timeout(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    s->agent_request_kill_incomplete = 0;
    int primary_sig = SIGTERM;
    int have_kill_after = 0;
    double kill_after_s = 0.0;
    int preserve_status = 0;
    int verbose = 0;
    int mode = timeout_format(argc, argv);
    if (mode < 0) return jash_discovery_error(w, ctx, "timeout", JASH_DISCOVERY_JSON, "conflicting_formats", 125);
    int json = mode != 0;
    int diagnose = 0;
    char where[TO_WHERE_MAX] = "";
    int where_real = 0;
    /* --foreground: accepted for CLI compatibility, documented no-op in v1
     * (no real tty-foreground-transfer semantics implemented). */

    int i = 0;
    for (; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "-s") == 0 || strcmp(a, "--signal") == 0) {
            if (i + 1 >= argc) { jash_writef(jash_stderr_writer, 0, "timeout: option '%s' requires an argument\n", a); return timeout_error(w, ctx, mode, "invalid_arguments", 125); }
            int sig = to_parse_signal_arg(argv[++i]);
            if (sig < 0) { jash_writef(jash_stderr_writer, 0, "timeout: %s: invalid signal\n", argv[i]); return timeout_error(w, ctx, mode, "invalid_arguments", 125); }
            primary_sig = sig;
            continue;
        }
        if (strncmp(a, "--signal=", 9) == 0) {
            int sig = to_parse_signal_arg(a + 9);
            if (sig < 0) { jash_writef(jash_stderr_writer, 0, "timeout: %s: invalid signal\n", a + 9); return timeout_error(w, ctx, mode, "invalid_arguments", 125); }
            primary_sig = sig;
            continue;
        }
        if (strcmp(a, "-k") == 0 || strcmp(a, "--kill-after") == 0) {
            if (i + 1 >= argc) { jash_writef(jash_stderr_writer, 0, "timeout: option '%s' requires an argument\n", a); return timeout_error(w, ctx, mode, "invalid_arguments", 125); }
            if (to_parse_duration(argv[++i], &kill_after_s) != 0) {
                jash_writef(jash_stderr_writer, 0, "timeout: %s: invalid duration\n", argv[i]); return timeout_error(w, ctx, mode, "invalid_arguments", 125);
            }
            have_kill_after = 1;
            continue;
        }
        if (strncmp(a, "--kill-after=", 13) == 0) {
            if (to_parse_duration(a + 13, &kill_after_s) != 0) {
                jash_writef(jash_stderr_writer, 0, "timeout: %s: invalid duration\n", a + 13); return timeout_error(w, ctx, mode, "invalid_arguments", 125);
            }
            have_kill_after = 1;
            continue;
        }
        if (strcmp(a, "--preserve-status") == 0) { preserve_status = 1; continue; }
        if (strcmp(a, "--foreground") == 0) { continue; }
        if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) { verbose = 1; continue; }
        if (jash_discovery_format_flag(a)) continue;
        if (strcmp(a, "--diagnose") == 0) { diagnose = 1; continue; }
        break;  /* first non-flag token: DURATION */
    }

    if (i >= argc) {
        jash_writes(jash_stderr_writer, 0, "timeout: missing duration operand\n");
        return timeout_error(w, ctx, mode, "invalid_arguments", 125);
    }
    double duration_s = 0.0;
    if (to_parse_duration(argv[i], &duration_s) != 0) {
        jash_writef(jash_stderr_writer, 0, "timeout: %s: invalid duration\n", argv[i]);
        return timeout_error(w, ctx, mode, "invalid_arguments", 125);
    }
    i++;
    if (i >= argc) {
        jash_writes(jash_stderr_writer, 0, "timeout: missing command\n");
        return timeout_error(w, ctx, mode, "invalid_arguments", 125);
    }

    /* Everything from here is the wrapped command's OWN argv, verbatim —
     * never permuted, never re-parsed as timeout flags. */
    char** wrapped_argv = &argv[i];

    char fullpath[512];
    int found = jash_path_lookup(s, wrapped_argv[0], fullpath, sizeof fullpath);
    if (!found) {
        jash_writef(jash_stderr_writer, 0, "timeout: failed to run command '%s': No such file or directory\n", wrapped_argv[0]);
        return timeout_error(w, ctx, mode, "command_not_found", 127);
    }

    to_envp_t envp;
    if (to_build_envp(&envp, s)) return timeout_error(w, ctx, mode, "environment_limit", 125);
    jash_discovery_rows_t rows;
    jash_structured_output_t output;
    if (mode && jash_discovery_rows_init(&rows)) { free(envp.buffer); free(envp.envp); return timeout_error(w, ctx, mode, "allocation_failed", 125); }
    if (mode == JASH_DISCOVERY_DATA) {
        int admission = jash_discovery_effects_begin(&rows, w, ctx, "timeout", mode, "jash.timeout.v1");
        if (admission) { jash_discovery_rows_free(&rows); free(envp.buffer); free(envp.envp); return admission; }
    } else if (mode == JASH_DISCOVERY_JSON) {
        if (jash_structured_begin(&output, w, ctx)) { jash_discovery_rows_free(&rows); free(envp.buffer); free(envp.envp); return 125; }
        /* A valid JSON whitespace prefix proves sink admission before fork. */
        jash_jsonw_raw(&output.json, " ");
        if (jash_jsonw_finish(&output.json)) { jash_discovery_rows_free(&rows); free(envp.buffer); free(envp.envp); return jash_structured_finish(&output, 125); }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = to_alarm_handler;
    struct sigaction previous_alarm;
    struct itimerval previous_timer;
    int previous_timer_valid = 0, previous_alarm_valid = 0;
    g_to_fired = 0;

    long t0 = to_now_ms();
    int status = 0, timed_out = 0, cancelled = 0, signal_sent = 0, execution_error = 0;
    long pid = -1;
    if (getitimer(ITIMER_REAL, &previous_timer)) { execution_error = errno; goto completed; }
    previous_timer_valid = 1;
    if (sigaction(SIGALRM, &sa, &previous_alarm)) { execution_error = errno; goto completed; }
    previous_alarm_valid = 1;
    /* A failed deadline must never admit an unbounded external action.
     * fork does not inherit the parent's armed interval timer. */
    if (duration_s > 0 ? to_arm_timer(duration_s) : to_disarm_timer()) {
        execution_error = errno; goto completed;
    }
    pid = sys_fork();
    if (pid < 0) {
        jash_writef(jash_stderr_writer, 0, "timeout: fork failed\n");
        execution_error = errno;
        goto completed;
    }
    if (pid == 0) {
        /* Child: own process group so the timeout/kill-after path can signal
         * the whole tree, not just this direct child. */
        setpgid(0, 0);
        if (mode && dup2(STDERR_FILENO, STDOUT_FILENO) < 0) _exit(126);
#ifdef JASH_AGENT_ONLY
        long fd_limit = sysconf(_SC_OPEN_MAX);
        if (fd_limit < 0 || fd_limit > 65536) fd_limit = 65536;
        for (int fd = 3; fd < fd_limit; fd++) close(fd);
#endif
        (void)signal(SIGINT, SIG_DFL);
        (void)signal(SIGQUIT, SIG_DFL);
        sys_execve(fullpath, wrapped_argv, envp.envp);
        const char* msg = "timeout: exec failed\n";
        if (write(2, msg, strlen(msg)) < 0) { /* exiting anyway */ }
        _exit(126);
    }

    /* Parent. */
    (void)setpgid(pid, pid);  /* close the child-only setpgid race */
    int wr = to_wait_or_timeout(s, pid, &status);
    int wait_error = wr < 0 ? errno : 0;
    if (to_disarm_timer()) execution_error = errno;

    if (wr < 0 || execution_error) {
        jash_writef(jash_stderr_writer, 0, "timeout: waitpid failed\n");
        if (wait_error) execution_error = wait_error;
        (void)to_kill_group_and_confirm(pid);
        (void)sys_waitpid(pid, &status, 0);
        goto completed;
    }

    if (wr == 1 || wr == 2) {
        timed_out = wr == 1;
        cancelled = wr == 2;
        if (cancelled) s->agent_request_cancelled = 1;
        if (timed_out) s->agent_request_timed_out = 1;
        signal_sent = primary_sig;
        if (verbose) {
            jash_writef(jash_stderr_writer, 0, "timeout: sending signal %s to command '%s'\n",
                        to_sig_name(primary_sig), wrapped_argv[0]);
        }
        /* JINV-058: amostra ANTES de sinalizar -- depois do kill nao ha o que ver. */
        if (diagnose) where_real = to_sample_where(pid, where, sizeof where);

        sys_kill(-pid, primary_sig);

        if (have_kill_after) {
            g_to_fired = 0;
            int arm_error = to_arm_timer(kill_after_s);
            if (arm_error) execution_error = errno;
            int wr2 = arm_error ? 1 : to_wait_or_timeout(NULL, pid, &status);
            int second_wait_error = wr2 < 0 ? errno : 0;
            if (to_disarm_timer() && !execution_error) execution_error = errno;
            if (second_wait_error) { execution_error = second_wait_error; wr2 = 1; }
            if (wr2 == 1) {
                signal_sent = SIGKILL;
                if (verbose) {
                    jash_writef(jash_stderr_writer, 0, "timeout: sending signal KILL to command '%s'\n",
                                wrapped_argv[0]);
                }
                sys_kill(-pid, SIGKILL);
                sys_waitpid(pid, &status, 0);  /* final blocking reap — SIGKILL cannot hang */
                if (to_kill_group_and_confirm(pid) != 0)
                    s->agent_request_kill_incomplete = 1;
            } else if (wr2 == 0) {
                /* Only ESRCH proves disappearance. EPERM and every other probe
                 * result remain live/unknown and must fail closed. */
                int probe = sys_kill(-pid, 0);
                if (!(probe != 0 && errno == ESRCH)) {
                    signal_sent = SIGKILL;
                    if (to_kill_group_and_confirm(pid) != 0)
                        s->agent_request_kill_incomplete = 1;
                }
            }
            /* else: wr2==0 already reaped the real post-signal status. */
        } else {
            /* No grace period configured — block until the primary signal
             * actually reaps the child so `status` reflects its real
             * post-signal state (needed for --preserve-status and to avoid
             * leaving a zombie). If the command ignores/traps the signal and
             * never exits, this blocks forever — same documented limitation
             * as real GNU timeout without --kill-after. */
            sys_waitpid(pid, &status, 0);
        }
    }

    /* The agent runtime advertises background=false. A command leader may
     * exit after spawning descendants, so reaping that leader alone is not
     * completion. Quiesce the whole process group before output and receipt
     * snapshots are committed. */
    if (wr == 0) {
        int probe = sys_kill(-pid, 0);
        if (probe == 0 || errno != ESRCH) {
            signal_sent = SIGKILL;
            if (to_kill_group_and_confirm(pid) != 0)
                s->agent_request_kill_incomplete = 1;
        }
    }

completed:
    if (previous_alarm_valid) {
        if (to_disarm_timer() && !execution_error) execution_error = errno;
        if (sigaction(SIGALRM, &previous_alarm, NULL) && !execution_error) execution_error = errno;
    }
    if (previous_timer_valid && setitimer(ITIMER_REAL, &previous_timer, NULL) && !execution_error) execution_error = errno;
    free(envp.buffer); free(envp.envp);
    long duration_ms = to_now_ms() - t0;
    int decoded = to_decode_status(status);
    /* JINV-058: o texto tambem entrega -- e um agente lendo stderr e o caso
     * comum. Vai para stderr junto com os demais avisos do timeout. */
    if (diagnose && timed_out && !json) {
        if (where_real) {
            jash_writef(jash_stderr_writer, 0,
                        "timeout: onde estava quando estourou:\n%s", where);
            if (where[0] && where[strlen(where) - 1] != '\n')
                jash_writes(jash_stderr_writer, 0, "\n");
        } else {
            jash_writef(jash_stderr_writer, 0,
                        "timeout: diagnostico indisponivel: %s\n", where);
        }
    }

    int exit_code = execution_error || s->agent_request_kill_incomplete ? 125 :
                    cancelled ? 130 :
                    ((timed_out && !preserve_status) ? 124 : decoded);

    if (json) {
        if (diagnose && !where[0]) snprintf(where, sizeof where, "deadline did not expire; no live process was sampled");
        jdv_value_t* arguments = calloc((size_t)(argc - i), sizeof(*arguments));
        jdv_value_t argument_list = jdv_value_nothing();
        if (!arguments) rows.error = 1;
        for (int j = i; j < argc && !rows.error; j++) arguments[j - i] = jash_discovery_string(&rows, argv[j]);
        if (!rows.error && jdv_value_list(&rows.arena, arguments, (size_t)(argc - i), &argument_list)) rows.error = 1;
        free(arguments);
        const char* signal_name = (timed_out || cancelled || signal_sent) ? to_sig_name(signal_sent) : NULL;
#define TF(key, value) {JDV_LITERAL(key), (value)}
        jdv_record_input_t fields[] = {
            TF("schema", jash_discovery_string(&rows, "jash.timeout.v1")),
            TF("exit_code", jdv_value_i64(exit_code)), TF("child_exit_code", pid > 0 ? jdv_value_i64(decoded) : jdv_value_nothing()),
            TF("started", jdv_value_bool(pid > 0)),
            TF("timed_out", jdv_value_bool(timed_out)), TF("cancelled", jdv_value_bool(cancelled)),
            TF("signal_sent", jash_discovery_string(&rows, signal_name)),
            TF("duration_ms", jdv_value_i64(duration_ms)),
            TF("duration", duration_ms >= 0 && (uint64_t)duration_ms <= INT64_MAX / 1000000 ? jdv_value_duration(duration_ms * 1000000) : jdv_value_nothing()),
            TF("pid", pid > 0 ? jdv_value_u64((uint64_t)pid) : jdv_value_nothing()),
            TF("argv", argument_list), TF("child_output_channel", jash_discovery_string(&rows, "stderr")),
            TF("kill_incomplete", jdv_value_bool(s->agent_request_kill_incomplete)),
            TF("error", jash_discovery_string(&rows, execution_error ? strerror(execution_error) : NULL)),
            TF("where_available", jdv_value_bool(where_real)),
            TF("where", jash_discovery_string(&rows, diagnose ? where : NULL)),
            TF("where_preview", jdv_value_bool(diagnose))
        };
#undef TF
        /* Diagnostic fields are opt-in in the established JSON contract.
         * Keep the same conditional record shape for the native projection. */
        size_t field_count = sizeof fields / sizeof *fields;
        if (!diagnose) field_count -= 3;
        jash_discovery_add(&rows, fields, field_count);
        if (mode == JASH_DISCOVERY_DATA) exit_code = jash_discovery_emit_schema(w, ctx, "timeout", mode, &rows, exit_code, "jash.timeout.v1");
        else {
            if (rows.error || !rows.count || jdv_json_write(&output.json, &rows.items[0], &JDV_DEFAULT_LIMITS)) {
                if (!output.json.error) { jash_jsonw_raw(&output.json, "{\"complete\":false,\"error\":{\"code\":\"collection_limit_exceeded\",\"effects_may_have_applied\":true}}"); exit_code = 75; }
            }
            jash_jsonw_raw(&output.json, "\n");
            exit_code = jash_structured_finish(&output, exit_code);
        }
        jash_discovery_rows_free(&rows);
    }

    return exit_code;
}
