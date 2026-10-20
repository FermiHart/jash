// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 The author makes music. Hear F E R M I ∞ H A R T wherever you stream.
//    No obligation. Just a wish.
//
// Module: core/state — jash_state_t lifecycle (init, serialize, deserialize)
//
// (FHCL-1.0 — F E R M I ∞ H A R T Creativity License — pending draft;
//  Unlicense placeholder until then.)

#include "state.h"
#include "env.h"
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <sys/syscalls.h>
#include <job_control.h>
#include <parser/coproc.h>
#include <function_defs.h>

void jash_state_init(jash_state_t* s) {
    /* Zero only the scalar fields — avoid memset on the full 355KB
     * struct which contains large arrays (env 73KB, history 262KB,
     * aliases 18KB). Arrays are zeroed lazily when first accessed. */
    s->exit_requested = 0;
    /* INV-233: ss_engine is created LAZILY by main (jash_ss_engine_create)
     * and guarded through the process-lifetime global there — the STATE
     * field itself must start NULL so a RE-initialized state can never
     * carry a stale engine pointer from a previous session. */
    s->ss_engine = (void *)0;
    s->agent_cancel_fd = -1;
    s->agent_active_request_id[0] = '\0';
    s->agent_cancel_buf[0] = '\0';
    s->agent_cancel_len = 0;
    s->agent_cancel_discarding = 0;
    s->agent_request_cancelled = 0;
    s->agent_request_timed_out = 0;
    s->agent_request_kill_incomplete = 0;
    s->loop_break_level = 0;
    s->env_count = 0;
    s->env_reserved_slots = 0;
    s->env_generation = 0;
    /* Native call frames start empty (table owns these keys until the
     * first frame pushes; see env.c frame hooks). */
    s->func_depth = 0;
    s->func_depth_str[0] = '0';
    s->func_depth_str[1] = '\0';
    s->frame_dirty = 0;
    memset(s->funcname_stack, 0, sizeof(s->funcname_stack));
    memset(s->frame_stack, 0, sizeof(s->frame_stack));

    /* Initialize env hash table: all buckets = -1 (empty).
     * 0xFF bytes give 0xFFFFFFFF = -1 for every int32_t slot. */
    memset(s->env_hash, 0xFF, sizeof(s->env_hash));
    jash_env_array_index_init(s);
    s->alias_count = 0;
    s->abbr_global_count = 0;
    s->abbr_local_count = 0;
    s->yank_len = 0;
    s->last_status.code = 0;
    s->last_status.msg[0] = '\0';
    s->last_status.suggestion[0] = '\0';
    s->last_status.cmd[0] = '\0';
    s->last_status.time_ns = 0;
    s->locale = 0;
    s->pid = 0;
    s->tty = 0;
    s->interactive = 1;
    s->resume_path = 0;
    s->jobs = jash_job_table_create();
    s->job_control_active = 1;
    s->shell_pgid = 0;
    s->shell_tty_fd = -1;
    s->functions = (jash_function_table*)malloc(sizeof(jash_function_table));
    if (s->functions) {
        jash_function_table_init(s->functions);
    }
    /* Allocate and initialize coproc table */
    s->coprocs = (coproc_table_t*)malloc(sizeof(coproc_table_t));
    if (s->coprocs) {
        jash_coproc_init(s->coprocs);
    }

    /* Get actual CWD from the system */
    {
        char real_cwd[512];
        long len = sys_getcwd(real_cwd, sizeof(real_cwd));
        if (len >= 0 && real_cwd[0] == '/') {
            strlcpy(s->cwd, real_cwd, sizeof(s->cwd));
            s->cwd[sizeof(s->cwd) - 1] = '\0';
        } else {
            s->cwd[0] = '/';
            s->cwd[1] = '\0';
        }
    }
    s->shell_opts.errexit = 0; /* bash parity: errexit OFF by default (enable via set -e) */
    s->shell_opts.nounset = 0;
    s->shell_opts.xtrace = 0;
    s->shell_opts.verbose = 0;
    s->shell_opts.pipefail = 0;
    s->shell_opts.posix = 0;
    s->shell_opts.emacs = 0;
    s->shell_opts.errtrace = 0;
    s->shell_opts.lastpipe = 0;
    s->shell_opts.nullglob = 0;
    s->shell_opts.failglob = 0;
    s->shell_opts.nocaseglob = 0;

    /* Initialize frecency table (persist_path filled later when HOME is available) */
    s->frecency.count = 0;
    s->frecency.persist_path[0] = '\0';

    /* Initialize prediction table (persist_path filled later when HOME is available) */
    s->predictions.count = 0;
    s->predictions.persist_path[0] = '\0';
    
    /* Initialize PATH cache (restored lazily on first external
     * lookup; a builtin-only run never opens path_cache.dat). */
    s->path_cache_loaded = 0;
    s->path_cache_count = 0;
    s->path_cache_disk_count = 0;
    memset(s->path_cache_dirty, 0, sizeof(s->path_cache_dirty));
    s->path_cache_meta_dirty = 0;

    /* Identidade do stdout da SESSÃO. O governador de saída usa isso para
     * distinguir a janela do agente de um pipe / `> arquivo`. Tem de ser aqui,
     * no arranque: depois que um redirect roda, a resposta já mudou. */
    extern void jash_exec_session_stdout_record(void);
    jash_exec_session_stdout_record();

    /* Initialize variable expansion cache */
    s->var_cache = NULL;
    jash_var_cache_init(s);
}

