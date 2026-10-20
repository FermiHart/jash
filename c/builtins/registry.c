// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: builtins/registry — the table of builtins.

#include "builtin.h"
#include "recovery_output.h"
#include "analysis_output.h"
#include <string.h>   /* JINV-046: strcmp */
#include <core/timeline.h>
#ifndef JASH_AGENT_ONLY
#include <module/loader.h>
#include <plugin.h>
#endif
#include <core/live_reload.h>
#include <core/argv.h>

static int j_streq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

/* The minimal canonical set. Bin utilities are NOT here. They are
 * resolved by PATH lookup; builtins/fallback utilities may shim them if bin
 * is empty. */
const jash_builtin_t JASH_BUILTINS[] = {
    /* shell-essential */
    { "cd",       jash_b_cd,       "change working directory", jash_b_cd_argv },
    { "pwd",      jash_b_pwd,      "print working directory", jash_b_pwd_argv },
    { "exit",     jash_b_exit,     "exit jash" },
    { "true",     jash_b_true,     "return 0" },
    { "false",    jash_b_false,    "return 1" },
    { ":",        jash_b_colon,    "no-op (POSIX)" },
    /* echo — JINV-079: a porta VALIDA e a de ARGV. A saida do `echo` e a
     * juncao de argv[1..n] com UM espaco; a linha crua ja dissolveu as
     * fronteiras de palavra e nao permite reconstrui-la (`echo "  x"` e
     * `echo   x` chegam iguais). A `fn` de string continua registrada como
     * fallback do dispatcher — nada foi apagado. */
    { "echo",     jash_b_echo,     "print arguments", jash_b_echo_argv },
    { "printf",   0,   "format and print data", jash_b_printf },  /* ADR-0036 W2: argv_fn */
#ifdef JASH_TARGET_SINGUOS
    /* SinguOS port (c/osif/singuos/): kernel-integrated builtins. Never
     * compiled into host builds. */
    { "irq",      jash_b_irq,      "serial IRQ statistics (Beyond surface ISR counter)" },
    { "genesis",  jash_b_genesis,  "inspect Genesis state or freeze the fixed probe" },
    { "ring3-selftest", jash_b_ring3_selftest, "probe hostile syscall boundaries" },
    { "cage",     0,   "arm a fail-closed GAIOLA resource envelope on a forked child, then exec (ADR-0089 W4b)", jash_b_cage },
#endif
    { "help",     jash_b_help,     "list builtins", jash_b_help_argv },
    { "type",     jash_b_type,     "identify how a name resolves", jash_b_type_argv },
    { "which",    jash_b_which,    "locate a command in PATH (builtin)", jash_b_which_argv },
    { "version",  jash_b_version,  "print jash version + codename", jash_b_version_argv },
    { "uname",    jash_b_uname,    "print system information (universal via OSIF)", jash_b_uname_argv },
    { "osif",     jash_b_osif,     "OSIF capabilities, conformance, port report", jash_b_osif_argv },
    { "undo",     jash_b_undo,     "revert operations by full ID or guarded --ref; --json inspection/store/quota, --dry", jash_b_undo_argv },
    { "redo",     jash_b_redo,     "re-apply what undo just reverted (ADR-0050 W6)", jash_b_redo_argv },
    { "why",      jash_b_why,      "diagnostico minimo do ultimo comando que falhou: o que rodou, como falhou, quando passou pela ultima vez, o que mudou desde entao", jash_b_why_argv },
    { "resume",   jash_b_resume,   "bilhete de handoff entre sessoes que se reverifica: diz o que mudou no repo desde que foi escrito, e o que NAO foi conferido", jash_b_resume_argv },
    { "assert",   jash_b_assert,   "scoped revalidatable claims: --check CMD, --dep PATH, --timeout-ms N; --status inspects hashes without executing; --json", jash_b_assert_structured_argv },
    { "timeline", jash_b_timeline, "mutation history; --json with exact IDs and --offset/--limit live pages", jash_b_timeline_argv },
    { "blame-file", jash_b_blamefile, "attested authorship per file: who/when/what-command mutated it (ADR-0042 F7, --json)", jash_b_blamefile_argv },
    { "distill",  jash_b_distill,  "mine the chain into an executable recipe (ADR-0042 F10; --param, --from/--to, --explain)", jash_b_distill_argv },
    { "recipe",   jash_b_recipe,   "run/list/show/forget distilled recipes (ADR-0042 F10)", jash_b_recipe_argv },
#ifndef JASH_AGENT_ONLY
    { "clear",    jash_b_clear,    "clear the screen" },
#endif
    /* env / alias / history */
    { "env",      jash_b_env,      "list environment", jash_b_env_argv },
    { "export",   jash_b_export,   "set/list environment vars" },
    { "unset",    jash_b_unset,    "remove an env var", jash_b_unset_argv },
#ifndef JASH_AGENT_ONLY
    { "alias",    jash_b_alias,    "list / set aliases", jash_b_alias_argv },
    { "unalias",  jash_b_unalias,  "remove an alias", jash_b_unalias_argv },
    { "history",  jash_b_history,  "show command history" },
    { "fc",       jash_b_fc,       "fix command: list, edit, or re-run history entries" },
#endif
    /* test — JINV-066: a porta VALIDA e a de argv (a semantica do POSIX se
     * define por contagem de argumentos, nao por prefixo na linha crua). A
     * `fn` de string continua registrada como fallback do dispatcher. */
    { "test",     jash_builtin_test,     "test expressions", jash_builtin_test_argv },
    { "[",        jash_builtin_bracket,  "test expressions ([ form)", jash_builtin_bracket_argv },
    /* Job control */
    { "jobs",     jash_b_jobs,           "list background jobs", jash_b_jobs_argv },
    { "wait",     jash_b_wait,           "wait for background jobs" },
    { "fg",       jash_b_fg,             "bring job to foreground" },
    { "bg",       jash_b_bg,             "resume job in background" },
    { "disown",   jash_b_disown,         "stop tracking a job" },
    { "suspend",  jash_b_suspend,        "suspend the shell (SIGSTOP)" },
    /* Functions */
    { "return",   jash_b_return,         "return from function" },
    /* Loop control */
    { "break",    jash_b_break,          "break out of loop" },
    { "continue", jash_b_continue,       "continue to next loop iteration" },
    /* Script */
#ifndef JASH_AGENT_ONLY
    { "source",   jash_b_source,   "execute commands from a file" },
    { ".",        jash_b_source,   "execute commands from a file (shorthand)" },
#endif
    { "read",     jash_b_read,     "read a line from stdin into a variable" },
    { "exec",     jash_b_exec,     "execute a command (replaces the shell)" },
#ifndef JASH_AGENT_ONLY
    { "eval",     jash_b_eval,     "evaluate arguments as a command line" },
#endif
    { "trap",     jash_b_trap,     "handle signals" },
    { "let",      jash_b_let,      "evaluate arithmetic expressions" },
    { "shift",    jash_b_shift,    "shift positional parameters" },
    { "local",    jash_b_local,    "create local variable in function scope" },
    { "readonly", jash_b_readonly, "mark variables as readonly" },
    { "declare",  jash_b_declare,  "declare variable attributes" },
    { "typeset",  jash_b_typeset,  "declare variable attributes (alias)" },
    { "ulimit",   jash_b_ulimit,   "get/set resource limits", jash_b_ulimit_argv },
    { "umask",    jash_b_umask,    "file creation mask", jash_b_umask_argv },
    { "times",    jash_b_times,    "print shell and child process CPU times", jash_b_times_argv },
    { "command",  jash_b_command,  "run command avoiding shell function lookup" },
    { "set",      jash_b_set,            "set shell options (-e, -u, -x)" },
    { "shopt",    jash_b_shopt,          "shell options (bash compat)", jash_b_shopt_argv },
#ifndef JASH_AGENT_ONLY
    { "compgen",  jash_b_compgen,        "generate completion matches" },
    { "complete", jash_b_complete,       "manage completion" },
#endif
    { "array",    jash_b_array,          "shell arrays (Rust)", jash_b_array_argv },
    { "mapfile",  jash_b_mapfile,        "read lines into array variable" },
    { "readarray",jash_b_mapfile,        "read lines into array variable (alias)" },
    { "getopts",  jash_b_getopts,        "parse positional parameters" },
    { "pushd", jash_b_pushd, "push directory and change directory", jash_b_pushd_argv },
    { "popd", jash_b_popd, "pop directory and change directory", jash_b_popd_argv },
    { "dirs", jash_b_dirs, "display directory stack", jash_b_dirs_argv },
#ifndef JASH_AGENT_ONLY
    { "abbr", jash_b_abbr, "manage abbreviations (local/global/command-scoped)" },
    { "juxconfig", jash_b_juxconfig, "configure interactive UX" },
#endif
    { "builtin", jash_b_builtin_cmd, "execute builtin command directly" },
    { "enable", jash_b_enable, "enable or disable builtins", jash_b_enable_argv },
    { "hash", jash_b_hash, "hash table for command paths", jash_b_hash_argv },
    { "logout", jash_b_logout, "exit login shell" },
#ifndef JASH_AGENT_ONLY
    { "plugin", jash_b_plugin, "load/unload/list plugins", jash_b_plugin_argv },
    { "module", jash_b_module, "load/unload/list/info .modjash modules (ADR-0003)", jash_b_module_argv },
    { "sigil",  jash_b_sigil,  "get/set execution sigil level (off/log/sign/seal)", jash_b_sigil_argv },
#endif
    { "intent", jash_b_intent, "set/show LLM session intent anchor (ADR-0022 F5)", jash_b_intent_argv },
    { "calc", jash_b_calc, "inline calculator (= EXPRESSION)" },
    { "jash_state", jash_b_jash_state, "manage persistent shell state", jash_b_jash_state_argv },
#ifndef JASH_AGENT_ONLY
    { "theme", jash_b_theme, "switch color themes at runtime" },
    { "statusbar", jash_b_statusbar, "toggle intelligent status bar" },
    { "spinner", jash_b_spinner, "toggle the idle prompt spinner" },
#endif
    { "kill", jash_b_kill, "kill by PID/name/pattern: -SIGNAL/-s, -l NUM/NAME lookup, --, full signal table, safety+dry-run (ADR-0044)", jash_b_kill_argv },
    { "ls", jash_b_ls, "list dir; --pretty color-indexed inventory with summary/limit; -t/-S/-r sort, -d, -l file/symlink→target, -R recurse; --delta, --json, --data|--data-v2 [--where-type=file] [paths...]", jash_b_ls_argv },
    { "touch", 0, "set/create file times: -t STAMP, -d DATE, -r REF, -c, -a/-m, -h, --json, --dry-run", jash_b_touch },
    { "mkdir", 0, "create dirs: -p, -m mode, --json, --dry-run; journaled, argv-native", jash_b_mkdir },
    { "rmdir", 0, "remove empty dirs: --json, --dry-run; journaled, argv-native", jash_b_rmdir },
    { "chmod", 0, "octal/symbolic modes, -R (≤64 journaled), --json, --dry-run; argv-native", jash_b_chmod },
    { "chown", 0, "numeric uid[:gid], --json, --dry-run; journaled, argv-native", jash_b_chown },
    { "ln", 0, "hard/-s links: --json, --dry-run; journaled, argv-native", jash_b_ln },
    { "cat", 0, "concatenate files in-process (budget-capped, binary-safe, no fork); --color=auto|always|never/--highlight C-family source; --json metadata; --data byte-exact paged file content; -n/-b/-s/-E/-T/-A/-v/-- (ADR-0044)", jash_b_cat },
    { "find", 0, "recursive search in-process; --json emits bounded jash.query.find.v1 pages (default maxdepth 16); supported predicates are native, unsupported JSON predicates fail without external fallback", jash_b_find },
    { "glob", 0, "expand one quoted pathname pattern in-process; --json typed file matches, --offset/--limit pages, ** recursive", jash_b_glob },
    { "sed", 0, "stream editor: s/re/repl/[gpiN] with -n/-e/-E/-i; --json reports native -i edits, --dry-run with JSON; legacy external fallback", jash_b_sed },
    { "wc", 0, "count in-process: -l/-w/-c/-m/-L; checked --json array; --json-report pages with line/word/byte totals and per-input errors", jash_b_wc },
    { "stat", 0, "file metadata: --json array or --json-report pages, -L, byte paths and nanoseconds; other forms use external stat", jash_b_stat },
    { "xargs", 0, "build & run command batches from stdin; -n/-I/-0/-t/-r/--; POSIX-quoted input splitting; GOLD: --plan previews the exact batches as JSON {argv} WITHOUT running (footgun-safe), and real runs go through the evaluator so each spawned command is attested/undoable/governed (| xargs rm is recoverable); advanced flags fall back to real xargs (ADR-0044)", jash_b_xargs },
    { "diff", 0, "compare two files in-process (Myers O(ND) engine): normal format, -u/-U N unified, \\ No newline marker, rc 0/1/2; GOLD: --json emits [{op,a:{start,count},b:{start,count},a_lines,b_lines}] hunks (no @@/</> parsing); byte-exact parity vs system diff is NOT the goal (round-trip-correct instead — see ADR-0044 diff design note); every other flag/binary/dir input falls back to real diff (ADR-0044)", jash_b_diff },
    { "awk", 0, "narrow in-process slice: single BEGIN/pattern-action/END program over $0.. $N/NF/NR, -F sep, /regex/ and relational patterns, print/printf, +=/-=/++/--; GOLD: --json streams [{fields:[...]}] per print (or {text:...} per printf), and the END-only aggregation idiom ({s+=$1} END{print s}) emits a single {result:val} object, not an array; control flow, arrays, functions, getline, multiple -v/-f, and every other construct fall back to real awk (ADR-0044)", jash_b_awk },
    { "rm", 0, "remove files: symlink-safe, -i/-I confirm, -d empty-dir, --dry-run, --json per-operand results, trash+undo; quote-aware argv", jash_b_rm },
    { "trash", jash_b_trash, "recycle bin: move/list/restore; empty/gc --json --dry-run preserve recovery pins; on/off/status", jash_b_trash_argv },
    { "tread", 0, "time-travel read: print FILE as it was at a checkpoint", jash_b_tread },
    { "tdiff", 0, "time-travel diff: checkpoint vs working tree", jash_b_tdiff },
    { "grep", 0, "search files (64KB/line cap): -P PCRE-lite, -e PAT, -A/-B/-C, -r --exclude-dir; GOLD: --json match records {file,line,col,match}, --since SEQ (chain-scoped recursive search), mtime-aware cache (real negative caching, no stale hits)", jash_b_grep },  /* ADR-0036 W2: argv_fn */
    { "timeout", 0, "run COMMAND with a wall-clock limit: DURATION supports s/m/h/d + fractional; -s/--signal, -k/--kill-after (SIGKILL escalation), --preserve-status; kills the WHOLE process group (not just the direct child); GOLD: --json {exit_code,timed_out,signal_sent,duration_ms}, --verbose signal notices (ADR-0044)", jash_b_timeout },
    { "cp", 0, "copy files: multi-source, -r/-a, -p preserve, -n no-clobber, -u update, --dry-run, --json per-action results; progress/resume/verify", jash_b_cp },  /* ADR-0036 W2: argv_fn */
    { "mv", 0, "move files: -f/-n/-i/-v, -t/-T target, multi-source, EXDEV fallback, --dry-run, --json per-operand results, atomic (without JSON), undo", jash_b_mv },
{ "ps", jash_b_ps, "list processes: -e/-A/ax all, -ef, -o fields, -C name, -p, --tree/--json/--sort (ADR-0044)", jash_b_ps_argv },
    { "proc", 0, "process snapshots: snapshot|restore|tree|diff (Wave 2)", jash_b_proc },
    { "du", jash_b_du, "disk usage: GNU-parseable SIZE\tPATH, -s/-h/-c/-a/-d N/--max-depth, multi-path, --json (ADR-0044)", jash_b_du_argv },
    { "df", jash_b_df, "filesystem usage: -i/--total, --json JSONL, --json-report pages, --baseline FILE/--delta FILE", jash_b_df_argv },
    { "config", jash_b_jash_config, "manage jash configuration (init, set-key, show)", jash_b_jash_config_argv },
    { "context", jash_b_context, "context compiler for agents: map/fold/bundle/ask (F0/ADR-0027)", jash_b_context_argv },
    { "digest", jash_b_digest, "generate cached project briefing for agents (ADR-0028)", jash_b_digest_argv },
    { "state", jash_b_state, "shell state and checked Git observations; --json, --data, --no-git", jash_b_state_argv },
    { "json", jash_b_json, "structured adapters for legacy CLIs (ADR-0026 Tier 1)", jash_b_json_argv },
    { "speculate", jash_b_speculate, "bounded speculative execution with observed copy, process and cleanup results; --json, --data", jash_b_speculate_argv },
    { "revert", jash_b_revert, "cross-domain undo/revert (ADR-0026 Tier 1)", jash_b_revert_argv },
    { "cost", jash_b_cost, "estimate command cost: bytes, tokens, USD (ADR-0026 Tier 2)", jash_b_cost_argv },
    { "contract", jash_b_contract, "run command and assert output contracts (ADR-0026 Tier 2)", jash_b_contract_argv },
    { "agent-guard", jash_b_agent_guard, "agent anti-bypass control panel (ADR-0026 Tier 3)", jash_b_agent_guard_argv },
    { "adopt", jash_b_adopt, "adoption governor: level, scorecard, metrics (ADR-0040 W2)", jash_b_adopt_argv },
    { "agent-guide", jash_b_agent_guide, "auto-generated agent field guide (ADR-0026 Tier 3)", jash_b_agent_guide_argv },
#ifndef JASH_AGENT_ONLY
    { "brain", jash_b_brain, "Liquid Reasoning Engine: suggest/intent/impact/learn/guard (ADR-0033)", jash_b_brain_argv },
#endif
    { "goal", jash_b_goal, "Intention Memory Caching: begin/step/verify/close/recall (ADR-0033)", jash_b_goal_argv },
    { "llm-invoice", jash_b_llm_invoice, "LLM token economics: begin/close/status/reset (ADR-0033 W3)", jash_b_llm_invoice_argv },
    { "kv", jash_b_kv, "agent key-value store (set/get/del/list) — survives across -c calls", jash_b_kv_argv },
    { "doctor", jash_b_doctor, "structured environment health (disk/tmp/modules/keys)", jash_b_doctor_argv },
    { "recall", jash_b_recall, "agent memory + chain BI (cmds/did/failed/stats/adoption, --json)", jash_b_recall_argv },
    /* ADR-0099: Shell inteligente contextual */
    { "breathe", jash_b_breathe, "stuck agent detection: status/check/kill/timeout (ADR-0099)", jash_b_breathe_argv },
    { "mirror", jash_b_mirror, "verify runtime receipts with --json --receipt REF --blake3 HASH --pubkey KEY; automatic command history recording unavailable", jash_b_mirror_argv },
    { "gossip", jash_b_gossip, "process-local default; --channel NAME for persistent interprocess send/receive with cursors", jash_b_gossip_argv },
    { "sense", jash_b_sense, "alias for `explain --risk` (ADR-0099 W2.3)", jash_b_sense_argv },
    { "forge", jash_b_forge, "unfinished C test scaffold; not implemented or validated code (ADR-0099)", jash_b_forge_argv },
    { "weave", jash_b_weave, "integrate multiple commands with unified output (ADR-0099)", jash_b_weave_argv },
    { "expect", jash_b_expect, "assert a fact about state (--json for structured verdict); ADR-0023 E3", jash_b_expect_argv },
    { "explain", jash_b_explain, "preview a command's blast radius without executing it (ADR-0023 E3b)", jash_b_explain_argv },
    { "mesh", jash_b_mesh, "multi-model swarm substrate: fleet registry + task board, --json (ADR-0024)", jash_b_mesh_argv },
    { "gap", jash_b_gap, "capability-gap radar: report/mine/verify — demand-weighted, proof-closing bugfix queue", jash_b_gap_argv },
    { "dashboard", jash_b_dashboard, "BI: one-view of agent usage — adoption %, activity, fleet, safety (--json)", jash_b_dashboard_argv },
    { "checkpoint", jash_b_checkpoint, "mark state for rewind; list/diff; checked --json; locked, synced TSV snapshot publication with bounded fields", jash_b_checkpoint_argv },
    { "rewind", jash_b_rewind, "roll back mutations to a checkpoint/--session; --dry to preview (ADR-0023 E2)", jash_b_rewind_argv },
    { "verify", jash_b_verify, "blake3 file-integrity checkpoint (verify FILE [HASH])", jash_b_verify_argv },
    { "change", jash_b_change, "sovereign spec-driven change engine: schema/propose/status (ADR-0003 W1)", jash_b_change_argv },
    /* ADR-0034: Agent Self-Improvement Primitives */
    { "mistake", jash_b_mistake, "memory of own errors: log/avoid/list/clear (ADR-0034)", jash_b_mistake_argv },
    { "token", jash_b_token, "token economy visibility: count/optimize/history/budget (ADR-0034)", jash_b_token_argv },
    { "learn", jash_b_learn, "code pattern learning: learn/list/recall/forget (ADR-0034)", jash_b_learn_argv },
    { "predict", jash_b_predict, "failure prediction: predict/risk assess/avoid (ADR-0034)", jash_b_predict_argv },
    { "risk", jash_b_risk, "risk assessment: risk assess/avoid (delegates to predict)", jash_b_risk_argv },
    { "preference", jash_b_preference, "user preference learning: learn/apply/recall/forget (ADR-0034)", jash_b_preference_argv },
    { "reflect", jash_b_reflect, "self-reflection: reflect/improve suggest/apply (ADR-0034)", jash_b_reflect_argv },
    { "selfcheck", jash_b_selfcheck, "binary integrity check: store/verify/force (ADR-0034)", jash_b_selfcheck_argv },
    { "prompt", jash_b_prompt, "prompt optimization engine: optimize/analyze/compress (no LLM required)", jash_b_prompt_argv },
    { "debug", jash_b_debug, "debugging tools: --parse, --locals, --profile", jash_b_debug_argv },
    { "lint", jash_b_lint, "static analysis: lint <file> | -c 'code' | --json <file>", jash_b_lint_argv },
    { "journal", jash_b_journal, "session timeline with causal auto-tags: --tag, --failed, --since, --what-happened", jash_b_journal_argv },
    { "stance", jash_b_stance, "operational modes: list, set <name>, show — see ADR-0035", jash_b_stance_argv },
    { "impact", jash_b_impact, "test impact map learned from the chain: impact [--run] FILE...", jash_b_impact_argv },
    { "handoff", jash_b_handoff, "distill the session into an LLM handoff brief: commands, failures, files, repo state", jash_b_handoff_argv },
    /* Structured pipe filters (JSONL).
     * BUG-056: these MUST NOT shadow coreutils names. Registering `sort`/
     * `uniq` here broke every plain `... | sort | ...` pipeline (JSONL sort
     * demands a FIELD arg → usage error), and builtin `head`/`tail` read
     * only stdin, so `head FILE` hung/emptied instead of reading the file.
     * JSONL semantics live under collision-free names; the POSIX names
     * resolve to the real external tools again. F E R M I ∞ H A R T */
    { "where",    jash_b_where,    "filter JSONL rows: FIELD=VAL, FIELD>NUM, ..." },
    { "pick",     jash_b_pick,     "project fields from JSONL: FIELD1,FIELD2,..." },
    { "sortby",   jash_b_sort,     "sort JSONL by field: [-r] FIELD" },
    { "count",    jash_b_count,    "count JSONL records: [-g GROUP_FIELD]" },
    { "sum",      jash_b_sum,      "sum a numeric FIELD: [-g GROUP_FIELD]" },
    { "avg",      jash_b_avg,      "average a numeric FIELD: [-g GROUP_FIELD]" },
    { "uniqby",   jash_b_uniq,     "deduplicate JSONL by FIELD: [-c] FIELD" },
    { "describe", jash_b_describe, "infer JSONL schema (field names, types, counts)" },
    { "graph", jash_b_graph, "living graph: build/explain/path/god-nodes/communities/hot/stats (ADR-0053)", jash_b_graph_argv },
    { "mem", jash_b_mem, "session memory distilled from the attested chain: distill/timeline/search/brief/verify (ADR-0054)", jash_b_mem_argv },
    { NULL, NULL, NULL }
};

