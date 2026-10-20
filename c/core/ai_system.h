// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — AI System Prompt and Guardrails
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>

#ifndef JASH_AI_SYSTEM_H
#define JASH_AI_SYSTEM_H

#include <stddef.h>  // NULL (header is otherwise include-order dependent)

// ── System Prompt ───────────────────────────────────────────────────────────
// This defines WHO the AI is. Runtime context selects display or auto mode.

#define JASH_AI_SYSTEM_PROMPT \
    "You are Jash, the intelligent assistant integrated into this shell.\n" \
    "The runtime context tells you whether commands are display-only or executable.\n" \
    "Never claim a command ran unless the runtime context says AUTO MODE.\n" \
    "\n" \
    "## Identity\n" \
    "- Answer directly in the user's language.\n" \
    "- In DISPLAY MODE, do not imply files were created: for explicit action requests, state briefly that nothing changed and tell the user to enable AUTO with `? auto` (or `? on auto`) before retrying.\n" \
    "- In AUTO MODE, use structured `jash_command` actions for commands and `jash_create_files` for a new-file batch; never pretend displayed source was saved. Do not repeat read-only inspections.\n" \
    "- When asked for source code, provide complete compilable code with all headers.\n" \
    "- Be direct, efficient, no fluff. The user wants results, not explanations.\n" \
    "\n" \
    "## Response Format\n" \
    "Keep prose concise and use Markdown where it improves readability.\n" \
    "For multi-item answers, use clear headings, short numbered lists, and a compact summary; avoid dumping raw, repetitive output.\n" \
    "Use language fences such as ```c for source code.\n" \
    "Use ```jash only for commands intended for the shell.\n" \
    "\n" \
    "## Available Builtins (Jash native — always available)\n" \
    "Navigation: cd, pwd, pushd, popd, dirs\n" \
    "Files: ls (--pretty for a compact color-indexed inventory), touch, rm, cp, mv, mkdir, grep\n" \
    "Info: echo, printf, cat (via less), head, tail, wc, history, type, help, version\n" \
    "Env: env, export, unset, alias, unalias\n" \
    "Process: kill, ps, jobs, fg, bg, wait, disown\n" \
    "System: df, du, clear, calc, date, uptime\n" \
    "Dev: git, make, gcc, python3, node, cmake, ninja\n" \
    "Shell: source, eval, exec, trap, let, set, shopt\n" \
    "State: jash_state (save/restore persistent state)\n" \
    "UI: theme, statusbar\n" \
    "\n" \
    "## Rules\n" \
    "1. Obey the runtime execution mode. DISPLAY MODE never executes commands.\n" \
    "2. Never say a file was created, compiled, or opened unless AUTO MODE did it.\n" \
    "3. AUTO MODE runs without confirmation prompts. Prefer Jash's recoverable builtins and report undo limits honestly. For directory orientation, prefer `ls --pretty` over `ls -la`; it groups and color-indexes entries and limits display to 48 rows. Use `--pretty-limit 0` only when the user requests the full listing.\n" \
    "4. If a command fails, explain why and suggest a fix.\n" \
    "5. When unsure, ask — don't guess.\n" \
    "6. Never execute: rm -rf /, dd, mkfs, shutdown, reboot, fork bombs.\n" \
    "7. Respond in the same language the user uses (Portuguese/English).\n" \
    "8. Be concise. No unnecessary explanations.\n" \
    "9. For multi-step AUTO MODE tasks, briefly state the plan and execute; do not wait for approval.\n" \
    "10. If you detect a pattern in user behavior, mention it proactively.\n" \
    "\n" \
    "## Context Awareness\n" \
    "You know:\n" \
    "- Recent conversations only when the Recent conversation memory block is supplied\n" \
    "- The current working directory and project context\n" \
    "- Time of day and session duration\n" \
    "Use this context to give smarter, faster answers. Never claim to remember past conversations unless their saved turns appear in the supplied memory block. The local memory is private to HOME and can be disabled with JASH_AI_MEMORY=0.\n"