/* Release every allocation owned by the state. Call before freeing a
 * heap-allocated jash_state_t. */
void jash_state_cleanup(jash_state_t* s) {
    if (!s) return;
    extern void jash_path_cache_flush(jash_state_t* s);
    jash_path_cache_flush(s);
    if (s->coprocs) {
        jash_coproc_shutdown(s->coprocs);
        free(s->coprocs);
        s->coprocs = NULL;
    }
    if (s->functions) {
        for (int i = 0; i < s->functions->count; i++) {
            free(s->functions->funcs[i].body_heap);
            s->functions->funcs[i].body_heap = NULL;
        }
        free(s->functions);
        s->functions = NULL;
    }
    free(s->jobs);
    s->jobs = NULL;
    for (uint32_t i = 0; i < s->env_count; i++) {
        if (s->env[i].heap_val) {
            free(s->env[i].heap_val);
            s->env[i].heap_val = NULL;
        }
    }
    if (s->var_cache) {
        free(s->var_cache);
        s->var_cache = NULL;
    }
}

/* Serialize state to /tmp/jash_state.bin for live-reload (--restore).
 * Preserves: CWD, aliases, env, history.
 * Returns 0 on success, negative on error. */
int jash_state_serialize(const jash_state_t* s, const char* path) {
    if (!s || !path) return -1;

    long fd = sys_open(path, 0x241, 0644);  /* O_WRONLY | O_CREAT | O_TRUNC */
    if (fd < 0) return -2;

    /* Magic + version header */
    unsigned int magic = 0x4A415348;  /* "JOSH" */
    unsigned int version = 1;
    unsigned int cwd_len = s->cwd[0] ? strlen(s->cwd) : 0;

    /* Write header: magic, version, cwd_len */
    char hdr[12];
    memcpy(hdr + 0, &magic, 4);
    memcpy(hdr + 4, &version, 4);
    memcpy(hdr + 8, &cwd_len, 4);

    long n = sys_write(fd, hdr, 12);
    if (n != 12) {
        sys_close(fd);
        return -3;
    }

    /* Write CWD */
    if (cwd_len > 0) {
        n = sys_write(fd, s->cwd, cwd_len);
        if (n != (long)cwd_len) {
            sys_close(fd);
            return -4;
        }
    }

    /* TODO: serialize aliases, env vars, history (future). */

    sys_close(fd);
    return 0;
}

int jash_state_deserialize(jash_state_t* s, const char* path) {
    if (!s || !path) return -1;

    long fd = sys_open(path, 0, 0);  /* O_RDONLY */
    if (fd < 0) return -2;  /* File not found is OK, just return error. */

    /* Read header */
    char hdr[12];
    long n = sys_read(fd, hdr, 12);
    if (n != 12) {
        sys_close(fd);
        return -3;
    }

    uint32_t magic = *(uint32_t*)(hdr + 0);
    uint32_t version = *(uint32_t*)(hdr + 4);
    uint32_t cwd_len = *(uint32_t*)(hdr + 8);

    if (magic != 0x4A415348 || version != 1) {
        sys_close(fd);
        return -4;
    }

    /* Read CWD */
    if (cwd_len > 0 && cwd_len < JASH_CWD_MAX) {
        n = sys_read(fd, s->cwd, cwd_len);
        if (n == (long)cwd_len) {
            s->cwd[cwd_len] = '\0';
        }
    }

    /* TODO: deserialize aliases, env vars, history (future). */

    sys_close(fd);
    return 0;
}
