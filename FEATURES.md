# JASH — Complete Feature Catalog

> **Generated from source code analysis.** Every feature described here exists in the
> codebase today. Not aspirational. Not roadmap. Read from the actual C source files.
>
> **5,029 files · 379 C source files · 113 headers · 127+ builtins · 3 modules**

---

## 0. Architecture Overview

```
┌──────────────────────────────────────────────────────────────┐
│                      JASH SHELL (ring 3)                      │
│  Pure C · POSIX-ish · Agent-first · Verifiable · Stateful    │
│                                                               │
│  c/main.c         Boot, REPL loop, ultra-fast -c path         │
│  c/builtins/      127+ builtin commands (registry.c)          │
│  c/core/          State, env, persist, guard, cache, AI       │
│  c/parser/        Lexer, eval, exec, pipelines, heredocs      │
│  c/module/        .modjash loader, sigil, crypto, BLAKE3      │
│  c/readline/      Input loop, line editing, completion        │
│  c/theme/         Prompt, banner, colors, output writers      │
│  c/ui/            Status bar, progress, spinner               │
│  c/osif/          Portable syscall layer (macOS/Linux/Beyond) │
│  c/sys/           Low-level syscall wrappers                  │
│  c/liquid/        Liquid Reasoning Engine (local ML)          │
│  c/journal/       Causal event journaling                     │
│  c/compiler/      Internal expression compiler                │
│  c/editor/        Snippet editor (CTRL+E)                     │
│  c/output/        Output highlighter                          │
│  c/completion/    Smart suggestions engine                    │
│                                                               │
│  modules/sigil/   Verifiable execution chain (.modjash)       │
│  modules/undo/    Transactional undo fabric (.modjash)        │
│  modules/ai/      Groq LLM integration (.modjash)             │
│                                                               │
│  libjef/          Sovereign Ed25519 standalone verifier       │
│  Docs/            ADRs, roadmap, bugfix tracking              │
│  tests/           A-Z battery, sigil tests, fuzz, maturity    │
└──────────────────────────────────────────────────────────────┘
```

---

## 1. Core Shell Engine

### 1.1 POSIX Shell Compatibility
- Full POSIX shell syntax (if/for/while/until/case/select)
- Pipelines, redirections (stdin/stdout/stderr, heredocs, here-strings)
- Command substitution `$()`, arithmetic `$(( ))`, parameter expansion `${}`
- Job control (bg, fg, jobs, disown, wait, coproc)
- Shell functions, aliases, abbreviations
- Traps, signals, umask, ulimit
- `set -e`/`-u`/`-x`/`-o pipefail`/`-o posix`/`-o noglob`/`-o noclobber`
- Arrays (Rust-backed), mapfile, readarray

### 1.2 JASH-Native Extensions

| Feature | Implementation | What it does |
|---|---|---|
| **Ultra-fast `-c` path** | `c/main.c:943` | Skips 355KB state init for trivial commands (`echo`, `true`, `exit`) — zero startup overhead |
| **Fast builtin dispatch** | `c/main.c:433` | `eval_command_mode_fast` bypasses full parser for simple commands |
| **Structured command input** | `c/core/serve.c` | `jash --exec-json '{"argv":[...],"cwd":…,"env":{…},"stdin":"…"}'` — argv literal (todo token citado, inclusive argv[0]), contexto aplicado ou pedido recusado. Mesma porta do `--serve` (JINV-016/017) |
| **`--capabilities-json`** | `c/core/caps_json.c` | Descritor legível por máquina do que ESTE jash aplica: máscara OSIF medida, nº de builtins, campos honrados do exec-json, verificação BLAKE3 exigida ou opcional (JINV-018) |
| **`--version` / `-V`** | `c/main.c:1044` | Print version + codename |
| **`--whoami`** | `c/main.c:1055` | Machine-readable build ID: version, codename, git hash, modabi — for agent drift detection |
| **`--agent-guide`** | `c/main.c:1063` | Auto-generated field guide of jash primitives |
| **`-i` (force interactive)** | `c/main.c:1069` | Force interactive mode (Beyond target) |
| **Self-healing TMPDIR** | `c/main.c:1077` | Auto-falls back to `~/.jash/tmp` when TMPDIR is unwritable |
| **Binary integrity auto-check** | `c/main.c:1094`, `builtin_selfcheck.c` | Detects if jash binary was replaced since last store |
| **Session token init** | `c/main.c:1097` | Tracking LLM token consumption |
| **Heredoc preprocessing** | `c/main.c:565` | Multi-line script with heredoc materialization, re-entrant expansion |
| **Backslash-newline continuation** | `c/main.c:768` | POSIX line continuation |
| **Compound statement accumulation** | `c/main.c:847` | Accumulates `if`/`for`/`while`/`case` blocks across multiple lines |
| **Function definition accumulation** | `c/main.c:812` | Accumulates `name() { ... }` across lines |
| **Quote-aware heredoc detection** | `c/main.c:550` | `<<` inside quotes is NOT treated as heredoc operator (BUG-036) |
| **cd.. shortcut** | `c/main.c:483` | `cd..` = `cd ..` |
| **Beyond Surface integration** | `c/main.c:87` | Native surface cell protocol (ERROR/PROGRESS/TABLE/PROMPT/INPUT) |
| **Ed25519 compute** | `main.c:1119` | Session-scoped Ed25519 public key for proof ledger |
| **`--resume=`** | `state.h` | Resume execution from checkpoint |
| **Frecency predictor** | `c/core/frecency.c` | Learns directory navigation patterns |
| **Smart Suggestions** | `c/completion/smart_suggestions.c` | Keystroke-driven learning from history |
| **Output Highlighter** | `c/output/highlighter.c` | Auto-detect & highlight command output |
| **Snippet Editor** | `c/editor/snippet.c` | CTRL+E multi-line editor |
| **Progress Bar** | `c/ui/progress.c` | Auto-detect long operations (>1s) |

---

## 2. Shell State & Environment

### 2.1 Central State (`c/core/state.h`)

The entire shell state is a single `jash_state_t` struct passed by pointer — no globals.