// ── Blocked Commands (NEVER execute — no exceptions) ────────────────────────
static const char* JASH_AI_BLOCKED_COMMANDS[] = {
    // Total destruction
    "rm -rf /",
    "rm -rf /*",
    "rm -rf ~",
    "rm -rf $HOME",
    "rm -rf .",
    "dd if=/dev/zero of=/dev/sda",
    "dd if=/dev/zero of=/dev/nvme",
    "dd if=/dev/zero of=/dev/sdb",
    "mkfs",
    "fdisk",
    "parted",

    // System compromise
    "chmod 777 /",
    "chmod -R 777 /",
    "chown -R root:root /",
    "mount -o remount,rw /",

    // Sensitive data
    "cat /etc/shadow",
    "cat ~/.ssh/id_rsa",
    "cat ~/.ssh/id_ed25519",
    "cat /etc/sudoers",

    // Remote code execution: handled STRUCTURALLY by jash_ai_is_rce_pipe()
    // below. (Older literal "curl.*|.*sh" entries were dead — the matcher does
    // substring compare, not regex, so they never fired. Do not re-add them.)

    // Fork bomb
    ":(){ :|:& };:",

    // Shutdown
    "shutdown",
    "reboot",
    "halt",
    "poweroff",
    "init 0",
    "init 6",

    NULL
};

// ── Higher-risk command hints (never an interactive AUTO approval gate) ──────
static const char* JASH_AI_HIGH_RISK_COMMANDS[] = {
    // File deletion
    "rm ",
    "rmdir",
    "unlink",

    // File movement/overwrite
    "mv ",

    // Permission changes
    "chmod",
    "chown",
    "chgrp",

    // Process killing
    "kill",
    "killall",
    "pkill",

    // Disk operations
    "dd ",
    "mkswap",
    "swapon",
    "swapoff",

    // Package management
    "apt install",
    "apt-get install",
    "yum install",
    "dnf install",
    "pacman -S",
    "pip install",
    "npm install",
    "npm install -g",

    // Network (potentially dangerous)
    "curl -O",
    "wget ",
    "ssh ",
    "scp ",

    // System service
    "systemctl",
    "service",

    NULL
};

// ── Safe Commands (execute without asking) ──────────────────────────────────
static const char* JASH_AI_SAFE_COMMANDS[] = {
    // Read-only info
    "ls", "pwd", "echo", "printf", "cat", "head", "tail", "less", "more",
    "wc", "file", "stat", "du", "df", "date", "whoami", "id", "uname",
    "uptime", "hostname", "env", "printenv", "history", "type", "help",
    "version", "clear", "man", "which",

    // Search (read-only)
    "find", "grep", "rg", "fd", "locate", "tree",

    // Text processing (read-only)
    "sort", "uniq", "cut", "awk", "sed", "tr", "tee",

    // Dev (read-only)
    "git status", "git log", "git diff", "git show", "git branch",
    "git remote", "git stash list",
    "make --dry-run", "cmake --version", "gcc --version",
    "python3 --version", "node --version",

    // Process info (read-only)
    "ps", "jobs", "top -bn1", "free", "lscpu", "lsblk",

    // Network info (read-only)
    "ping -c", "ip addr", "ifconfig", "netstat", "ss",

    NULL
};

// ── Check if command is blocked (NEVER execute) ─────────────────────────────

// Sensitive path fragments that must never be READ regardless of which reader
// (cat/head/tail/less/more/strings/...) the model picks. Matched on the
// path-and-whitespace-normalized command, so `/etc//shadow` and `cat  /etc/shadow`
// are caught too.
static const char* JASH_AI_SENSITIVE_PATHS[] = {
    "/etc/shadow", "/etc/sudoers", "/etc/gshadow",
    "/.ssh/id_rsa", "/.ssh/id_ed25519", "/.ssh/id_ecdsa", "/.ssh/id_dsa",
    "/private/etc/master.passwd",
    NULL
};

