// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: builtins/cage — ADR-0089 GAIOLA W4b: arm a fail-closed resource
// envelope on a freshly-forked child, then exec it.
//
// Usage:
//   cage [cpu=US] [wall=US] [rss=PAGES] [pids=N] [fds=N] -- CMD [ARGS...]
//
// Beyond-only. bear_fork()'s POSIX pid_t return has no room for the child's
// cap_domid_t, which SYS_gaiola_arm needs as its target — so cage cannot
// reuse bear_fork()/sys_fork(). It drives its own raw Conduit fork (mirrors
// blibc's _bear_fork_conduit, core/process/process.c) to read the domid
// SYS_surface_fork packs into the high 32 bits of a successful return
// (INV-106), arms the envelope on that exact domid in the parent, then lets
// the child exec. See INV-105 (arming authority) and INV-106 (the domid
// handoff) in docs/INVARIANTS.md.
//
// ADR-0091 C4: the arm request itself no longer calls SYS_gaiola_arm
// directly (bear_raw_syscall6) — it goes through gaiola-surface over a
// Conduit (surfaces/gaiola/gaiola_surface.c), the same PROC_OP_*-style
// pattern proc-surface already established. jash no longer needs
// CAP_GAIOLA_ARM itself (removed from bshell.basm/shell_caps.basm) —
// only CAP_IPC_SEND/CAP_IPC_RECV, which it already holds for the fork
// request. gaiola-surface holds CAP_GAIOLA_ARM and resolves the REAL
// caller's authority via the kernel-verified conduit sender (INV-108/109),
// not a self-reported field, so nothing is trusted that a hostile client
// could forge.

#include "builtin.h"

#ifndef JASH_TARGET_BEYOND

int jash_b_cage(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    (void)s; (void)argc; (void)argv;
    jash_writes(w, ctx, "jash: cage: GAIOLA resource envelopes are Beyond-only\n");
    return 1;
}

#else /* JASH_TARGET_BEYOND */

#include <core/surface/proc.h>
#include <sys/syscalls.h>
#include <theme/colors.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>

/* ADR-0091 C4: gaiola-surface's wire protocol (surfaces/gaiola/protocol.h).
 * Kept as an independent, hand-synced copy rather than a shared header —
 * the same precedent core/surface/proc.h already sets for proc-surface's
 * protocol. proc_conduit_send/proc_conduit_recv (core/surface/proc.h) are
 * generic Conduit primitives despite the header's name — reused as-is. */
#define GAIOLA_REQ    0x47410001u
#define GAIOLA_RESP   0x47410002u
#define GAIOLA_OP_ARM 1
#define GAIOLA_REQ_CONDUIT   "gaiola-requests"
#define GAIOLA_RESP_CONDUIT  "gaiola-responses"

typedef struct {
    uint32_t type;
    uint32_t op;
    int32_t  status;
    uint32_t domid;
    uint64_t cpu_us;
    uint64_t wall_us;
    uint64_t rss_pages;
    uint64_t pids;
    uint64_t fds;
} gaiola_cell_t;

extern char **environ;

/* Beyond fork child resumes here (bshell.basm: xor eax,eax; ret) with
 * RAX=0 and RSP = the parent's captured entry_rsp, so `ret` lands back in
 * whoever called the fork wrapper. jash needs its OWN copy of what blibc
 * calls _bear_fork_child_ret — that symbol isn't just unreusable (it's
 * paired with bear_fork()'s pid_t-only return, which has no room for the
 * domid), it doesn't even exist in the currently-linked libblibc.a (see
 * bshell.basm's comment on this stub for why). */
extern char cage_fork_child_ret[];

/* Called from the real-assembly trampoline `cage_raw_fork` (bshell.basm)
 * — bear silently drops a bare top-level asm-only function (confirmed this
 * wave: compiling process.c with bear+__BEYOND__ emits neither bear_fork nor
 * _bear_fork_child_ret), so unlike blibc's own _bear_fork_conduit this one
 * cannot be paired with a same-file "__asm__(...)" stub; the entry-RSP
 * capture lives in basm instead (see the comment there for the full why).
 * rdi = out_domid, rsi = entry_rsp — both set by that trampoline. Must stay
 * non-static: the basm stub jmps to this symbol by name. */
long
cage_fork_conduit(uint32_t* out_domid, uint64_t entry_rsp)
{
    proc_cell_t req = {0}, resp = {0};
    uint64_t rfl;

    asm volatile("pushfq; popq %0" : "=r"(rfl));

    req.type = PROC_REQ;
    req.op   = PROC_OP_FORK;
    req.pid  = (int32_t)getpid();

    ((uint64_t *)req.data)[0] = (uint64_t)cage_fork_child_ret;
    ((uint64_t *)req.data)[1] = entry_rsp;
    ((uint64_t *)req.data)[2] = rfl;

    if (proc_request(&req, &resp) < 0) return -1;
    /* GAIOLA W4b (INV-106): the domid SYS_surface_fork packed into the high
     * 32 bits of its return, relayed by proc-surface into resp.data[0]. */
    if (out_domid) *out_domid = ((uint32_t *)resp.data)[0];
    return (long)(int32_t)resp.pid;
}