| Field | Size | Purpose |
|---|---|---|
| `cwd` | 512B | Current working directory |
| `env` | 512 entries | Environment table with O(1) hash lookup |
| `env_hash` | 1024 buckets | Hash table for env var lookup |
| `aliases` | 64 entries | Alias table |
| `abbr_global` | 64 entries | Global abbreviations |
| `abbr_local` | 32 entries | Local (project) abbreviations |
| `yank` | 1024B | Yank buffer (clipboard) |
| `last_status` | struct | Structured exit status (code, msg, suggestion, cmd, timing) |
| `shell_opts` | struct | 13 POSIX shell option flags |
| `jobs` | pointer | Job control table |
| `functions` | pointer | User-defined functions |
| `coprocs` | pointer | Coprocess table |
| `frecency` | struct | Frecency directory table |
| `predictions` | struct | Command prediction |
| `path_cache` | 128 entries | PATH executable cache with negative caching |
| `path_list` | 256 entries | Full PATH list for `did_you_mean` |
| `var_cache` | pointer | Variable expansion cache |
| `ss_engine` | pointer | Smart Suggestions engine |

### 2.2 Environment (`c/core/env.c`)

| Feature | Detail |
|---|---|
| **O(1) lookup** | FNV-1a hash table, 1024 buckets, linear probing |
| **Inline/heap hybrid** | Values ≤128B inline (zero alloc); longer values on heap |
| **Insertion-order iteration** | `jash_env_at()` preserves order |
| **In-place append** | `jash_env_append()` for PATH-like concatenation |
| **Swap-with-last compaction** | O(1) deletion on unset |
| **Full environ inheritance** | Copies every var from parent environ |
| **macOS path folding** | Reads `/etc/paths` + `/etc/paths.d/*` → PATH (skips `path_helper`) |
| **Default overrides** | SHELL, TERM, COLORTERM, JASH_VERSION, $0 |
| **Session persistence** | Saves/loads CWD + env + aliases across `-c` calls |

### 2.3 Session Persistence (`c/core/persist.c`)

| Feature | Detail |
|---|---|
| **Global state** | `~/.jash_state` — CWD, env, aliases across all sessions |
| **Scoped sessions** | `JASH_SESSION=<id>` → `~/.jash/sessions/<id>.state` |
| **Cross-`-c` state** | CWD restored even in non-interactive `-c` mode |
| **Graceful degradation** | Silently skips on full/RO filesystem |
| **Array-var filtering** | Skips array keys (`[`) during save |

---

## 3. Complete Builtin Reference

### 3.1 Shell-Essential (POSIX-compatible)

| Command | File | Description |
|---|---|---|
| `cd` | `c/builtins/cd.c` | Change working directory |
| `pwd` | `c/builtins/pwd.c` | Print working directory |
| `exit` | `c/builtins/exit.c` | Exit jash (with optional exit code) |
| `true` | `c/builtins/true_false.c` | Return 0 |
| `false` | `c/builtins/true_false.c` | Return 1 |
| `:` | `c/builtins/misc.c` | No-op (POSIX) |
| `echo` | `c/builtins/echo.c` | Print arguments |
| `printf` | `c/builtins/printf.c` | Format and print data |
| `help` | `c/builtins/help.c` | List builtins by category (categorized) |
| `type` | `c/builtins/type.c` | Identify how a name resolves (builtin/alias/PATH) |
| `which` | `c/builtins/which.c` | Locate command in PATH |
| `version` | `c/builtins/version.c` | Print jash version + codename |
| `uname` | `c/builtins/uname.c` | Print system info (via OSIF, cross-platform) |
| `osif` | `c/builtins/osif.c` | OSIF capabilities and port report |
| `osif port` | `c/builtins/osif_port.c` | OSIF port management |

### 3.2 File Operations (Builtin, No Subprocess)

| Command | File | Flags | JSON |
|---|---|---|---|
| `ls` | `builtin_ls.c` | `-l`, `-la`, `--tree`, `--json` | ✅ |
| `cp` | `builtin_cp.c` | `--resume`, `--dedup`, `--verify`, `-r` | ❌ |
| `mv` | `builtin_mv.c` | Batch, atomic, conflict resolution | ❌ |
| `rm` | `builtin_rm.c` | `-r`, `--undo`, `--dry-run` (trash-backed) | ❌ |
| `touch` | `builtin_touch.c` | Create/update timestamps (builtin perf) | ❌ |
| `trash` | `builtin_trash.c` | `list`, `restore <f>`, `empty`, `on`, `off`, `status` | ❌ |
| `grep` | `builtin_grep.c` | Smart cache, fuzzy matching, context | ❌ |
| `du` | `builtin_du.c` | `--cache`, `-h`, `-d N`, `-a`, `--top N`, `--json` | ✅ |
| `df` | `builtin_df.c` | `-h`, `--color`, `--no-color`, `-i`, `--json` | ✅ |

**Unique features vs bash equivalents:**
- **cp** — progress bar, resume interrupted copies, dedup (hardlinks for identical files), MD5 verify
- **rm** — trash integration (recoverable), dry-run preview, guard blocks deletion of jash binary
- **trash** — dedicated recycle bin with list/restore/empty + protection toggle
- **grep** — smart cache, fuzzy matching — builtin, no subprocess
- **du** — ASCII bar chart, `--top N`, smart caching per directory
- **df** — Unicode gradient bars (█▓▒░) with color thresholds, pipe detection for plain output

### 3.3 Process & Job Control

| Command | File | Flags |
|---|---|---|
| `ps` | `builtin_ps.c` | Smart filtering, tree view, cache |
| `kill` | `builtin_kill.c` | By PID, name, or pattern with safety checks |
| `jobs` | `builtin_jobs.c` | List background jobs |
| `wait` | `builtin_wait.c` | Wait for background jobs |
| `fg` | `builtin_fg.c` | Foreground job |
| `bg` | `builtin_bg.c` | Background job |
| `disown` | `builtin_disown.c` | Stop tracking a job |
| `suspend` | `builtin_suspend.c` | Suspend shell (SIGSTOP) |

### 3.4 Environment & Aliases