/* Name→slot hash over JASH_BUILTINS (FNV-1a, open addressing) — the linear
 * scan cost ~87 strcmp per lookup at 173 entries, paid 1-3× per command.
 * Built lazily on first lookup; JASH_BUILTINS is const, so a concurrent
 * build from another thread writes identical bytes (benign race). */
#define BUILTIN_HASH_CAP 512   /* pow2, ≥ 2× the entry count */
static uint16_t g_bhash[BUILTIN_HASH_CAP];   /* index+1; 0 = empty */
static int      g_bhash_ready = 0;

static uint32_t builtin_hash_fnv(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

static void builtin_hash_build(void) {
    if (g_bhash_ready) return;
    for (uint32_t i = 0; JASH_BUILTINS[i].name; i++) {
        uint32_t h = builtin_hash_fnv(JASH_BUILTINS[i].name) & (BUILTIN_HASH_CAP - 1);
        while (g_bhash[h]) h = (h + 1) & (BUILTIN_HASH_CAP - 1);
        g_bhash[h] = (uint16_t)(i + 1);
    }
    g_bhash_ready = 1;
}

const jash_builtin_t* jash_builtin_lookup(const char* name) {
    if (!name) return 0;
    if (jash_builtin_is_disabled(name)) return 0;
    builtin_hash_build();
    uint32_t h = builtin_hash_fnv(name) & (BUILTIN_HASH_CAP - 1);
    while (g_bhash[h]) {
        const jash_builtin_t* b = &JASH_BUILTINS[g_bhash[h] - 1];
        if (j_streq(b->name, name)) return b;
        h = (h + 1) & (BUILTIN_HASH_CAP - 1);
    }
#ifndef JASH_AGENT_ONLY
    return jash_plugin_lookup_builtin(name);
#else
    return 0;
#endif
}

int jash_builtin_count(void) {
    static int n = -1;   /* JASH_BUILTINS is const — count once */
    if (n < 0) {
        n = 0;
        while (JASH_BUILTINS[n].name) n++;
    }
    return n;
}

const jash_builtin_t* jash_builtin_at(uint32_t idx) {
    int n = jash_builtin_count();
    if ((int)idx >= n) return 0;
    return &JASH_BUILTINS[idx];
}

/* ADR-0036 W2: single dispatch point — see builtin.h for why this exists. */
int jash_builtin_dispatch(const jash_builtin_t* b, jash_state_t* s,
                          jash_writer_t w, void* ctx, const char* args) {
    if (!b) return 127;
    if (!b->argv_fn) return b->fn(s, w, ctx, args);

    jash_argv_t av;
    jash_argv_scratch_t scratch;
    int n = jash_argv_build_into(args, &av, &scratch);
    if (n < 0) {
        jash_writef(w, ctx, "%s: unterminated quote\n", b->name ? b->name : "?");
        return 2;
    }
    int rc = b->argv_fn(s, w, ctx, av.argc, av.argv);
    jash_argv_free_into(&av, &scratch);
    return rc;
}

/* JINV-046 — contrato em builtin.h. */
int jash_reject_unknown_flag(const char* builtin_name, const char* args,
                             const char* const* conhecidas,
                             jash_writer_t w, void* ctx)
{
    if (!args) return 0;
    while (*args == ' ' || *args == '\t') args++;
    if (args[0] != '-' || args[1] != '-' || args[2] == '\0') return 0;

    /* Recorta o token da flag. */
    char flag[64];
    size_t i = 0;
    while (args[i] && args[i] != ' ' && args[i] != '\t' && i + 1 < sizeof flag) {
        flag[i] = args[i]; i++;
    }
    flag[i] = '\0';

    for (int k = 0; conhecidas && conhecidas[k]; k++)
        if (strcmp(flag, conhecidas[k]) == 0) return 0;

    jash_writef(jash_stderr_writer, 0,
                "jash: %s: opcao desconhecida '%s'\n"
                "jash: nao vou trata-la como nome/argumento — uma flag "
                "engolida vira acao com o nome errado\n",
                builtin_name ? builtin_name : "builtin", flag);
    (void)w; (void)ctx;
    return 1;
}
