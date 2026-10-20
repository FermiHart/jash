// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell
// Builtin: shopt — shell options (bash compat)

#include "builtin.h"
#include "shell_output.h"
#include <string.h>

/* Shell options table.
 *
 * The list mirrors the shopt surface of the bash the host ships, and it had
 * fallen 15 entries behind (everything added in bash 4.4 through 5.2). An
 * option missing here is not silent: `shopt -s NAME` answers
 * "invalid option name" on stderr, so any tool that sets a modern option gets
 * a warning on EVERY command. Found via Claude Code, whose Bash tool opens
 * each command with `shopt -s varredir_close`.
 *
 * Recognising an option is not the same as implementing its semantics — the
 * compat3x/4x/5x rows have always been inert too. The table's job is the
 * INTERFACE: accept what bash accepts, report the state back, and never make a
 * caller think the shell is broken because it knows a newer option than we do.
 * Defaults below are the ones bash 5.2 actually reports, not guesses.
 *
 * F E R M I ∞ H A R T */
static struct {
    const char* name;
    int         enabled;
} shopt_opts[] = {
    { "assoc_expand_once", 0 },   /* bash 5.0 */
    { "autocd",       0 },
    { "cdable_vars",  0 },
    { "cdspell",      1 },
    { "checkhash",    0 },
    { "checkjobs",    0 },
    { "checkwinsize", 1 },
    { "cmdhist",      1 },
    { "compat31",     0 },
    { "compat32",     0 },
    { "compat40",     0 },
    { "compat41",     0 },
    { "compat42",     0 },
    { "compat43",     0 },
    { "compat44",     0 },
    { "compat50",     0 },
    { "compat51",     0 },
    { "compat52",     0 },   /* bash 5.2 */
    { "complete_fullquote", 1 },
    { "directquote",  0 },
    { "direxpand",    0 },
    { "dirspell",     0 },
    { "dotglob",      0 },
    { "execfail",     0 },
    { "expand_aliases", 1 },
    { "extdebug",     0 },
    { "extglob",      1 },
    { "extquote",     1 },
    { "failglob",     0 },
    { "force_fignore", 0 },
    { "globasciiranges", 1 },
    { "globskipdots", 1 },   /* bash 5.2 */
    { "globstar",     0 },
    { "gnu_errfmt",   0 },
    { "histappend",   1 },
    { "histreedit",   0 },
    { "histverify",   0 },
    { "hostcomplete", 1 },
    { "huponexit",    0 },
    { "inherit_errexit", 0 },
    { "interactive_comments", 1 },
    { "lastpipe",     0 },
    { "lithist",      0 },
    { "localvar_inherit", 0 },
    { "localvar_unset",  0 },   /* bash 4.4 */
    { "login_shell",  0 },
    { "mailwarn",     0 },
    { "no_empty_cmd_completion", 1 },
    { "nocaseglob",   0 },
    { "nocasematch",  0 },
    { "noexpand_translation", 0 },   /* bash 5.0 */
    { "nullglob",     0 },
    { "patsub_replacement", 1 },   /* bash 5.2 */
    { "progcomp",     1 },
    { "progcomp_alias", 0 },
    { "promptvars",   1 },
    { "restricted_shell", 0 },
    { "shift_verbose", 0 },
    { "sourcepath",   1 },
    { "varredir_close", 0 },   /* bash 4.4 */
    { "xpg_echo",     0 },
    { 0, 0 }
};

static int find_shopt(const char* name) {
    for (int i = 0; shopt_opts[i].name; i++) {
        if (strcmp(shopt_opts[i].name, name) == 0) return i;
    }
    return -1;
}

size_t jash_shopt_count(void) { return sizeof(shopt_opts) / sizeof(*shopt_opts) - 1; }
const char* jash_shopt_at(size_t index, int* enabled) {
    if (index >= jash_shopt_count()) return NULL;
    if (enabled) *enabled = shopt_opts[index].enabled;
    return shopt_opts[index].name;
}
int jash_shopt_set(jash_state_t* s, const char* name, int enabled) {
    int index = find_shopt(name);
    if (index < 0) return -1;
    shopt_opts[index].enabled = !!enabled;
    if (s) {
        if (!strcmp(name, "lastpipe")) s->shell_opts.lastpipe = !!enabled;
        else if (!strcmp(name, "nullglob")) s->shell_opts.nullglob = !!enabled;
        else if (!strcmp(name, "failglob")) s->shell_opts.failglob = !!enabled;
        else if (!strcmp(name, "nocaseglob")) s->shell_opts.nocaseglob = !!enabled;
    }
    return 0;
}

int jash_b_shopt(jash_state_t* s, jash_writer_t w, void* ctx,
                 const char* args) {
    int structured = jash_shell_state_try(s, w, ctx, "shopt", args);
    if (structured != JASH_SHELL_TEXT) return structured;
    (void)s;
    if (!args || !*args) {
        /* List all options */
        for (int i = 0; shopt_opts[i].name; i++) {
            jash_writef(w, ctx, "%s %s\n",
                shopt_opts[i].enabled ? "on" : "off",
                shopt_opts[i].name);
        }
        return 0;
    }

    const char* p = args;
    while (*p == ' ') p++;

    /* -s / -u / -o / -q flags */
    int quiet = 0;
    int print_mode = 0;
    while (*p == '-') {
        p++;
        if (*p == 'q') { quiet = 1; p++; }
        else if (*p == 's') { print_mode = 1; p++; }
        else if (*p == 'u') { print_mode = 2; p++; }
        else if (*p == 'o') { p++; }
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }

    if (!*p && print_mode == 0) {
        /* No args: list all */
        for (int i = 0; shopt_opts[i].name; i++) {
            if (!quiet)
                jash_writef(w, ctx, "%s %s\n",
                    shopt_opts[i].enabled ? "on" : "off",
                    shopt_opts[i].name);
        }
        return 0;
    }

    /* Set or unset options */
    int set = 1;
    if (print_mode == 2) set = 0; /* -u: unset */

    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != ' ') p++;
        uint32_t len = (uint32_t)(p - start);
        char name[64];
        if (len >= sizeof(name)) { while (*p == ' ') p++; continue; }
        for (uint32_t i = 0; i < len; i++) name[i] = start[i];
        name[len] = '\0';

        int idx = find_shopt(name);
        if (idx < 0) {
            if (!quiet)
                jash_writef(w, ctx, "jash: shopt: %s: invalid option name\n", name);
            return 1;
        }
        shopt_opts[idx].enabled = set;
        /* Sync critical options to shell state */
        if (strcmp(name, "lastpipe") == 0 && s) {
            s->shell_opts.lastpipe = set;
        }
        /* BUG-167: nullglob/failglob/nocaseglob were stored in this table but
         * never read anywhere else — every glob call site hardcoded "off".
         * Sync into shell_opts the same way lastpipe already is, so the glob
         * sites can actually check them. */
        if (strcmp(name, "nullglob") == 0 && s) {
            s->shell_opts.nullglob = set;
        } else if (strcmp(name, "failglob") == 0 && s) {
            s->shell_opts.failglob = set;
        } else if (strcmp(name, "nocaseglob") == 0 && s) {
            s->shell_opts.nocaseglob = set;
        }
        while (*p == ' ') p++;
    }
    return 0;
}