| Command | File | Description |
|---|---|---|
| `env` | `builtin_env.c` | List environment |
| `export` | `builtin_export.c` | Set/list env vars |
| `unset` | `builtin_unset.c` | Remove env var |
| `alias` | `builtin_alias.c` | List/set aliases |
| `unalias` | `builtin_unalias.c` | Remove alias |
| `hash` | `builtin_hash.c` | Command path hash table |
| `declare` | `builtin_declare.c` | Variable attributes |
| `typeset` | `builtin_declare.c` | Alias for declare |
| `readonly` | `builtin_readonly.c` | Mark readonly |
| `local` | `builtin_local.c` | Function-local scope |
| `shift` | `builtin_shift.c` | Shift positional params |
| `getopts` | `builtin_getopts.c` | Parse options |

### 3.5 History & Navigation

| Command | File | Description |
|---|---|---|
| `history` | `builtin_history.c` | Command history |
| `fc` | `builtin_fc.c` | Fix command (list/edit/re-run) |
| `pushd` | `builtin_pushd.c` | Push directory |
| `popd` | `builtin_popd.c` | Pop directory |
| `dirs` | `builtin_dirs.c` | Directory stack |
| `abbr` | `builtin_abbr.c` | Manage abbreviations (local/global/command-scoped) |

### 3.6 Scripting & Programming

| Command | File | Description |
|---|---|---|
| `source` / `.` | `builtin_source.c` | Execute from file |
| `read` | `builtin_read.c` | Read line into variable |
| `exec` | `builtin_exec.c` | Replace shell |
| `eval` | `builtin_eval.c` | Evaluate string |
| `trap` | `builtin_trap.c` | Signal handler |
| `let` | `builtin_let.c` | Arithmetic eval |
| `return` | `builtin_return.c` | Return from function |
| `break` | `builtin_break.c` | Break loop |
| `continue` | `builtin_continue.c` | Continue loop |
| `set` | `builtin_set.c` | Set shell options + show all vars |
| `shopt` | `builtin_shopt.c` | Bash-compat shell options |
| `test` / `[` | `builtin_test.c` | Expression evaluation |
| `calc` | `calc.c` | Inline calculator `= EXPRESSION` |
| `array` | `builtin_array.c` | Shell arrays (Rust) |
| `mapfile` / `readarray` | `builtin_mapfile.c` | Read lines to array |
| `times` | `builtin_times.c` | Shell + child CPU times |
| `ulimit` | `builtin_ulimit.c` | Resource limits |
| `umask` | `builtin_umask.c` | File creation mask |
| `command` | `builtin_command.c` | Bypass function lookup |
| `builtin` | `builtin_builtin.c` | Run builtin directly |
| `enable` | `builtin_enable.c` | Enable/disable builtins |
| `complete` | `builtin_complete.c` | Completion definitions |
| `compgen` | `builtin_compgen.c` | Generate completions |

### 3.7 Configuration & UI

| Command | File | Flags |
|---|---|---|
| `config` | `builtin_config.c` | `init`, `set-key <key>`, `key <key>`, `show` |
| `juxconfig` | `juxconfig.c` | `show`, `set <k> <v>` |
| `theme` | `builtin_theme.c` | List/switch/reload themes (amber, forest, ocean, mono, etc.) |
| `statusbar` | `builtin_statusbar.c` | `on`, `off`, `toggle`, `style <minimal\|compact\|full>` |
| `spinner` | `builtin_spinner.c` | `on`, `off`, `style <1-6>` |
| `jash_state` | `builtin_persist.c` | Manage persistent shell state |
| `plugin` | `builtin_plugin.c` | `load`, `unload`, `list` |
| `module` | `builtin_module.c` | `load`, `unload`, `list`, `info` |
| `logout` | `builtin_logout.c` | Exit login shell |
| `clear` | `builtin_clear.c` | Clear screen |
| `debug` | `builtin_debug.c` | `--parse`, `--locals`, `--profile` |

Sigil Defender remains an idle-only easter egg. Its three-lane arena escalates
through jumping, rare same-lane phase-shift, overclock and armored threats, with an ASCII
fallback and `NO_COLOR` support. It intentionally has no command name, leaving
`bear` available to the external Bear compiler.

### 3.8 Agent-Native Commands (THE DIFFERENTIATORS)

These are the commands that exist ONLY in JASH — designed for AI agents, not humans.

#### Verification & Proof

| Command | Subcommands | Description |
|---|---|---|
| `sigil` | `off\|log\|sign\|seal`, `chain`, `chain verify`, `proof emit`, `proof dsse`, `proof verify`, `proof show`, `history [--tail N]`, `blame <pattern>`, `diff <A> <B>`, `bisect [range]`, `caps`, `export`, `push`, `restore <seq>`, `replay [--exec] <seq>` | Verifiable execution chain: BLAKE3 hash chain + Ed25519/DSSE proof per session |
| `verify` | `FILE [HASH]` | BLAKE3 file-integrity checkpoint |
| `expect` | `[not] <predicate> ARG...`, `--json` | Assert state: file/dir/exec/empty/contains/match/rc — with structured verdict |
| `explain` | `<command>` | Preview blast radius WITHOUT executing |

#### Memory & Context

| Command | Subcommands | Description |
|---|---|---|
| `recall` | `cmds\|did\|history [pattern] [--failed] [--last N] [--json] [--tag T]`, `stats`, `adoption [--json]` | Agent chain memory — search attested execution history. Shows what ran, what failed, adoption BI |
| `kv` | `set KEY VALUE`, `get KEY`, `del KEY`, `list` | Agent key-value store — persists across `-c` calls |
| `brain` | `suggest`, `intent`, `impact`, `learn`, `reward`, `guard`, `status`, `reset` | Liquid Reasoning Engine — local ML predictions, no LLM needed |
| `goal` | `begin`, `step`, `verify`, `close`, `recall` | Intention Memory Caching |
| `context` | `map [dir]`, `fold <file>`, `ask` | Context compiler: repo digest, function signatures, code skeleton |
| `digest` | `--brief`, `--standard`, `--deep`, `--refresh`, `--output`, `--session` | Cached project briefing for agents |
| `mistake` | `log`, `avoid`, `list`, `clear` | Memory of own errors |
| `learn` | `learn`, `list`, `recall`, `forget` | Code pattern learning |
| `preference` | `learn`, `apply`, `recall`, `forget` | User preference learning |
| `reflect` | `reflect`, `improve`, `suggest`, `apply` | Self-reflection engine |

