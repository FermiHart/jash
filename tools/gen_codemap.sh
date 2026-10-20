#!/usr/bin/env bash

# Pin collation: the map is diffed byte-for-byte by tests/test_codemap.sh,
# and glibc's locale-aware sort disagrees with macOS on underscores and
# case (JASH_ABSORB_SPOOL vs JASH_ABSORBED, Docs/ vs build/). Without this
# the freshness gate flips red on whichever host did not generate it.
export LC_ALL=C
# gen_codemap.sh — generate CODEMAP.md from the source of truth (the CODE).
#
# The auto sections (builtins, modules, env knobs, source dirs, battery count) are
# INTROSPECTED and checked for drift; descriptions are not behavioral proofs.
# Known build output locations are curated and listed even before they exist.
# The curated sections (the
# STOP header, intent->file index, knob descriptions, audience tours) are the
# only hand-written parts and live in the heredocs below.
#
# Usage:  bash tools/gen_codemap.sh > CODEMAP.md
# Verify: bash tests/test_codemap.sh   (regenerates and diffs — fails on drift)
#
# Deterministic: all globs/listings are sorted; no timestamps, no battery run.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# ── auto: builtin catalog (from the dispatch table) ─────────────────────────
builtins_table() {
    awk -F'"' '
        # The generated catalog is the Linux/general profile. Port-only
        # registry rows are present in source but excluded from the Linux
        # live denominator; their backend has separate conformance gates.
        /^[[:space:]]*[#]ifdef[[:space:]]+JASH_TARGET_SINGUOS([[:space:]]|$)/ { port_only=1; next }
        port_only && /^[[:space:]]*[#]endif([[:space:]]|$)/ { port_only=0; next }
        port_only { next }
        /^[[:space:]]*\{[[:space:]]*"/ && NF>=4 {
            name=$2; syn=$(NF-1);
            gsub(/\|/,"\\|",name); gsub(/\|/,"\\|",syn);
            printf "| `%s` | %s |\n", name, syn
        }' c/builtins/registry.c
}
builtins_count() { builtins_table | wc -l | tr -d ' '; }

# ── auto: module catalog (from manifests) ───────────────────────────────────
modules_table() {
    local m name ver desc verbs
    for m in $(ls modules/*/manifest.toml 2>/dev/null | sort); do
        name=$(sed -n 's/^name=//p' "$m" | head -1)
        ver=$(sed -n 's/^version=//p' "$m" | head -1)
        desc=$(sed -n 's/^description=//p' "$m" | head -1)
        verbs=$(sed -n 's/^provides_verbs=//p' "$m" | head -1)
        printf '| `%s` | %s | `%s` | %s |\n' \
               "$name" "${ver:-?}" "${verbs:--}" "${desc:--}"
    done
}

# ── auto: env knobs (from getenv calls) + curated one-liners ─────────────────
knob_doc() {
    case "$1" in
      JASH_AGENT)          echo "emit a machine-readable JSON result envelope on stderr (rc, ms, cwd, mutations)";;
      JASH_AGENT_CGROUP_ROOT) echo "startup-pinned delegated cgroup-v2 parent for optional job resources; per-job setup must enforce every requested limit";;
      JASH_AGENT_FD) echo "startup-pinned writable fd for a separate foreground -c/exec-json envelope; implies agent JSON; command stdout/stderr stay separate";;
      JASH_SESSION)        echo "persist cwd+env+aliases across separate \`-c\` calls (shared logical session)";;
      JASH_SIGIL)          echo "verifiable execution: 0 off · 1 log (BLAKE3 chain) · 2 sign (Ed25519 proof) · 3 seal";;
      JASH_LLM_BUDGET)     echo "cap output bytes for non-tty consumers (truncate to overflow file)";;
      JASH_CONTEXT_BUDGET) echo "context/budget hint for agent output shaping";;
      JASH_QUIET)          echo "suppress idle statusbar/spinner noise (agent-friendly)";;
      JASH_SAFE)           echo "agent safety stack: load sigil+undo, enable transactional safety";;
      JASH_ERR_DIGEST)     echo "compact error digest on failures";;
      JASH_GUARD|JASH_GUARD_STRICT) echo "destructive-command guard (strict = block, not warn)";;
      JASH_INTENT)         echo "intent-completion / hint subsystem toggle";;
      JASH_SENSORY)        echo "ambient sensory UI toggle";;
      JASH_BANNER)         echo "startup banner toggle";;
      JASH_BENCH)          echo "print boot/exec timing to stderr";;
      *)                   echo "(see source: grep getenv(\\\"$1\\\"))";;
    esac
}
knobs_table() {
    local k
    for k in $(grep -rhoE 'getenv\("JASH_[A-Z_]+"\)' c/ 2>/dev/null \
               | sed -E 's/getenv\("([^"]+)"\)/\1/' | sort -u); do
        printf '| `%s` | %s |\n' "$k" "$(knob_doc "$k")"
    done
}

# ── auto: battery check count (compat is CI-gated, never a baked %) ──────────
battery_checks() { grep -cE '^[[:space:]]*test_cmd "' tests/test_az_battery.sh 2>/dev/null || echo "?"; }

# ── auto: source dirs + curated build locations and purpose ─────────────────
top_level_dirs() {
    local d
    {
        for d in */; do
            [ -d "$d" ] && printf '%s\n' "${d%/}"
        done
        # Build artifacts are intentionally absent from a clean checkout.
        # Keep their known locations in the map regardless of local builds.
        printf '%s\n' build build-speed
    } | sort -u
}
dir_purpose() {
    case "$1" in
      c)        echo "all C source (the shell itself)";;
      libjef)   echo "sovereign attestation engine as a standalone linkable lib (+ verifier)";;
      modules)  echo "loadable .modjash modules (sigil/undo/ai/slm-mesh)";;
      tests)    echo "test suites (A-Z battery, sigil, libjef, codemap)";;
      examples) echo "real captured attestation artifacts + independent verifiers (W5.1)";;
      tools)    echo "repo tooling + experimental autonomy harness (jash_autonomy.py)";;
      build|build-speed) echo "known CMake build output location (generated artifacts)";;
      *)        echo "";;
    esac
}
csub_purpose() {
    case "$1" in
      c/main.c)        echo "boot, REPL loop, the hot \`-c\` path, flag parsing";;
      c/parser)        echo "lexer, eval, exec, pipelines, heredocs, expansion, arithmetic";;
      c/builtins)      echo "builtins + the dispatch registry";;
      c/core)          echo "state, env, history, persist(session), agent_json, err_digest";;
      c/module)        echo "module loader/ABI, sigil core, crypto (blake3, jash_crypto)";;
      c/readline)      echo "input loop, line editing, completion, agent loop";;
      c/theme)         echo "prompt, banner, colors, output writers";;
      c/ui)            echo "statusbar, progress";;
      c/sys|c/osif)    echo "portable syscalls / OS interface layer";;
      *)               echo "";;
    esac
}