/* Defined in bshell.basm. */
extern long cage_raw_fork(uint32_t* out_domid);

/* Decode wait4 status into a shell-style exit code (exec.c/pipeline.c carry
 * their own copy of this same tiny decode — Beyond's wait status ABI isn't
 * plumbed yet, FASE2 follow-up in blibc's bear_waitpid, so this currently
 * only matters once that lands). */
static int cage_decode_wait_status(int status) {
    if ((status & 0x7F) == 0) return (status >> 8) & 0xFF;      /* exited */
    if ((status & 0x7F) != 0x7F) return 128 + (status & 0x7F);  /* signaled */
    return 128;                                                 /* stopped */
}

int jash_b_cage(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    (void)s;

    uint64_t cpu_us = 0, wall_us = 0, rss_pages = 0, pids = 0, fds = 0;
    int i = 0;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }

        char* eq = strchr(argv[i], '=');
        if (!eq) break; /* first bare token: CMD given without a "--" */

        *eq = '\0';
        const char* key = argv[i];
        uint64_t val = (uint64_t)strtoull(eq + 1, NULL, 10);
        int known = 1;
        if      (strcmp(key, "cpu")  == 0) cpu_us    = val;
        else if (strcmp(key, "wall") == 0) wall_us   = val;
        else if (strcmp(key, "rss")  == 0) rss_pages = val;
        else if (strcmp(key, "pids") == 0) pids      = val;
        else if (strcmp(key, "fds")  == 0) fds       = val;
        else known = 0;
        *eq = '=';

        if (!known) {
            jash_writef(w, ctx, "%sjash: cage: unknown envelope key '%s'%s\n",
                        J_ROLE_ERR, argv[i], J_RESET);
            return 2;
        }
    }

    if (i >= argc) {
        jash_writef(w, ctx,
            "%sjash: cage: usage: cage [cpu=US] [wall=US] [rss=PAGES] [pids=N] [fds=N] -- CMD [ARGS...]%s\n",
            J_ROLE_ERR, J_RESET);
        return 2;
    }

    char** cmd_argv = &argv[i];

    uint32_t domid = 0;
    long pid = cage_raw_fork(&domid);
    if (pid < 0) {
        jash_writef(w, ctx, "%sjash: cage: fork failed (%ld)%s\n",
                    J_ROLE_ERR, pid, J_RESET);
        return 1;
    }

    if (pid == 0) {
        /* Child: sys_execve already routes through proc-surface's
         * PROC_OP_EXEC on Beyond (exec.c relies on this same call
         * unmodified under JASH_TARGET_BEYOND) — nothing cage-specific
         * needed here beyond picking the caged argv. */
        sys_execve(cmd_argv[0], cmd_argv, environ);
        const char* msg = "cage: execve failed\n";
        sys_write(2, msg, strlen(msg));
        sys_exit(127);
        for (;;) { }
    }

    /* Parent: arm the envelope on the exact domid we just learned, before
     * waiting on the child. There is an unavoidable window between the
     * child becoming runnable (the instant SYS_surface_fork returns) and
     * this arm landing — closing it needs a start-stopped child, deferred
     * to a future wave. Descendant-only authority (INV-105) means this can
     * only ever narrow what the child may do, never widen it, so a slow
     * arm is a scope gap, not a soundness gap.
     *
     * ADR-0091 C4: routed through gaiola-surface, not a raw syscall — jash
     * holds no CAP_GAIOLA_ARM itself. gaiola-surface resolves cage's own
     * identity from the kernel-verified Conduit sender (INV-108/109), so
     * the descendant-only check runs against jash's real domid either way. */
    gaiola_cell_t garm_req = {0}, garm_resp = {0};
    garm_req.type      = GAIOLA_REQ;
    garm_req.op        = GAIOLA_OP_ARM;
    garm_req.domid     = domid;
    garm_req.cpu_us    = cpu_us;
    garm_req.wall_us   = wall_us;
    garm_req.rss_pages = rss_pages;
    garm_req.pids      = pids;
    garm_req.fds       = fds;

    long arm_rc = -1;
    if (proc_conduit_send(GAIOLA_REQ_CONDUIT, 0, &garm_req, sizeof(garm_req)) >= 0 &&
        proc_conduit_recv(GAIOLA_RESP_CONDUIT, &garm_resp, sizeof(garm_resp)) >= 0 &&
        garm_resp.type == GAIOLA_RESP && garm_resp.op == GAIOLA_OP_ARM) {
        arm_rc = garm_resp.status;
    }
    if (arm_rc < 0) {
        jash_writef(w, ctx,
            "%sjash: cage: arm failed (%ld) — child left unconfined, killing it%s\n",
            J_ROLE_ERR, arm_rc, J_RESET);
        sys_kill(pid, SIGKILL);
        sys_waitpid(pid, 0, 0);
        return 1;
    }

    int status = 0;
    long w_rc = sys_waitpid(pid, &status, 0);
    if (w_rc < 0) {
        jash_writef(w, ctx, "%sjash: cage: waitpid failed (%ld)%s\n",
                    J_ROLE_ERR, w_rc, J_RESET);
        return 1;
    }
    return cage_decode_wait_status(status);
}

#endif /* JASH_TARGET_BEYOND */