#### Safety & Undo

| Command | Subcommands | Description |
|---|---|---|
| `undo` | `[N]` | Revert last N destructive operations |
| `timeline` | (none) | Show session mutation history |
| `checkpoint` | `[name]`, `list`, `diff` | Mark state for rewind |
| `rewind` | `[name]`, `--session`, `--dry`, `list` | Roll back to checkpoint |
| `agent-guard` | `status`, `on`, `off`, `strict`, `relaxed` | Agent anti-bypass control panel |
| `trash` | `list`, `restore`, `empty`, `on`, `off`, `status` | Recycle bin with protection toggle |

#### State & Environment

| Command | Subcommands | Description |
|---|---|---|
| `state` | (none) | Live system state mirror (CWD, git, env, jobs in JSON) |
| `doctor` | (none) | Structured environment health check (disk/tmp/modules/keys) |
| `json` | (variant) | Structured adapters for legacy CLIs |
| `speculate` | `<command>` | Speculative execution sandbox (preview without committing) |
| `revert` | (variant) | Cross-domain undo (git, docker, etc.) |
| `intent` | (variant) | Set/show LLM session intent anchor |

#### Multi-Model & Token Economics

| Command | Subcommands | Description |
|---|---|---|
| `mesh` | `--json` | Multi-model swarm: fleet registry + task board |
| `cost` | `<command> [--json] [--dry-run]` | Estimate command cost: bytes, tokens, USD |
| `contract` | `<command> --expect rc N \| --expect stdout-contains X` | Run + assert output contracts |
| `token` | `count`, `optimize`, `history`, `budget` | Token economy visibility |
| `llm-invoice` | `begin`, `close`, `status`, `reset` | LLM token economics |

#### Development Tools

| Command | Subcommands | Description |
|---|---|---|
| `change` | `schema init/fork`, `propose`, `continue`, `ff`, `apply`, `verify`, `sync`, `archive`, `rewind`, `recall`, `status` | Spec-driven change engine (shell-native OpenSpec replacement) |
| `lint` | `<file> \| -c 'code' \| --json` | Static analysis |
| `journal` | `--tag`, `--failed`, `--since`, `--what-happened` | Session timeline with causal auto-tags |
| `predict` | `predict`, `risk`, `assess`, `avoid` | Failure prediction |
| `risk` | `risk`, `assess`, `avoid` | Risk assessment |
| `prompt` | `optimize`, `analyze`, `compress` | Prompt optimization (zero LLM required) |
| `selfcheck` | `store`, `verify`, `force` | Binary integrity check |
| `agent-guide` | `--markdown`, `--save` | Auto-generated field guide of agent primitives |

#### Stance System

| Command | Subcommands | Description |
|---|---|---|
| `stance` | `list`, `set <name>`, `show` | Operational modes: EXPLORE (8K budget), BUILD (6K), DEBUG (4K), REVIEW (5K), DEPLOY (4K) |

---

## 4. Cryptographic Attestation System (Sigil)

### 4.1 Sigil Levels

| Level | Env | Behavior |
|---|---|---|
| **0 — OFF** | `JASH_SIGIL=0` | No attestation |
| **1 — LOG** | `JASH_SIGIL=1` | BLAKE3 hash chain recording every command + exit code + timestamp |
| **2 — SIGN** | `JASH_SIGIL=2` | LOG + Ed25519 session proof emission on shutdown |
| **3 — SEAL** | `JASH_SIGIL=3` | SIGN + adversarial battery verification |

### 4.2 Hash Chain (`modules/sigil/sigil_module.c`)

```
h_n = BLAKE3(h_{n-1} || timestamp_ns || exit_code || command)
```

| Property | Detail |
|---|---|
| **Algorithm** | BLAKE3 (keyless) for chain hashing |
| **Forward secrecy** | Ratchet: `tag_n = BLAKE3_keyed(k_n, h_n)`, then `k_{n+1} = BLAKE3(k_n \|\| "r")` |
| **Storage** | `~/.jash/chain.log` — append-only text log |
| **Auto-truncation** | Keeps 10K lines max, trims to 5K |
| **Chain migration** | Auto-detects old SHA-256 v1 chain, renames to `.v1`, starts fresh BLAKE3 |
| **Session sentinel** | Auto-records `SESSION_END` on shutdown |
| **Persistence** | Survives across `-c` calls when `JASH_SIGIL>=1` |

### 4.3 Chain Commands

| Command | Description |
|---|---|
| `sigil chain` | Show last 20 chain entries |
| `sigil chain verify` | Recompute BLAKE3 for every line, check tags, detect tampering |
| `sigil history [--tail N]` | Colorized command log with exit codes |
| `sigil blame <pattern>` | Search chain by command text |
| `sigil diff <A> <B>` | Compare two chain entries |
| `sigil bisect [range]` | Find first failing command in range |

### 4.4 Proof System

| Feature | Detail |
|---|---|
| **Key type** | Ed25519 (RFC 8032) |
| **Key storage** | `~/.jash/keys/ed25519.seed` — auto-generated from `/dev/urandom` |
| **Proof format (text)** | `jashproof/1` — session_id, timestamps, user, host, chain_tip, pubkey, Ed25519 signature |
| **Proof format (DSSE)** | Dead Simple Signing Envelope — in-toto/sigstore compatible |
| **Proof file** | `~/.jash/session.jashproof` (text) and/or `~/.jash/session.jashproof.dsse` (DSSE) |
| **Proof verification** | Auto-detects DSSE vs text, verifies Ed25519 via libjef |
| **Standalone verifier** | `libjef/jef_verify` — read-only binary, needs no jash state, no secrets |
| **OCI export** | `sigil export` — chain + proof as OCI v1 image layout |
| **OCI push** | `sigil push` — export + delegate to `oras push` |

### 4.5 Adversarial Battery

On `sigil restore <seq>`, runs 4 attacks:

| Attack | What it checks |
|---|---|
| 1 — Hash forgery | Recompute BLAKE3 chain and compare |
| 2 — Timestamp replay | Timestamp not >2s in future |
| 3 — Sequence gap | Monotonic sequence continuity |
| 4 — Cross-session tamper | Verify proof MAC (BLAKE3-keyed or HMAC-SHA256) |

### 4.6 Crypto Primitives (No Dependencies)

| Primitive | File | Usage |
|---|---|---|
| BLAKE3 (keyless) | `c/module/blake3.c` | Chain hashing, session integrity |
| BLAKE3 (keyed) | `c/module/blake3.c` | Ratchet tags, proof MAC, KDF |
| BLAKE3 (chain) | `c/module/jash_crypto.c` | `h_n = BLAKE3(h_{n-1} \|\| payload)` |
| BLAKE3 (ratchet) | `c/module/jash_crypto.c` | `k_{n+1} = BLAKE3(k_n \|\| "r")` |
| Ed25519 | `libjef/ed25519.c` | Sovereign proof signing and verification |
| SHA-256 | `c/module/jash_crypto.c` | Legacy chain, OCI blob, HMAC |
| HMAC-SHA256 | `c/module/jash_crypto.c` | Legacy proof, session auth |
| SHA-512 | `libjef/ed25519.c` | Ed25519 internal hash |

### 4.7 Standalone Verifier (`libjef/jef_verify.c`)

| Feature | Detail |
|---|---|
| **Chain verification** | Recomputes BLAKE3 chain line by line — needs only chain.log |
| **DSSE verification** | Parses JSON envelope, base64-decodes payload+signature, verifies Ed25519 |
| **Text proof verification** | Parses `jashproof/1` format, extracts pubkey, verifies Ed25519 |
| **Auto-detection** | DSSE (leading `{`) vs text proof |
| **Third-party** | No jash state, no secrets — usable by anyone with the proof file and pubkey |

---

## 5. Agent Protocol (JASH_AGENT)

### 5.1 JSON Envelope (`c/core/agent_json.c`)

When `JASH_AGENT=1`, every `jash -c` invocation emits a JSON line to stderr:

```json
{
  "rc": 0,
  "ms": 12.34,
  "tl": 3,
  "cwd": "/home/user/project",
  "cpu_ms": 8,
  "maxrss_kb": 4096,
  "jash": "v0.4.0+abc1234",
  "codename": "...",
  "git": "abc1234",
  "modabi": 1,
  "created": ["file1"],
  "modified": ["file2"],
  "deleted": [],
  "moved": []
}
```

| Field | Description |
|---|---|
| `rc` | Exit code |
| `ms` | Wall clock time (milliseconds) |
| `tl` | Timeline depth (undo stack size) |
| `cwd` | Working directory |
| `cpu_ms` | CPU time (self + children from getrusage) |
| `maxrss_kb` | Peak RSS |
| `jash` | Version + git hash (drift detection) |
| `git` | Git hash |
| `modabi` | Module ABI version |
| `created/modified/deleted/moved` | File mutations from timeline |

### 5.2 Structured Input (`--exec-json`)

```
jash --exec-json '{"argv":["grep","foo bar","f.c"]}'
jash --exec-json '{"argv":["cat"],"cwd":"/repo","env":{"LC_ALL":"C"},"stdin":"bytes"}'
jash --exec-json '{"line":"cmake --build build | tail -3","cwd":"/repo"}'
```

- `argv` vai como array JSON — sem word-splitting, globbing ou volta pelo quoting
- **Todo** token é citado, inclusive `argv[0]`: um `"echo x; rm -rf y"` é um NOME
  de comando, nunca dois comandos (JINV-016 — antes disto, era)
- `cwd` / `env` / `stdin` são aplicados de verdade, e o pedido é **fail-closed**:
  um `cwd` inacessível RECUSA em vez de rodar no diretório errado (JINV-017)
- `line` dá semântica de shell completa pela mesma porta
- Mesmo protocolo, mesma implementação que `--serve` — uma porta só

### 5.2b Capability descriptor (`--capabilities-json`)

```
jash --capabilities-json
```

Uma linha JSON com o que este binário, nesta máquina, **aplica** — não o que
pretende: máscara OSIF medida (`have` / `missing`), nº de builtins da tabela
real, campos do exec-json de fato honrados, e `blake3_verify: required|optional`.
O campo aceito-mas-não-imposto (`timeout_ms`) se declara `"reserved"`, nunca
`true`; o gate `tests/test_jinv018_caps_json.sh` compara cada afirmação com o
comportamento observado do binário (JINV-018).

### 5.3 Agent Environment Variables

| Var | Values | Effect |
|---|---|---|
| `JASH_AGENT` | 1 | Enable JSON envelope output |
| `JASH_AGENT_ID` | string | Agent identity |
| `JASH_SESSION` | string | Session ID for state persistence |
| `JASH_SIGIL` | 0-3 | Attestation level |
| `JASH_LLM_BUDGET` | N (bytes) | Cap output, overflow to recall file |
| `JASH_CONTEXT_BUDGET` | N | Context budget hint |
| `JASH_SAFE` | 1 | Agent safety stack (load sigil+undo) |
| `JASH_QUIET` | 1 | Suppress statusbar/spinner noise |
| `JASH_BENCH` | 1 | Print boot/exec timing |
| `JASH_BANNER` | 0/1 | Toggle startup banner |
| `JASH_VERBOSE` | 1 | Verbose mode |
| `JASH_ERR_DIGEST` | 1 | Compact error digest |
| `JASH_GUARD` | 1 | Enable guard system |
| `JASH_GUARD_BYPASS` | 1 | Enable bypass guard |
| `JASH_GUARD_BYPASS_STRICT` | 1 | Block bypasses (vs warn) |
| `JASH_GUARD_STRICT` | 1 | Block dangerous ops (vs warn) |
| `JASH_TRASH` | on/off | Trash protection override |
| `JASH_INTENT` | 1 | Intent subsystem toggle |
| `JASH_NUDGE` | 1 | Nudge subsystem |
| `JASH_SENSORY` | 1 | Ambient sensory UI |
| `JASH_SKIP_SELFCHECK` | 1 | Skip binary integrity check |
| `JASH_EXIT_CONFIRM` | 0/1/never | Exit confirmation behavior |
| `JASH_BRAIN` | 1 | Enable liquid brain |
| `JASH_CP_PROGRESS` | 1 | CP progress display |