# ════════════════════════════════════════════════════════════════════════════
cat <<'HDR'
# CODEMAP — read THIS, not the ADRs

> **Agents & humans: this is the entry point.** It is **generated from the code**
> (`tools/gen_codemap.sh`) and **CI-enforced** (`tests/test_codemap.sh` fails if
> the code adds a builtin/module/knob not reflected here). This verifies inventory
> freshness, not feature completeness: descriptions and curated sections still
> require code inspection and behavioral tests. Do **not** start with `Docs/ADR-*`:
> those are **intent and history**, often aspirational or superseded.
> For audits, follow `Docs/AUDITING.md`; this map indexes evidence, not verdicts.

**What jash is, today:** a pure-C POSIX-ish shell that is **verifiable**
(BLAKE3 hash-chain + Ed25519/DSSE proof of every command, via `modules/sigil` +
`libjef`), **stateful** across `-c` calls (`JASH_SESSION`), **structured** for
agents (`JASH_AGENT` JSON envelope), and **undoable** (`rm`→trash, `undo`).
Build: `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target jash` (`-Werror` obrigatório).

---

## 1. Intent → where in the code (start here)

| I want to… | Open |
|---|---|
| understand startup / the `-c` fast path | `c/main.c` → `main`, `ultra_fast_c_path`, `eval_command_mode_fast` |
| the core command evaluator | `c/parser/eval.c` → `jash_eval_line_inner` |
| how a command line is split (`;` `&&` `\|\|`) | `c/parser/eval.c` → `find_top_separator` |
| add / change a builtin | `c/builtins/registry.c` (the `JASH_BUILTINS` table) → `c/builtins/*.c` |
| arithmetic `$(( ))` | `c/parser/arithmetic.c` |
| variable / parameter expansion `$VAR` `${...}` | `c/parser/param_expand.c` |
| command substitution `$(...)` / backticks | `c/parser/cmdsub.c` |
| globbing / brace / tilde | `c/parser/expand.c` |
| for / while / until / case loops | `c/parser/for_loop.c`, `while_loop.c`, `select_loop.c`, `script.c` |
| pipelines & redirection | `c/parser/pipeline.c`, `c/parser/builtin_redir.c` |
| the verifiable execution chain (BLAKE3 + ratchet) | `modules/sigil/sigil_module.c` → `chain_append` |
| the Ed25519 / DSSE proof | `modules/sigil/sigil_module.c` → `cmd_proof_emit`/`_dsse`, `libjef/ed25519.c` |
| the independent verifier (no jash) | `libjef/jef_verify.c`, CLI `libjef/jef_verify_main.c` |
| concrete spec/start/terminal binding | `libjef/jef_verify.c` -> `jef_verify_job_spec` |
| scoped dependency-aware assertions | `c/builtins/builtin_assert.c` -> `jash_b_assert_argv` |
| aggregate job quotas and independent wall deadlines | `c/core/agent_cgroup.c`, `c/core/agent_process.c`; protocol in `c/core/serve.c` |
| job cleanup, managed listener shutdown and retained terminal observations | `c/core/serve.c`, `c/core/local_server.c`, `c/core/agent_process.c`; `Docs/AGENT_JOB_LIFECYCLE.md` |
| experimental isolated candidate/check/publication workflow | `tools/jash_autonomy.py`, `tools/autonomy_sandbox.py`; limits in `Docs/AUTONOMY.md` |
| crypto primitives (SHA-256/HMAC/BLAKE3) | `c/module/jash_crypto.c`, `c/module/blake3.c` |
| the module loader / ABI | `c/module/loader.c`, `c/module/modjash.h` |
| the JSON result envelope (agents) | `c/core/agent_json.c` |
| compiled agent contract inventory and shared job field validation | `c/core/agent_contract.c`, `c/core/json_writer.c`; `Docs/AGENT_CONTRACT.md` |
| native filesystem mutation JSON outcomes and delivery boundaries | `c/core/mutation_report.c`, `c/builtins/builtin_metaops.c`; `Docs/AGENT_MUTATIONS.md` |
| request-ID cancellation of native mutators and supervised exec | `c/core/agent_cancel.c`, `c/core/serve.c`, `c/core/mutation_report.c`; `Docs/AGENT_CANCELLATION.md` |
| checked df/stat/wc pages and checkpoint snapshot persistence | `c/builtins/builtin_{df,stat,wc,checkpoint}.c`; `Docs/AGENT_QUERIES_AND_STATE.md` |
| cross-`-c` session state | `c/core/persist.c` |
| env / variable storage | `c/core/env.c` |
| undo / trash / timeline | `c/core/timeline.c`, `modules/undo/undo_module.c` |
| guarded cross-process undo references and retained recovery data | `c/core/recovery.c`; `Docs/AGENT_RECOVERY.md` |
| agent artifact roots, exclusive GC and CAS/archive admission quota | `c/core/serve.c`, `c/core/recovery.c`; `Docs/AGENT_ARTIFACT_STORE.md` |
| signed maintenance intent/results and explicit root retention release | `c/core/serve.c`, `libjef/jef_verify.c`; `Docs/AGENT_ARTIFACT_STORE.md` |
| resolve interrupted maintenance result publication using signed staging | `c/core/serve.c` -> `srv_store_resolve`, `maintenance_candidate`; `Docs/AGENT_ARTIFACT_STORE.md` |
| inspect maintenance evidence by ID and verify signed preparation | `c/core/serve.c` -> `srv_maintenance_inspect`; `libjef/jef_verify.c` -> `jef_verify_maintenance_intent` |
| admit maintenance effects or close modern unadmitted preparation | `c/core/serve.c` -> `maintenance_decide`, `maintenance_close`; `libjef/jef_verify.c` -> `jef_verify_maintenance_decision` |
| discover maintenance IDs with guarded paginated history | `c/core/serve.c` -> `srv_maintenance_history`, `maintenance_history_scan`; `Docs/AGENT_ARTIFACT_STORE.md` |
| bound journal growth before effects and recover under logical quota pressure | `c/core/serve.c` -> `maintenance_journal_usage/admit/space`; `Docs/AGENT_ARTIFACT_STORE.md` |
| collect only redundant journal result staging and reconcile captured holds | `c/core/serve.c` -> `maintenance_journal_gc`, `journal_gc_same_result`; `Docs/AGENT_ARTIFACT_STORE.md` |
| local clients, private cancellation endpoints and persistent gossip | `c/core/local_server.c`, `c/osif/ipc.c`, `c/builtins/builtin_gossip.c`; `Docs/AGENT_LOCAL_SESSIONS.md` |
| observe managed budgets and configure typed data-plane policy | `c/core/serve.c` -> `srv_budget_get`, `srv_data_policy_command`; `Docs/AGENT_DATA_PLANE.md` |
| pass sealed CPU buffers and promote/attach durable artifacts | `c/osif/buffer.{c,h}`, `c/osif/ipc.{c,h}`, `c/core/serve.c`; `Docs/AGENT_DATA_PLANE.md` |
| stream bounded job output with explicit credits and per-consumer cursors | `c/osif/ipc.{c,h}`, `c/core/serve.c` -> `srv_stream_open`, `srv_stream_credit`; `Docs/AGENT_DATA_PLANE.md` |
| feed immutable artifacts or terminal stream ranges to external job stdin | `c/osif/buffer.{c,h}`, `c/core/serve.c` -> `srv_job_input_open`; `Docs/AGENT_DATA_PLANE.md` |
| bounded semantic values, linear typed streams, native filter, explicit JTV1 adapter and filesystem observations | `c/core/data_value.{c,h}`, `c/core/data_stream.{c,h}`, `c/core/data_filter.{c,h}`, `c/core/data_wire.{c,h}`, `c/core/data_wire_decode.c`, `c/builtins/ls_collector.{c,h}`, `c/builtins/ls_data.{c,h}`; `Docs/data/JTV1_TYPED_STREAM.md`; `tests/test_{data_value,data_stream,data_filter,data_wire,ls_collector,ls_data}.c` |
| versioned JTV2 native import boundary for typed streams | `Docs/data/JTV2_NATIVE_IMPORT_PLAN.md`; `c/core/data_wire_import.{c,h}`, `c/core/data_wire_decode.c`, `c/core/data_stream.c`; `tests/test_wire_sealed_import.c` |
| bounded read-only AI observation/count/source pages, composable native type/uid predicates, sealed refs and explicit authorized promotion gate | `c/core/ai_observation.{c,h}`, `c/core/ai_source_page.{c,h}`, `c/osif/buffer.{c,h}`, `modules/ai/ai_module.c` -> `ai_dispatch`; `Docs/data/AI_TYPED_OBSERVATIONS.md`; `tests/test_ai_observation{,_bounds}.c`, `tests/test_ai_source_page.c`, `tests/test_ai_typed_observation.py`, `tests/test_ai_ref_promotion.py`, `tests/ai_ref_fd_export.c` |
| approval-free AI auto with per-response timeline marker and scoped evidence | `modules/ai/ai_module.c`, `c/core/jash_ai.{c,h}`, `c/builtins/builtin_checkpoint.c`, `c/core/timeline.c`; `tests/test_ai_auto_session.py` |
| explicit bounded compile-repair with typed AI cells, delegated cgroup-v2 budgets, native `ls --data`, hash-guarded full-file and unique-hunk edits | `modules/ai/ai_module.c` -> `ai_repair_request`, `ai_repair_build`, `ai_apply_edit`; `c/core/agent_cgroup.{c,h}`, `c/core/ai_observation.c`, `c/core/ai_patch.{c,h}`; `tests/test_ai_auto_session.py`; `Docs/data/AI_TYPED_OBSERVATIONS.md` |
| measure repository-scoped attested agent adoption including digest | `c/builtins/builtin_recall.c` -> `jash_recall_chain_adoption`, `c/builtins/builtin_dashboard.c` -> `jash_b_dashboard`; `tests/test_recall.sh`, `tests/test_dashboard.sh` |
| produce bounded project briefings while excluding generated CMake build trees | `c/core/digest_engine.{c,h}` -> `digest_walk`, `digest_generated_cmake_dir`, `digest_pool_submit`, `digest_diff_render`, `digest_manifest_parse`, `digest_ledger_print`, `digest_cache_status_print`, `digest_fetch_print`; `c/builtins/builtin_digest.c` -> `jash_b_digest_argv`; `tests/test_digest_project.py` |
| Python scoped local client with read-only default | `integrations/jash_client.py`, `c/core/local_server.c`; `tests/test_python_local_client.py` |
| provider-independent read-only tool batch adapters | `integrations/jash_tools.py`, `modules/ai/ai_module.c` -> `ai_call_tool`; `tests/test_ai_tool_adapters.py` |
| read-only MCP stdio tools with session-local observation pages | `integrations/jash_mcp.py`, `integrations/jash_tools.py`; `tests/test_jash_mcp.py` |
| the sigil level state machine | `c/module/sigil.c` |
| the `sigil` CLI verbs | `c/builtins/sigil.c` + `modules/sigil/sigil_module.c` (`sigil_dispatch`) |

