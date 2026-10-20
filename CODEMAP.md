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

171 builtins, from `c/builtins/registry.c` (Linux/general profile of the `JASH_BUILTINS` table; port-only rows use separate conformance gates). Defined across `c/builtins/*.c`.

| builtin | does |
|---|---|
| `cd` | change working directory |
| `pwd` | print working directory |
| `exit` | exit jash |
| `true` | return 0 |
| `false` | return 1 |
| `:` | no-op (POSIX) |
| `echo` | print arguments |
| `printf` | format and print data |
| `help` | list builtins |
| `type` | identify how a name resolves |
| `which` | locate a command in PATH (builtin) |
| `version` | print jash version + codename |
| `uname` | print system information (universal via OSIF) |
| `osif` | OSIF capabilities, conformance, port report |
| `undo` | revert operations by full ID or guarded --ref; --json inspection/store/quota, --dry |
| `redo` | re-apply what undo just reverted (ADR-0050 W6) |
| `why` | diagnostico minimo do ultimo comando que falhou: o que rodou, como falhou, quando passou pela ultima vez, o que mudou desde entao |
| `resume` | bilhete de handoff entre sessoes que se reverifica: diz o que mudou no repo desde que foi escrito, e o que NAO foi conferido |
| `assert` | scoped revalidatable claims: --check CMD, --dep PATH, --timeout-ms N; --status inspects hashes without executing; --json |
| `timeline` | mutation history; --json with exact IDs and --offset/--limit live pages |
| `blame-file` | attested authorship per file: who/when/what-command mutated it (ADR-0042 F7, --json) |
| `distill` | mine the chain into an executable recipe (ADR-0042 F10; --param, --from/--to, --explain) |
| `recipe` | run/list/show/forget distilled recipes (ADR-0042 F10) |
| `clear` | clear the screen |
| `env` | list environment |
| `export` | set/list environment vars |
| `unset` | remove an env var |
| `alias` | list / set aliases |
| `unalias` | remove an alias |
| `history` | show command history |
| `fc` | fix command: list, edit, or re-run history entries |
| `test` | test expressions |
| `[` | test expressions ([ form) |
| `jobs` | list background jobs |
| `wait` | wait for background jobs |
| `fg` | bring job to foreground |
| `bg` | resume job in background |
| `disown` | stop tracking a job |
| `suspend` | suspend the shell (SIGSTOP) |
| `return` | return from function |
| `break` | break out of loop |
| `continue` | continue to next loop iteration |
| `source` | execute commands from a file |
| `.` | execute commands from a file (shorthand) |
| `read` | read a line from stdin into a variable |
| `exec` | execute a command (replaces the shell) |
| `eval` | evaluate arguments as a command line |
| `trap` | handle signals |
| `let` | evaluate arithmetic expressions |
| `shift` | shift positional parameters |
| `local` | create local variable in function scope |
| `readonly` | mark variables as readonly |
| `declare` | declare variable attributes |
| `typeset` | declare variable attributes (alias) |
| `ulimit` | get/set resource limits |
| `umask` | file creation mask |
| `times` | print shell and child process CPU times |
| `command` | run command avoiding shell function lookup |
| `set` | set shell options (-e, -u, -x) |
| `shopt` | shell options (bash compat) |
| `compgen` | generate completion matches |
| `complete` | manage completion |
| `array` | shell arrays (Rust) |
| `mapfile` | read lines into array variable |
| `readarray` | read lines into array variable (alias) |
| `getopts` | parse positional parameters |
| `pushd` | push directory and change directory |
| `popd` | pop directory and change directory |
| `dirs` | display directory stack |
| `abbr` | manage abbreviations (local/global/command-scoped) |
| `juxconfig` | configure interactive UX |
| `builtin` | execute builtin command directly |
| `enable` | enable or disable builtins |
| `hash` | hash table for command paths |
| `logout` | exit login shell |
| `plugin` | load/unload/list plugins |
| `module` | load/unload/list/info .modjash modules (ADR-0003) |
| `sigil` | get/set execution sigil level (off/log/sign/seal) |
| `intent` | set/show LLM session intent anchor (ADR-0022 F5) |
| `calc` | inline calculator (= EXPRESSION) |
| `jash_state` | manage persistent shell state |
| `theme` | switch color themes at runtime |
| `statusbar` | toggle intelligent status bar |
| `spinner` | toggle the idle prompt spinner |
| `kill` | kill by PID/name/pattern: -SIGNAL/-s, -l NUM/NAME lookup, --, full signal table, safety+dry-run (ADR-0044) |
| `ls` | list dir; --pretty color-indexed inventory with summary/limit; -t/-S/-r sort, -d, -l file/symlink→target, -R recurse; --delta, --json, --data\|--data-v2 [--where-type=file] [paths...] |
| `touch` | set/create file times: -t STAMP, -d DATE, -r REF, -c, -a/-m, -h, --json, --dry-run |
| `mkdir` | create dirs: -p, -m mode, --json, --dry-run; journaled, argv-native |
| `rmdir` | remove empty dirs: --json, --dry-run; journaled, argv-native |
| `chmod` | octal/symbolic modes, -R (≤64 journaled), --json, --dry-run; argv-native |
| `chown` | numeric uid[:gid], --json, --dry-run; journaled, argv-native |
| `ln` | hard/-s links: --json, --dry-run; journaled, argv-native |
| `cat` | concatenate files in-process (budget-capped, binary-safe, no fork); --color=auto\|always\|never/--highlight C-family source; --json metadata; --data byte-exact paged file content; -n/-b/-s/-E/-T/-A/-v/-- (ADR-0044) |
| `find` | recursive search in-process; --json emits bounded jash.query.find.v1 pages (default maxdepth 16); supported predicates are native, unsupported JSON predicates fail without external fallback |
| `glob` | expand one quoted pathname pattern in-process; --json typed file matches, --offset/--limit pages, ** recursive |
| `sed` | stream editor: s/re/repl/[gpiN] with -n/-e/-E/-i; --json reports native -i edits, --dry-run with JSON; legacy external fallback |
| `wc` | count in-process: -l/-w/-c/-m/-L; checked --json array; --json-report pages with line/word/byte totals and per-input errors |
| `stat` | file metadata: --json array or --json-report pages, -L, byte paths and nanoseconds; other forms use external stat |
| `xargs` | build & run command batches from stdin; -n/-I/-0/-t/-r/--; POSIX-quoted input splitting; GOLD: --plan previews the exact batches as JSON {argv} WITHOUT running (footgun-safe), and real runs go through the evaluator so each spawned command is attested/undoable/governed (\| xargs rm is recoverable); advanced flags fall back to real xargs (ADR-0044) |
| `diff` | compare two files in-process (Myers O(ND) engine): normal format, -u/-U N unified, \\ No newline marker, rc 0/1/2; GOLD: --json emits [{op,a:{start,count},b:{start,count},a_lines,b_lines}] hunks (no @@/</> parsing); byte-exact parity vs system diff is NOT the goal (round-trip-correct instead — see ADR-0044 diff design note); every other flag/binary/dir input falls back to real diff (ADR-0044) |
| `awk` | narrow in-process slice: single BEGIN/pattern-action/END program over $0.. $N/NF/NR, -F sep, /regex/ and relational patterns, print/printf, +=/-=/++/--; GOLD: --json streams [{fields:[...]}] per print (or {text:...} per printf), and the END-only aggregation idiom ({s+=$1} END{print s}) emits a single {result:val} object, not an array; control flow, arrays, functions, getline, multiple -v/-f, and every other construct fall back to real awk (ADR-0044) |
| `rm` | remove files: symlink-safe, -i/-I confirm, -d empty-dir, --dry-run, --json per-operand results, trash+undo; quote-aware argv |
| `trash` | recycle bin: move/list/restore; empty/gc --json --dry-run preserve recovery pins; on/off/status |
| `tread` | time-travel read: print FILE as it was at a checkpoint |
| `tdiff` | time-travel diff: checkpoint vs working tree |
| `grep` | search files (64KB/line cap): -P PCRE-lite, -e PAT, -A/-B/-C, -r --exclude-dir; GOLD: --json match records {file,line,col,match}, --since SEQ (chain-scoped recursive search), mtime-aware cache (real negative caching, no stale hits) |
| `timeout` | run COMMAND with a wall-clock limit: DURATION supports s/m/h/d + fractional; -s/--signal, -k/--kill-after (SIGKILL escalation), --preserve-status; kills the WHOLE process group (not just the direct child); GOLD: --json {exit_code,timed_out,signal_sent,duration_ms}, --verbose signal notices (ADR-0044) |
| `cp` | copy files: multi-source, -r/-a, -p preserve, -n no-clobber, -u update, --dry-run, --json per-action results; progress/resume/verify |
| `mv` | move files: -f/-n/-i/-v, -t/-T target, multi-source, EXDEV fallback, --dry-run, --json per-operand results, atomic (without JSON), undo |
| `ps` | list processes: -e/-A/ax all, -ef, -o fields, -C name, -p, --tree/--json/--sort (ADR-0044) |
| `proc` | process snapshots: snapshot\|restore\|tree\|diff (Wave 2) |
| `du` | disk usage: GNU-parseable SIZE\tPATH, -s/-h/-c/-a/-d N/--max-depth, multi-path, --json (ADR-0044) |
| `df` | filesystem usage: -i/--total, --json JSONL, --json-report pages, --baseline FILE/--delta FILE |
| `config` | manage jash configuration (init, set-key, show) |
| `context` | context compiler for agents: map/fold/bundle/ask (F0/ADR-0027) |
| `digest` | generate cached project briefing for agents (ADR-0028) |
| `state` | shell state and checked Git observations; --json, --data, --no-git |
| `json` | structured adapters for legacy CLIs (ADR-0026 Tier 1) |
| `speculate` | bounded speculative execution with observed copy, process and cleanup results; --json, --data |
| `revert` | cross-domain undo/revert (ADR-0026 Tier 1) |
| `cost` | estimate command cost: bytes, tokens, USD (ADR-0026 Tier 2) |
| `contract` | run command and assert output contracts (ADR-0026 Tier 2) |
| `agent-guard` | agent anti-bypass control panel (ADR-0026 Tier 3) |
| `adopt` | adoption governor: level, scorecard, metrics (ADR-0040 W2) |
| `agent-guide` | auto-generated agent field guide (ADR-0026 Tier 3) |
| `brain` | Liquid Reasoning Engine: suggest/intent/impact/learn/guard (ADR-0033) |
| `goal` | Intention Memory Caching: begin/step/verify/close/recall (ADR-0033) |
| `llm-invoice` | LLM token economics: begin/close/status/reset (ADR-0033 W3) |
| `kv` | agent key-value store (set/get/del/list) — survives across -c calls |
| `doctor` | structured environment health (disk/tmp/modules/keys) |
| `recall` | agent memory + chain BI (cmds/did/failed/stats/adoption, --json) |
| `breathe` | stuck agent detection: status/check/kill/timeout (ADR-0099) |
| `mirror` | verify runtime receipts with --json --receipt REF --blake3 HASH --pubkey KEY; automatic command history recording unavailable |
| `gossip` | process-local default; --channel NAME for persistent interprocess send/receive with cursors |
| `sense` | alias for `explain --risk` (ADR-0099 W2.3) |
| `forge` | unfinished C test scaffold; not implemented or validated code (ADR-0099) |
| `weave` | integrate multiple commands with unified output (ADR-0099) |
| `expect` | assert a fact about state (--json for structured verdict); ADR-0023 E3 |
| `explain` | preview a command's blast radius without executing it (ADR-0023 E3b) |
| `mesh` | multi-model swarm substrate: fleet registry + task board, --json (ADR-0024) |
| `gap` | capability-gap radar: report/mine/verify — demand-weighted, proof-closing bugfix queue |
| `dashboard` | BI: one-view of agent usage — adoption %, activity, fleet, safety (--json) |
| `checkpoint` | mark state for rewind; list/diff; checked --json; locked, synced TSV snapshot publication with bounded fields |
| `rewind` | roll back mutations to a checkpoint/--session; --dry to preview (ADR-0023 E2) |
| `verify` | blake3 file-integrity checkpoint (verify FILE [HASH]) |
| `change` | sovereign spec-driven change engine: schema/propose/status (ADR-0003 W1) |
| `mistake` | memory of own errors: log/avoid/list/clear (ADR-0034) |
| `token` | token economy visibility: count/optimize/history/budget (ADR-0034) |
| `learn` | code pattern learning: learn/list/recall/forget (ADR-0034) |
| `predict` | failure prediction: predict/risk assess/avoid (ADR-0034) |
| `risk` | risk assessment: risk assess/avoid (delegates to predict) |
| `preference` | user preference learning: learn/apply/recall/forget (ADR-0034) |
| `reflect` | self-reflection: reflect/improve suggest/apply (ADR-0034) |
| `selfcheck` | binary integrity check: store/verify/force (ADR-0034) |
| `prompt` | prompt optimization engine: optimize/analyze/compress (no LLM required) |
| `debug` | debugging tools: --parse, --locals, --profile |
| `lint` | static analysis: lint <file> \| -c 'code' \| --json <file> |
| `journal` | session timeline with causal auto-tags: --tag, --failed, --since, --what-happened |
| `stance` | operational modes: list, set <name>, show — see ADR-0035 |
| `impact` | test impact map learned from the chain: impact [--run] FILE... |
| `handoff` | distill the session into an LLM handoff brief: commands, failures, files, repo state |
| `where` | filter JSONL rows: FIELD=VAL, FIELD>NUM, ... |
| `pick` | project fields from JSONL: FIELD1,FIELD2,... |
| `sortby` | sort JSONL by field: [-r] FIELD |
| `count` | count JSONL records: [-g GROUP_FIELD] |
| `sum` | sum a numeric FIELD: [-g GROUP_FIELD] |
| `avg` | average a numeric FIELD: [-g GROUP_FIELD] |
| `uniqby` | deduplicate JSONL by FIELD: [-c] FIELD |
| `describe` | infer JSONL schema (field names, types, counts) |
| `graph` | living graph: build/explain/path/god-nodes/communities/hot/stats (ADR-0053) |
| `mem` | session memory distilled from the attested chain: distill/timeline/search/brief/verify (ADR-0054) |

---

## 3. Modules (`.modjash`, loaded on demand)

Loaded via `module load <name>` or auto (e.g. sigil when `JASH_SIGIL>=1`).
Source: `modules/<name>/<name>_module.c`; manifest: `modules/<name>/manifest.toml`.

| module | ver | verbs | purpose |
|---|---|---|---|
| `ai` | 0.2.0 | `-` | Jash AI agent — context expansion and command execution |
| `sigil` | 0.7.0 | `sigil` | Verifiable execution chain — BLAKE3 + ratchet + pledge + replay (S1-S9) |
| `undo` | 0.2.0 | `tx` | Transactional undo — tx begin/commit/rollback + sessions + agent safe mode (ADR-0018 W2-4) |

---

## 4. For LLMs — control knobs (env vars)

Set these to shape jash for agent use. (Auto-listed from `getenv("JASH_*")`.)

| env var | effect |
|---|---|
| `JASH_ABSORB` | (see source: grep getenv(\"JASH_ABSORB\")) |
| `JASH_ABSORBED` | (see source: grep getenv(\"JASH_ABSORBED\")) |
| `JASH_ABSORB_SPOOL` | (see source: grep getenv(\"JASH_ABSORB_SPOOL\")) |
| `JASH_ADOPT` | (see source: grep getenv(\"JASH_ADOPT\")) |
| `JASH_AGENT` | emit a machine-readable JSON result envelope on stderr (rc, ms, cwd, mutations) |
| `JASH_AGENT_CANCEL_FD` | (see source: grep getenv(\"JASH_AGENT_CANCEL_FD\")) |
| `JASH_AGENT_CGROUP_ROOT` | startup-pinned delegated cgroup-v2 parent for optional job resources; per-job setup must enforce every requested limit |
| `JASH_AGENT_FD` | startup-pinned writable fd for a separate foreground -c/exec-json envelope; implies agent JSON; command stdout/stderr stay separate |
| `JASH_AGENT_ID` | (see source: grep getenv(\"JASH_AGENT_ID\")) |
| `JASH_AGENT_PID` | (see source: grep getenv(\"JASH_AGENT_PID\")) |
| `JASH_BANNER` | startup banner toggle |
| `JASH_BENCH` | print boot/exec timing to stderr |
| `JASH_BG_ID` | (see source: grep getenv(\"JASH_BG_ID\")) |
| `JASH_BIN` | (see source: grep getenv(\"JASH_BIN\")) |
| `JASH_BRAIN` | (see source: grep getenv(\"JASH_BRAIN\")) |
| `JASH_BREATHE_INTERVAL` | (see source: grep getenv(\"JASH_BREATHE_INTERVAL\")) |
| `JASH_BREATHE_THRESHOLD` | (see source: grep getenv(\"JASH_BREATHE_THRESHOLD\")) |
| `JASH_BRIEF` | (see source: grep getenv(\"JASH_BRIEF\")) |
| `JASH_CHANGES` | (see source: grep getenv(\"JASH_CHANGES\")) |
| `JASH_CLAIM` | (see source: grep getenv(\"JASH_CLAIM\")) |
| `JASH_CLAIM_STRICT` | (see source: grep getenv(\"JASH_CLAIM_STRICT\")) |
| `JASH_CONTEXT_BUDGET` | context/budget hint for agent output shaping |
| `JASH_CP_PROGRESS` | (see source: grep getenv(\"JASH_CP_PROGRESS\")) |
| `JASH_DRIFT` | (see source: grep getenv(\"JASH_DRIFT\")) |
| `JASH_ERR_DIGEST` | compact error digest on failures |
| `JASH_ERR_FOLD` | (see source: grep getenv(\"JASH_ERR_FOLD\")) |
| `JASH_EXIT_CONFIRM` | (see source: grep getenv(\"JASH_EXIT_CONFIRM\")) |
| `JASH_GOLD` | (see source: grep getenv(\"JASH_GOLD\")) |
| `JASH_GOVERNOR` | (see source: grep getenv(\"JASH_GOVERNOR\")) |
| `JASH_GUARD` | destructive-command guard (strict = block, not warn) |
| `JASH_GUARD_BYPASS` | (see source: grep getenv(\"JASH_GUARD_BYPASS\")) |
| `JASH_GUARD_BYPASS_STRICT` | (see source: grep getenv(\"JASH_GUARD_BYPASS_STRICT\")) |
| `JASH_GUARD_STRICT` | destructive-command guard (strict = block, not warn) |
| `JASH_HINT_BUDGET` | (see source: grep getenv(\"JASH_HINT_BUDGET\")) |
| `JASH_INTENT` | intent-completion / hint subsystem toggle |
| `JASH_LLM_BUDGET` | cap output bytes for non-tty consumers (truncate to overflow file) |
| `JASH_LOOPGUARD` | (see source: grep getenv(\"JASH_LOOPGUARD\")) |
| `JASH_MEMO` | (see source: grep getenv(\"JASH_MEMO\")) |
| `JASH_MEMO_DELTA` | (see source: grep getenv(\"JASH_MEMO_DELTA\")) |
| `JASH_MEMO_TEST` | (see source: grep getenv(\"JASH_MEMO_TEST\")) |
| `JASH_MODULE_CACHE_EVENTS` | (see source: grep getenv(\"JASH_MODULE_CACHE_EVENTS\")) |
| `JASH_MODULE_CAPS_STRICT` | (see source: grep getenv(\"JASH_MODULE_CAPS_STRICT\")) |
| `JASH_MODULE_DEV_ROOT` | (see source: grep getenv(\"JASH_MODULE_DEV_ROOT\")) |
| `JASH_MODULE_REQUIRE_HASH` | (see source: grep getenv(\"JASH_MODULE_REQUIRE_HASH\")) |
| `JASH_MODULE_REQUIRE_SIGNATURE` | (see source: grep getenv(\"JASH_MODULE_REQUIRE_SIGNATURE\")) |
| `JASH_NUDGE` | (see source: grep getenv(\"JASH_NUDGE\")) |
| `JASH_PREEXEC_SNAPSHOT` | (see source: grep getenv(\"JASH_PREEXEC_SNAPSHOT\")) |
| `JASH_PREFETCH` | (see source: grep getenv(\"JASH_PREFETCH\")) |
| `JASH_PREFETCH_CHILD` | (see source: grep getenv(\"JASH_PREFETCH_CHILD\")) |
| `JASH_PREFETCH_MAX_MS` | (see source: grep getenv(\"JASH_PREFETCH_MAX_MS\")) |
| `JASH_PROVENANCE` | (see source: grep getenv(\"JASH_PROVENANCE\")) |
| `JASH_QUIET` | suppress idle statusbar/spinner noise (agent-friendly) |
| `JASH_RECALL_GLOBAL` | (see source: grep getenv(\"JASH_RECALL_GLOBAL\")) |
| `JASH_RECALL_GUARD` | (see source: grep getenv(\"JASH_RECALL_GUARD\")) |
| `JASH_RISK_CHECKPOINT` | (see source: grep getenv(\"JASH_RISK_CHECKPOINT\")) |
| `JASH_SAFE` | agent safety stack: load sigil+undo, enable transactional safety |
| `JASH_SALIENT_K` | (see source: grep getenv(\"JASH_SALIENT_K\")) |
| `JASH_SENSORY` | ambient sensory UI toggle |
| `JASH_SESSION` | persist cwd+env+aliases across separate `-c` calls (shared logical session) |
| `JASH_SIGIL` | verifiable execution: 0 off · 1 log (BLAKE3 chain) · 2 sign (Ed25519 proof) · 3 seal |
| `JASH_SIGIL_ALLOW_DEGRADED` | (see source: grep getenv(\"JASH_SIGIL_ALLOW_DEGRADED\")) |
| `JASH_SIGIL_TEST_TRUNCATE_FAIL` | (see source: grep getenv(\"JASH_SIGIL_TEST_TRUNCATE_FAIL\")) |
| `JASH_STALE_GUARD` | (see source: grep getenv(\"JASH_STALE_GUARD\")) |
| `JASH_TIMELINE` | (see source: grep getenv(\"JASH_TIMELINE\")) |
| `JASH_TINA_SURFACE_DEBUG` | (see source: grep getenv(\"JASH_TINA_SURFACE_DEBUG\")) |
| `JASH_TRASH` | (see source: grep getenv(\"JASH_TRASH\")) |
| `JASH_TRASH_TTL_H` | (see source: grep getenv(\"JASH_TRASH_TTL_H\")) |
| `JASH_VERBOSE` | (see source: grep getenv(\"JASH_VERBOSE\")) |
| `JASH_WORLD` | (see source: grep getenv(\"JASH_WORLD\")) |
| `JASH_WORLD_DEPTH` | (see source: grep getenv(\"JASH_WORLD_DEPTH\")) |

**JSON envelope** (`JASH_AGENT=1`): one object on **stderr** per `-c` —
`{rc, ms, tl, cwd, cpu_ms, maxrss_kb, jash, git, modabi, created/modified/deleted/moved[...]}`
(see `c/core/agent_json.c`). Parse that, don't scrape text. `git`/`modabi`
let an agent detect a stale binary; `jash --whoami` prints the same build id.

**Structured input** — `jash --exec-json '{"argv":["grep","foo bar","f.c"]}'`
runs argv **literally**: no word-splitting, globbing, $-expansion, or quote
round-trip. Use it to run commands whose args contain spaces/metachars.

**Structured output** — `--json` on `ls` `ps` `df` `du` `env` `pwd`
emits machine-readable data (JSONL records, or one object for env/pwd) instead of
text; each is an isolated path so the human text output is unchanged.

**Verify a session afterward** (no secret needed):
```
sigil chain verify          # recompute the BLAKE3 chain
sigil proof verify          # Ed25519, public-key only
build/jef_verify proof ~/.jash/session.jashproof   # standalone, links only libjef
```

---

## 5. Compatibility (CI-gated, not a baked %)

The A-Z battery exercises **94** POSIX/bash feature checks
(`tests/test_az_battery.sh`). It is **CI-gated at 100%** — the tree is only
green when all pass, so this map never claims a stale percentage. Run it live:
`bash tests/test_az_battery.sh`. Sovereign-attestation tests:
`tests/test_sigil.sh`, `tests/test_libjef.sh`, `tests/test_libjef_verify.sh`.

---

## 6. The tree

Source directories are discovered from the checkout. `build/` and
`build-speed/` are known output locations, listed even before a build creates them.

| dir | purpose |
|---|---|
| `build/` | known CMake build output location (generated artifacts) |
| `build-speed/` | known CMake build output location (generated artifacts) |
| `c/` | all C source (the shell itself) |
| `examples/` | real captured attestation artifacts + independent verifiers (W5.1) |
| `libjef/` | sovereign attestation engine as a standalone linkable lib (+ verifier) |
| `modules/` | loadable .modjash modules (sigil/undo/ai/slm-mesh) |
| `tests/` | test suites (A-Z battery, sigil, libjef, codemap) |
| `tools/` | repo tooling + experimental autonomy harness (jash_autonomy.py) |

Inside `c/`:

| dir | purpose |
|---|---|
| `c/builtins` | builtins + the dispatch registry |
| `c/core` | state, env, history, persist(session), agent_json, err_digest |
| `c/module` | module loader/ABI, sigil core, crypto (blake3, jash_crypto) |
| `c/osif` | portable syscalls / OS interface layer |
| `c/parser` | lexer, eval, exec, pipelines, heredocs, expansion, arithmetic |
| `c/readline` | input loop, line editing, completion, agent loop |
| `c/sys` | portable syscalls / OS interface layer |
| `c/theme` | prompt, banner, colors, output writers |
| `c/ui` | statusbar, progress |
| `c/main.c` | boot, REPL loop, the hot `-c` path, flag parsing |

---

## 7. Live status (not history)

- **`Docs/ROADMAP_STATUS.md`** — what's done / in-flight / next (source of truth).
- **`Docs/BUGFIX_ROADMAP.md`** — every observed bug + fix status.
- **`SPEC.md`** — on-disk formats (chain, jashproof, .modjash, libjef) + verifier checklist.
- `Docs/ADR-*` — deep history / "why" only. Do not assume they describe current code.

---

*Generated by `tools/gen_codemap.sh` from the source. Do not hand-edit — change
the code (or the generator's curated sections) and regenerate.*