### 5.4 `--json` Support (Structured Output)

Builtins that support `--json` for machine-readable output:

`ls` · `ps` · `df` · `du` · `env` · `pwd` · `state` · `json` · `cost` · `expect` · `dashboard` · `mesh` · `recall` · `doctor` · `lint` · `change` · `digest`

Each outputs JSONL records (or single JSON object) on a dedicated output channel, keeping human text output unchanged.

---

## 6. Safety & Guard Systems

### 6.1 Bypass Guard (`c/core/guard.c`)

Detects when an agent tries to escape via `bash -c`, `sh -c`, `python -c`, `ruby -c`, `perl -c`, `node -c`, `php -c`:

| Mode | Behavior |
|---|---|
| **Watch** (`JASH_GUARD_BYPASS=1`) | Warn + suggest jash equivalent |
| **Strict** (`JASH_GUARD_BYPASS_STRICT=1`) | Block + error message |
| **Persisted** | Mode saved to `~/.jash/agent_guard_mode` |

**Jash differentiators** (21 primitives the guard encourages agents to use instead of shelling out): `recall`, `checkpoint`, `rewind`, `expect`, `explain`, `mesh`, `undo`, `timeline`, `trash`, `sigil`, `kv`, `context`, `verify`, `doctor`, `speculate`, `revert`, `cost`, `contract`, `state`, `digest`, `json`

### 6.2 RM Guard (`c/core/guard.c:298`)

- Blocks deletion of the jash binary itself (realpath + symlink target)
- Warns/blocks on `rm` of git repo roots
- Warns/blocks on `rm` of git working trees (strict mode)

### 6.3 Git Guard (`c/core/guard.c:339`)

Warns/blocks on dangerous git subcommands:
- `reset --hard`, `reset --mixed`
- `clean -f`
- `checkout --` (unstaged overwrite)
- `push --force`, `push -f`
- `restore` (non-`--staged`)

### 6.4 Adoption Metrics (`c/core/guard.c:98`)

Every command is recorded to `~/.jash/agent_metrics.tsv` with:
- Timestamp
- Native (jash builtin) / Bypass / Blocked classification
- Per-command differentiator tracking

`recall adoption` displays BI dashboard: total commands, jash-native %, bypass %, per-feature breakdown.

### 6.5 Trash Protection (`c/core/guard.c:214`)

Precedence: explicit toggle (`trash on|off`) > env var (`JASH_TRASH`) > sigil level (sigil≥1 auto-enables trash)

### 6.6 Context Engine (`c/core/context_engine.c`)

Unified context management for AI agent LLM sessions:

- **Provider registry**: dynamically register/unregister context providers (max 8)
- **4 built-in providers**: journal (tag history), git (repo state), state (shell state), digest (session digest)
- **Priority system**: pinned > priority number > timestamp recency
- **Token budgeting**: tracks total, reserved, per-provider consumption
- **Pruning**: evicts lowest-priority non-pinned items to fit budget
- **Default budget**: 8000 tokens
- **256 max context items**

### 6.7 Stance System (`c/core/stance.c`)

5 operational modes that control context budget + digest level:

| Stance | Budget | Digest Level | Use Case |
|---|---|---|---|
| EXPLORE | 8000 | DEEP | Broad context, all providers |
| BUILD | 6000 | STANDARD | File changes + build output |
| DEBUG | 4000 | BRIEF | Errors + failure patterns |
| REVIEW | 5000 | STANDARD | Git state + code review |
| DEPLOY | 4000 | BRIEF | Config + environment |

---

## 7. Transactional Undo System (`modules/undo/undo_module.c`)

### 7.1 Mutation Tracking

Every destructive operation is journaled:
- `FILE_OVERWRITE` — backups original to `~/.jash/tl_backup/` before writing
- `FILE_CREATE` — records path for deletion on undo
- `MOVE` — records source + destination for reverse
- `TRASH` — delegates to trash restore
- `ENV_SET` — records old value
- `ENV_UNSET` — records value for restore
- `CHDIR` — records previous CWD

### 7.2 Ring Buffer

- 128-entry circular buffer
- Oldest entries overwritten by newest
- Backup files capped at 16MB

### 7.3 Transaction Commands

| Command | Description |
|---|---|
| `tx begin [name]` | Start named transaction |
| `tx status` | Show active transaction state |
| `tx preview` | Preview pending mutations |
| `tx diff` | Summarize mutation types |
| `tx commit` | Finalize, compute tx_root (BLAKE3), bridge to sigil chain |
| `tx rollback` | Revert all mutations via undo |
| `tx save [name]` | Save session to disk |
| `tx sessions` | List saved sessions |
| `tx session show <id>` | Display session contents |
| `tx verify [id]` | Verify BLAKE3 integrity of session journal |
| `tx safe on\|off` | Toggle agent safe mode |
| `tx policy rollback-on-fail ask\|always\|never` | Auto-rollback policy |
| `tx risk <command>` | Evaluate command risk (0-5) |

### 7.4 Risk Scoring

| Score | Label | Patterns |
|---|---|---|
| 5 | CRITICAL | `rm -rf`, `dd`, `mkfs`, `format` |
| 4 | HIGH | `chmod 777`, `chown -R`, `git push --force` |
| 3 | MEDIUM | `rm file`, `mv /path`, `cp -r` |
| 2 | LOW | `touch`, `mkdir` |
| 1-0 | NONE | `ls`, `echo`, `cat` |

### 7.5 Persistence

- Sessions saved to `~/.jash/undo/sessions/<id>.journal`
- Format: metadata header + BLAKE3 integrity hash + mutation list
- Auto-save on shutdown
- Sigil bridge: records `TX_COMMIT`/`TX_ROLLBACK` in the attested chain

---

## 8. Liquid Reasoning Engine (`c/liquid/`, `builtin_brain.c`)

Local ML engine for command prediction. No LLM required. No external dependencies.

### 8.1 Features