---

## 2. Builtins
HDR

echo
echo "$(builtins_count) builtins, from \`c/builtins/registry.c\` (Linux/general profile of the \`JASH_BUILTINS\` table; port-only rows use separate conformance gates). Defined across \`c/builtins/*.c\`."
echo
echo "| builtin | does |"
echo "|---|---|"
builtins_table

cat <<'MODHDR'

---

## 3. Modules (`.modjash`, loaded on demand)

Loaded via `module load <name>` or auto (e.g. sigil when `JASH_SIGIL>=1`).
Source: `modules/<name>/<name>_module.c`; manifest: `modules/<name>/manifest.toml`.

| module | ver | verbs | purpose |
|---|---|---|---|
MODHDR
modules_table

cat <<'KNOBHDR'

---

## 4. For LLMs — control knobs (env vars)

Set these to shape jash for agent use. (Auto-listed from `getenv("JASH_*")`.)

| env var | effect |
|---|---|
KNOBHDR
knobs_table

cat <<KNOBFOOT

**JSON envelope** (\`JASH_AGENT=1\`): one object on **stderr** per \`-c\` —
\`{rc, ms, tl, cwd, cpu_ms, maxrss_kb, jash, git, modabi, created/modified/deleted/moved[...]}\`
(see \`c/core/agent_json.c\`). Parse that, don't scrape text. \`git\`/\`modabi\`
let an agent detect a stale binary; \`jash --whoami\` prints the same build id.

**Structured input** — \`jash --exec-json '{"argv":["grep","foo bar","f.c"]}'\`
runs argv **literally**: no word-splitting, globbing, \$-expansion, or quote
round-trip. Use it to run commands whose args contain spaces/metachars.

**Structured output** — \`--json\` on \`ls\` \`ps\` \`df\` \`du\` \`env\` \`pwd\`
emits machine-readable data (JSONL records, or one object for env/pwd) instead of
text; each is an isolated path so the human text output is unchanged.

**Verify a session afterward** (no secret needed):
\`\`\`
sigil chain verify          # recompute the BLAKE3 chain
sigil proof verify          # Ed25519, public-key only
build/jef_verify proof ~/.jash/session.jashproof   # standalone, links only libjef
\`\`\`

---

## 5. Compatibility (CI-gated, not a baked %)

The A-Z battery exercises **$(battery_checks)** POSIX/bash feature checks
(\`tests/test_az_battery.sh\`). It is **CI-gated at 100%** — the tree is only
green when all pass, so this map never claims a stale percentage. Run it live:
\`bash tests/test_az_battery.sh\`. Sovereign-attestation tests:
\`tests/test_sigil.sh\`, \`tests/test_libjef.sh\`, \`tests/test_libjef_verify.sh\`.

---

## 6. The tree

Source directories are discovered from the checkout. \`build/\` and
\`build-speed/\` are known output locations, listed even before a build creates them.

| dir | purpose |
|---|---|
KNOBFOOT
for d in $(top_level_dirs); do
    p="$(dir_purpose "$d")"; [ -n "$p" ] && printf "| \`%s/\` | %s |\n" "$d" "$p"
done
echo
echo "Inside \`c/\`:"
echo
echo "| dir | purpose |"
echo "|---|---|"
for d in $(ls -d c/*/ 2>/dev/null | sed 's:/$::' | sort) c/main.c; do
    p="$(csub_purpose "$d")"; [ -n "$p" ] && printf "| \`%s\` | %s |\n" "$d" "$p"
done

cat <<'FOOT'

---

## 7. Live status (not history)

- **`Docs/ROADMAP_STATUS.md`** — what's done / in-flight / next (source of truth).
- **`Docs/BUGFIX_ROADMAP.md`** — every observed bug + fix status.
- **`SPEC.md`** — on-disk formats (chain, jashproof, .modjash, libjef) + verifier checklist.
- `Docs/ADR-*` — deep history / "why" only. Do not assume they describe current code.

---

*Generated by `tools/gen_codemap.sh` from the source. Do not hand-edit — change
the code (or the generator's curated sections) and regenerate.*
FOOT