// Normalize a command for robust matching: collapse runs of whitespace to a
// single space, collapse duplicate '/' in paths, drop a leading space. Bounded;
// never writes past cap-1. No libc dependency (safe for kernel-side use).
static inline void jash_ai_normalize(const char* in, char* out, unsigned cap) {
    unsigned o = 0;
    int prev_space = 1; // trim leading space
    if (cap == 0) return;
    for (const char* p = in; p && *p && o + 1 < cap; p++) {
        char c = *p;
        if (c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
            c = ' ';
        if (c == ' ') {
            if (prev_space) continue;     // collapse whitespace runs
            prev_space = 1;
            out[o++] = ' ';
            continue;
        }
        prev_space = 0;
        if (c == '/' && o > 0 && out[o-1] == '/') continue; // collapse '//'
        out[o++] = c;
    }
    while (o > 0 && out[o-1] == ' ') o--; // trim trailing
    out[o] = '\0';
}

// Bounded substring match (no libc, no regex).
static inline int jash_ai_has(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return 0;
    for (const char* p = hay; *p; p++) {
        const char* cp = p; const char* np = needle;
        while (*cp && *np && *cp == *np) { cp++; np++; }
        if (!*np) return 1;
    }
    return 0;
}

// STRUCTURAL remote-code-execution detector: any command that pipes (or
// command-substitutes / redirects) into a shell interpreter. Catches the whole
// `curl ... | sh`, `wget ... | bash`, `... | sudo bash`, `$(...)`-into-sh class
// regardless of the fetch tool, flags, or spacing. Operates on the normalized
// string. This is the real fix for the dead "curl.*|.*sh" literals.
static inline int jash_ai_is_rce_pipe(const char* norm) {
    // A network fetch must be present for this to be the curl|sh class.
    int has_fetch = jash_ai_has(norm, "curl") || jash_ai_has(norm, "wget") ||
                    jash_ai_has(norm, "fetch") || jash_ai_has(norm, "nc ") ||
                    jash_ai_has(norm, "ncat") || jash_ai_has(norm, "/dev/tcp");
    if (!has_fetch) return 0;

    // Walk every pipe; if a shell interpreter token appears anywhere in the
    // downstream segment, it's piping fetched bytes into an interpreter. This
    // catches `| sh`, `|bash`, `| sudo bash`, `| env X=1 sh`, etc. regardless
    // of spacing or intervening wrappers.
    static const char* shells[] = {
        "sh", "bash", "zsh", "ksh", "dash", "python", "perl", "ruby", NULL
    };
    for (const char* p = norm; *p; p++) {
        if (*p != '|') continue;
        const char* seg = p + 1;
        for (int i = 0; shells[i]; i++) {
            // require the shell token to be space- or pipe-delimited so that
            // "share"/"bashful" don't false-positive.
            const char* sp = seg;
            while (*sp) {
                const char* cp = sp; const char* tp = shells[i];
                while (*cp && *tp && *cp == *tp) { cp++; tp++; }
                if (!*tp) {
                    char after = *cp;
                    char before = (sp == seg) ? ' ' : sp[-1];
                    int bdelim = (before == ' ' || before == '|');
                    int adelim = (after == '\0' || after == ' ' ||
                                  after == '|' || after == ';' || after == '&');
                    if (bdelim && adelim) return 1;
                }
                sp++;
            }
        }
    }
    return 0;
}

static inline int jash_ai_is_blocked(const char* cmd) {
    if (!cmd) return 1;

    char norm[2048];
    jash_ai_normalize(cmd, norm, sizeof(norm));

    // 1. Structural RCE pipe (curl|sh and friends).
    if (jash_ai_is_rce_pipe(norm)) return 1;

    // 2. Sensitive-path reads, regardless of reader command.
    for (int i = 0; JASH_AI_SENSITIVE_PATHS[i]; i++)
        if (jash_ai_has(norm, JASH_AI_SENSITIVE_PATHS[i])) return 1;

    // 3. Literal blocklist, matched on the NORMALIZED command so that
    //    `rm  -rf /` and `rm\t-rf /` collapse to the canonical `rm -rf /`.
    for (int i = 0; JASH_AI_BLOCKED_COMMANDS[i]; i++)
        if (jash_ai_has(norm, JASH_AI_BLOCKED_COMMANDS[i])) return 1;

    return 0;
}

// ── Classification only; AUTO never waits for a confirmation prompt ─────────
static inline int jash_ai_high_risk(const char* cmd) {
    if (!cmd) return 0;
    if (jash_ai_is_blocked(cmd)) return 0; // blocked != needs confirm

    char norm[2048];
    jash_ai_normalize(cmd, norm, sizeof(norm));

    for (int i = 0; JASH_AI_HIGH_RISK_COMMANDS[i]; i++)
        if (jash_ai_has(norm, JASH_AI_HIGH_RISK_COMMANDS[i])) return 1;

    return 0;
}

// ── Check if command is safe (auto-execute) ─────────────────────────────────
static inline int jash_ai_is_safe(const char* cmd) {
    if (!cmd) return 0;
    if (jash_ai_is_blocked(cmd)) return 0;
    if (jash_ai_high_risk(cmd)) return 0;

    for (int i = 0; JASH_AI_SAFE_COMMANDS[i]; i++) {
        const char* safe = JASH_AI_SAFE_COMMANDS[i];
        const char* p = cmd;
        while (*p) {
            const char* cp = p;
            const char* sp = safe;
            while (*cp && *sp && *cp == *sp) { cp++; sp++; }
            if (!*sp) return 1;
            p++;
        }
    }
    return 0;
}

#endif // JASH_AI_SYSTEM_H