| Subcommand | Description |
|---|---|
| `brain suggest ["last command"]` | Predict next useful command with intent, risk, confidence |
| `brain intent "text"` | Classify intent → 1 of 5 classes (build/debug/test/deploy/explore) |
| `brain impact <path>` | Estimate blast radius + anomaly risk |
| `brain learn <cmd> rc=N next="..."` | Online training example |
| `brain reward 1\|-1` | Reward/punish last prediction |
| `brain guard [on\|off\|status]` | Anomaly detection guard toggle |
| `brain status` | Model statistics |
| `brain reset` | Forget all weights |

### 8.2 Architecture

- Feature extraction: `jash_liquid_features(cmd, rc, intent_cached, impact_mode, cwd)`
- Input dimension: fixed-size feature vector
- Prediction: command class + intent class + risk score + confidence score
- Training: online SGD via `jash_liquid_learn()`
- Persistence: auto-save to disk via `jash_liquid_save()`
- Guard: anomaly detection with `~/.jash/brain_guard_mode` persistence

---

## 9. Smart Output Cache (`c/core/cache.c`)

### 9.1 Architecture

| Component | Value |
|---|---|
| Buckets | 256 (hash table) |
| Key | Command + CWD |
| Max value | 131KB |
| Eviction | TTL-based + file-dependency invalidation |
| Persistence | Binary file with magic + version validation |

### 9.2 Whitelist (Idempotent Commands)

| Command | TTL | Dependencies |
|---|---|---|
| `git status` | 5s | `.git/HEAD`, `index`, `MERGE_HEAD` |
| `git branch` | 5s | `.git/HEAD`, `refs/` |
| `git rev-parse` | 10s | — |
| `git log` | 5s | — |
| `git diff` | 3s | `.git/HEAD`, `index` |
| `git remote` | 30s | `.git/config` |
| `git tag` | 5s | `.git/refs/tags/` |
| `git describe` | 10s | `.git/HEAD`, `tags/` |
| `whoami`, `hostname`, `uname` | 3600s | — |
| `id`, `groups`, `which` | 60s | — |
| `pwd`, `dirname`, `basename` | ∞ | — |
| `echo`, `printf` | ∞ | — |
| `date`, `uptime` | 1-5s | — |

### 9.3 Blacklist (Never Cached)

`rm`, `mv`, `cp`, `chmod`, `chown`, `kill`, `sudo`, `su`, all redirects `>`, pipes `\|`, backgrounding `&`

---

## 10. Module System (.modjash)

### 10.1 Binary Format

| Section | Size | Content |
|---|---|---|
| Header | 64B | Magic `JMODv01`, ABI version, flags, BLAKE3 hash, sizes |
| Manifest | Variable | Name, version, description, capabilities required |
| Body | Variable | Shared library (.dylib/.so) |
| Signature | Variable | Optional Ed25519 signature |

### 10.2 Module Operations

| Hook | When | Purpose |
|---|---|---|
| `init` | Module load | One-time setup |
| `shutdown` | Module unload | Cleanup |
| `dispatch` | Command match | Handle verb |
| `pre_cmd` | Before every command | Intercept/transform |
| `post_cmd` | After every command | Record/audit |
| `exit` | Shell shutdown | Final cleanup |
| `mutation` | File state change | Track changes |

### 10.3 Capability System

13 defined capabilities: FS_READ, FS_WRITE, NET, EXEC, STATE_READ, STATE_WRITE, TTY, CRYPTO, MODULE_LOAD, HOOK_PRE_CMD, HOOK_POST_CMD, HOOK_EXIT, HOOK_MUTATION

Modules declare required caps in manifest; runtime enforcement via cap mismatch detection.

### 10.4 Search Path

1. `~/.jash/modules/<name>.modjash`
2. `./modules/<name>/<name>.modjash`
3. `./modules/<name>.modjash`

### 10.5 Loaded Modules

| Module | Version | Verbs | Purpose |
|---|---|---|---|
| `sigil` | 0.7.0 | `sigil` | Verifiable execution chain |
| `undo` | 0.2.0 | `tx` | Transactional undo fabric |
| `ai` | 0.1.0 | `?` | Groq LLM integration |

---

## 11. Journal System (`c/journal/`)

Causal event journaling with auto-tags:

| Tag | Trigger |
|---|---|
| `err` | Non-zero exit code |
| `retry` | Same command re-executed (within N lines) |
| `session_start` | New session detected |
| `resumed` | Session resumed after idle |
| `sw:task` | Mode/task switch detected |
| `sw:file` | File context switch detected |

Journal features:
- 64-entry ring buffer
- Auto-tagging by pattern matching
- `journal [--tag T] [--failed] [--since N] [--what-happened]`
- Integrated with `context` engine as first provider

---

## 12. AI Module (`modules/ai/ai_module.c`)

