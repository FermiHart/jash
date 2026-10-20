// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — ADR-0003 M3: execution sigil state machine
//
// Level 0 (OFF):  no recording — zero-overhead hot path
// Level 1 (LOG):  log cmd + exit code to ~/.jash/commands.log
// Level 2 (SIGN): LOG + BLAKE3 + Ed25519 placeholder (S2)
// Level 3 (SEAL): SIGN + anti-bypass enforcement (S5)

#ifndef JASH_MODULE_SIGIL_H
#define JASH_MODULE_SIGIL_H

typedef enum {
    SIGIL_OFF  = 0,
    SIGIL_LOG  = 1,
    SIGIL_SIGN = 2,
    SIGIL_SEAL = 3,
} jash_sigil_level_t;

int  jash_sigil_get(void);
void jash_sigil_set(int level);

/* Auto-init from JASH_SIGIL env var — call once at startup */
void jash_sigil_auto_init(void);

/* Called by eval.c hook wiring at top-level eval (g_eval_depth == 0) */
void jash_sigil_pre_cmd(const char* line);
void jash_sigil_post_cmd(const char* line, int exit_code);

/* ADR-0050 W14: attested undo — proof the agent reverted its damage. */
void jash_sigil_attest(const char* event, const char* detail);

#endif /* JASH_MODULE_SIGIL_H */