| Feature | Detail |
|---|---|
| **Provider** | Groq API |
| **Default model** | `llama-3.3-70b-versatile` |
| **API key path** | `~/.config/jash/groq.key` |
| **Context expansion** | `@file:path`, `@diff` (git diff), `@dir` (ls -la), `@git` (log) |
| **Code extraction** | Parses ` ```jash/bash/sh` blocks from LLM response |
| **Auto-execution** | `AUTO` mode = direct execution without confirmation |
| **Loop protection** | FNV-1a hash guardrail — blocks repeated commands |
| **Obsidian logging** | Saves to `~/.jash_obsidian/<date>.md` |
| **Temperature** | 0.3 |
| **Interface** | `? <question>` (interactive) or `ai <prompt>` (one-shot) |

---

## 13. Platform Support (OSIF Layer)

### 13.1 Supported Targets

| Target | Path |
|---|---|
| **macOS** | `c/osif/macos/` |
| **Linux** | `c/osif/linux/` |
| **BeyondBSD** | `c/osif/beyondbsd/` |

### 13.2 OSIF Builtins

| Command | Description |
|---|---|
| `osif` | Capabilities, conformance level, port report |
| `osif port` | Port management |
| `uname` | Universal uname across all targets |

---

## 14. UI/UX Features

### 14.1 Theme System

| Feature | Detail |
|---|---|
| **Built-in themes** | amber, forest, ocean, mono, etc. |
| **Runtime switching** | `theme <name>` — no restart needed |
| **Reload** | `theme reload` — reload current theme from disk |

### 14.2 Status Bar

| Mode | Content |
|---|---|
| `minimal` | Session, sigil level, command count |
| `compact` | Minimal + cost, latency, job count |
| `full` | Compact + git branch, frecency suggestion |

Toggle: `statusbar on|off|style minimal|compact|full`

### 14.3 Spinner

6 animation styles for idle state. Shows during AI queries (`?` command). Toggle: `spinner on|off|style <1-6>`

### 14.4 Output Highlighter

Auto-detect and highlight command output patterns (errors in red, warnings in yellow, success in green).

### 14.5 Progress Bar

Auto-detect long operations (>1s) and display animated progress.

### 14.6 Smart Suggestions

Keystroke-driven learning from history. Integrated via FREcency + prediction engine.

### 14.7 Snippet Editor

CTRL+E multi-line editing for complex commands.

### 14.8 Beyond Surface Integration

Native cell protocol (ERROR, PROGRESS, TABLE, PROMPT, INPUT, DIAG) when running on BeyondBSD surface. Falls back to ANSI on regular terminals.

---

## 15. Performance Characteristics

### 15.1 Benchmark (JASH vs BASH vs ZSH vs FISH)

| Test Case | Bash | Jash | Zsh | Fish |
|---|---|---|---|---|
| No-op (`:`) | 7.71ms | 8.89ms | 11.01ms | 18.35ms |
| `echo hello` | 7.19ms | 8.31ms | 10.47ms | 17.69ms |
| Arithmetic (1k loop) | 17.70ms | **12.55ms** | FAIL | FAIL |
| Arithmetic (10k loop) | 107.49ms | **40.27ms** | FAIL | FAIL |
| Pipe chain | 12.06ms | 13.09ms | 16.24ms | 20.86ms |
| Conditional | 6.75ms | 9.19ms | 9.93ms | FAIL |
| Function call | 6.69ms | 9.01ms | 9.61ms | FAIL |

**JASH wins on arithmetic**: 2.7× faster than BASH on 10k loop

### 15.2 Ultra-Fast `-c` Path

Trivial commands (`echo`, `true`, `false`, `exit`, `:`) with no shell expansion bypass 355KB state init entirely — zero startup cost.

### 15.3 Smart Output Cache

256-bucket hash table with TTL + file-dependency invalidation. Caches git status, git branch, whoami, hostname, and other idempotent commands.

### 15.4 Inline/Heap Hybrid Env Storage

Values ≤128B are inline (zero alloc); longer values use heap. Saves ~4.1MB vs fixed 8KB per entry.

---

## 16. Files & Persistence

### 16.1 State Files

| Path | Purpose |
|---|---|
| `~/.jash_state` | Global session state (CWD, env, aliases) |
| `~/.jash/sessions/<id>.state` | Scoped session state |
| `~/.jash/chain.log` | BLAKE3 attested execution chain |
| `~/.jash/chain.log.v1` | Legacy SHA-256 chain (migrated) |
| `~/.jash/session.jashproof` | Ed25519 session proof |
| `~/.jash/session.jashproof.dsse` | DSSE-enveloped proof |
| `~/.jash/keys/ed25519.seed` | Ed25519 identity keypair |
| `~/.jash/ratchet.key` | Ratchet key for forward secrecy |
| `~/.jash/session.key` | Legacy HMAC session key |
| `~/.jash/timeline.bin` | Undo timeline persistence |
| `~/.jash/tl_backup/<PID>/` | File backups before mutation |
| `~/.jash/token_usage.session` | Token consumption tracking |
| `~/.jash/agent_metrics.tsv` | Agent adoption metrics |
| `~/.jash/agent_guard_mode` | Agent guard persistence |
| `~/.jash/trash_mode` | Trash protection toggle |
| `~/.jash/brain_guard_mode` | Brain anomaly guard toggle |
| `~/.jash/undo/sessions/<id>.journal` | Undo session journals |
| `~/.jash/oci-export/` | OCI image layout export |
| `~/.jash/cache/jmod_*.dylib` | Module body cache |
| `~/.jash/tmp/` | Temp files (overflow, heredocs, AI) |
| `~/.jash_history` | Command history |
| `~/.jash_frecency` | Frecency directory rankings |
| `~/.jash_obsidian/<date>.md` | AI interaction log |
| `~/.config/jash/groq.key` | Groq API key |
| `.changes/` | Change engine artifacts |
| `.changes/schemas/` | Schema definitions |
| `.changes/archive/` | Archived change sets |

### 16.2 Directories Created

| Directory | Purpose |
|---|---|
| `~/.jash/` | Main config/data |
| `~/.jash/keys/` | Cryptographic keys |
| `~/.jash/sessions/` | Scoped session state |
| `~/.jash/tmp/` | Temporary files |
| `~/.jash/cache/` | Module body cache |
| `~/.jash/tl_backup/` | Timeline file backups |
| `~/.jash/undo/sessions/` | Transaction journals |
| `~/.jash/oci-export/` | OCI attestation export |
| `~/.jash/modules/` | Module installation |
| `~/.jash_obsidian/` | AI interaction journals |

---

## 17. Summary

| Category | Count |
|---|---|
| **Total source files** | 5,029 |
| **C source files** | 379 |
| **Header files** | 113 |
| **Builtins (registered)** | 127+ |
| **Builtins with `--json`** | 15+ |
| **Agent-native builtins** | 40+ (differentiators) |
| **Sigil levels** | 4 (OFF/LOG/SIGN/SEAL) |
| **Crypto primitives** | 6 (BLAKE3, Ed25519, SHA-256, SHA-512, HMAC-SHA256, FNV-1a) |
| **Module hooks** | 7 (init, shutdown, dispatch, pre_cmd, post_cmd, exit, mutation) |
| **Capabilities** | 13 |
| **Env vars** | 25+ |
| **Stances** | 5 (EXPLORE/BUILD/DEBUG/REVIEW/DEPLOY) |
| **Context providers** | 4 (journal, git, state, digest) |
| **Supported targets** | 3 (macOS, Linux, BeyondBSD) |
| **Tests** | 93 A-Z battery + sigil + libjef + fuzz + maturity |
| **Codemap enforced** | CI-gated at 100% |
