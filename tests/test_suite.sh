#!/usr/bin/env bash
# JINV-128: exec before Bash incrementally reads any registrations. Python
# captures this file once and feeds those bytes to Bash's internal exporter.
# CLI: [--list] [--from N --to N] [--timeout SECONDS] [--output NEWDIR].
# --list emits JSONL objects with 1-based index, name, and literal argv.
if [[ ${1-} != --suite-export-v1 ]]; then
    exec python3 "$(dirname -- "${BASH_SOURCE[0]}")/../tools/suite_runner.py" "$@"
    exit 1
fi
set -e
# Only this integration gate may explicitly select a disposable Linux kernel.
# No fallback: native remains the default; unknown values fail before export.
case ${JASH_AUTONOMY_BACKEND:-native} in
    native) autonomy_gate=(bash tests/test_autonomy.sh) ;;
    qemu-tcg) autonomy_gate=(python3 -B tools/run_autonomy_vm.py) ;;
    *) printf 'Unknown JASH_AUTONOMY_BACKEND: %s\n' "$JASH_AUTONOMY_BACKEND" >&2; exit 2 ;;
esac
# Wire format: repeated NUL-terminated field count, label, then argv fields.
# This is trusted repository code, not a shell parser or a sandbox.
run() {
    builtin printf '%s\0' "$#" "$@"
}
registrations() {
run "A-Z regression battery" bash tests/test_az_battery.sh
run "build honesty (JINV-001/002)" bash tests/test_jinv001_build_honesty.sh
run "hot path O(1) (JINV-005/006)" bash tests/test_jinv005_hot_path.sh
run "script size honesty (JINV-007)" bash tests/test_jinv007_script_size.sh
run "redirect restore (JINV-008)" bash tests/test_jinv008_redir_restore.sh
run "glob word splitting (JINV-009)" bash tests/test_jinv009_glob_spaces.sh
run "cmdsub sem deadlock (JINV-010)" bash tests/test_jinv010_cmdsub_size.sh
run "printf/ls sem perda de arg (JINV-011/012)" bash tests/test_jinv011_printf_ls_args.sh
run "modulo verificado antes do dlopen (JINV-013)" bash tests/test_jinv013_module_verify.sh
run "assinatura falha alto (JINV-014)" bash tests/test_jinv014_sign_loud.sh
run "ABI de modulo unica (JINV-015)" bash tests/test_jinv015_abi_single.sh
run "argv[0] citado, sem injecao (JINV-016)" bash tests/test_jinv016_execjson_inject.sh
run "exec-json honra cwd/env/stdin (JINV-017)" bash tests/test_jinv017_execjson_context.sh
run "capabilities-json nao mente (JINV-018)" bash tests/test_jinv018_caps_json.sh
run "palavra de comando quote-aware (JINV-019)" bash tests/test_jinv019_cmdword_quotes.sh
run "separador em aspas aninhadas (JINV-020)" bash tests/test_jinv020_cmdsub_separator.sh
run "segmento sem corte mudo (JINV-021)" bash tests/test_jinv021_pipe_segment.sh
run "corpo de heredoc nao vira comando (JINV-022/023)" bash tests/test_jinv022_heredoc_body.sh
run "prova: integridade != origem (JINV-024)" bash tests/test_jinv024_proof_origin.sh
run "AI module ABI and lazy load" bash tests/test_ai_module.sh
run "portao de caps de modulo nega (JINV-025)" bash tests/test_jinv025_module_caps.sh
run "linha longa nao perde o pipe (JINV-026)" bash tests/test_jinv026_long_line_structure.sh
run "unquote falha alto no estouro (JINV-027)" bash tests/test_jinv027_unquote_overflow.sh
run "redirect de builtin nao some (JINV-028)" bash tests/test_jinv028_builtin_redirect.sh
run "lint: truncar em silencio e proibido (JINV-029)" bash tools/lint-no-silent-truncation.sh
run "lint: todo gate JINV esta registrado nesta suite" bash tools/lint-gate-registration.sh
run "lint: nenhuma assercao infalsificavel nova" bash tools/mutation-check.sh lint
run "o lint de truncamento dispara (JINV-029)" bash tests/test_jinv029_truncation_lint.sh
run "o lint de truncamento ENXERGA o que procura (JINV-091)" bash tests/test_jinv091_lint_sees.sh
run "um teto de linha, um so (JINV-030)" bash tests/test_jinv030_single_line_cap.sh
run "log truncado nao passa como intacto (JINV-031)" bash tests/test_jinv031_chain_truncation.sh
run "LOG retention preserves archives and prompt hysteresis" python3 tests/test_sigil_log_retention.py
run "bracketed paste is literal and paints once" python3 tests/test_bracketed_paste.py
run "familia de separadores bate com bash (JINV-032)" bash tests/test_jinv032_separator_family.sh
run "familia de glob bate com bash (JINV-033/034/035)" bash tests/test_jinv033_glob_family.sh
run "corpo de funcao executa apos fi/esac (JINV-036)" bash tests/test_jinv036_block_tail.sh
run "erro vai para stderr, nao stdout (JINV-038)" bash tests/test_jinv038_error_channel.sh
run "buffers grandes fora da pilha (JINV-039)" bash tests/test_jinv039_recursion_stack.sh
run "til expande em atribuicao (JINV-040)" bash tests/test_jinv040_tilde_assign.sh
run "entrada desconhecida falha alto (JINV-044/045/046)" bash tests/test_jinv046_unknown_flags.sh
run "inventario de builtins com efeito (JINV-047)" bash tests/test_jinv047_builtins_json.sh
run "explain --json (opiniao do shell) (JINV-048)" bash tests/test_jinv048_explain_json.sh
run "checkpoint list --json (JINV-049)" bash tests/test_jinv049_checkpoint_json.sh
run "timeline --json (JINV-050)" bash tests/test_jinv050_timeline_json.sh
run "sigil verify --json declara o que sustenta (JINV-051)" bash tests/test_jinv051_sigil_verify_json.sh
run "sigil newline/ratchet quarantine (JINV-099/100/101)" bash tests/test_jinv099_101_sigil_recovery.sh
run "sigil OCI manifest binding (JINV-102)" bash tests/test_sigil_export_manifest.sh
run "assert: afirmacao amarrada a verificador (JINV-055)" bash tests/test_jinv055_assert.sh
run "why: diagnostico minimo, nao adivinha (JINV-056)" bash tests/test_jinv056_why.sh
run "resume: bilhete que sabe se envelheceu (JINV-057)" bash tests/test_jinv057_resume.sh
run "timeout --diagnose: onde estava quando morreu (JINV-058)" bash tests/test_jinv058_timeout_diagnose.sh
run "JASH_NUDGE=0 silencia TODAS as dicas (JINV-059)" bash tests/test_jinv059_nudge_killswitch.sh
run "fuzz determinístico: nenhuma entrada trava o shell (JINV-060)" bash tests/test_jinv060_fuzz_scanners.sh
run "mutação: gate que não pode falhar é gate ausente (JINV-061)" bash tests/test_jinv061_mutation.sh
run "laço de varredura tem que avançar o ponteiro (JINV-062)" bash tests/test_jinv062_pointer_advance.sh
run "espaço no fim é separador, citado é dado (JINV-063)" bash tests/test_jinv063_trailing_space.sh
run "fechador órfão é palavra, e a volta sempre avança (JINV-064)" bash tests/test_jinv064_split_statements_advance.sh
run "corrida de '*' custa o mesmo que um '*' só (JINV-065)" bash tests/test_jinv065_glob_star_run.sh
run "'!' dentro de [ ] inverte; guarda defensiva não falha aberta (JINV-066)" bash tests/test_jinv066_test_negation.sh
run "flag antes do -c não engole o comando (JINV-067)" bash tests/test_jinv067_invocation_flags.sh
run "stdin roda o mesmo programa que o arquivo (JINV-068)" bash tests/test_jinv068_stdin_multiline.sh
run "'-n' nunca aprova sem ter checado nada (JINV-069)" bash tests/test_jinv069_noexec_refusal.sh
run "comentário acaba na LINHA, não no arquivo (JINV-070)" bash tests/test_jinv070_block_comment.sh
run "'\\' no fim junta a linha seguinte, também em bloco (JINV-071)" bash tests/test_jinv071_block_continuation.sh
run "palavra-chave é PALAVRA; ';' citado não separa (JINV-072)" bash tests/test_jinv072_keyword_is_word.sh
run "254/255 são status, não instrução de fluxo (JINV-073)" bash tests/test_jinv073_exit255_not_break.sh
run "a cauda depois do 'fi' executa UMA vez (JINV-074)" bash tests/test_jinv074_if_tail_once.sh
run "cauda depois de done/esac respeita break (JINV-075)" bash tests/test_jinv075_loop_tail_break.sh
run "printf sem teto de argumentos; erro no stderr (JINV-076)" bash tests/test_jinv076_printf_no_arg_ceiling.sh
run "echo monta a saída do ARGV, não da linha crua (JINV-079)" bash tests/test_jinv079_echo_argv.sh
run "'export NOME' sem valor é POSIX, não erro (JINV-077)" bash tests/test_jinv077_export_bare.sh
run "'for VAR;' sem 'in' percorre os posicionais (JINV-078)" bash tests/test_jinv078_for_positional.sh
run "corpo de laço sem teto mudo de 1 KB (JINV-080)" bash tests/test_jinv080_loop_body_size.sh
run "acumulador de composto sem teto de 4 KB (JINV-081)" bash tests/test_jinv081_compound_accumulator.sh
run "corpo de while/until/case no heap, sem teto (JINV-082)" bash tests/test_jinv082_loop_body_heap.sh
run "palavra-chave de laço: PALAVRA, fora de aspas, em posição (JINV-083)" bash tests/test_jinv083_keyword_in_quotes.sh
run "'{ … }' é UM comando composto, não dois comandos (JINV-084)" bash tests/test_jinv084_brace_group.sh
run "padrão de case é PALAVRA; 'esac' do filho não é do pai (JINV-085)" bash tests/test_jinv085_case_patterns.sh
run "corpo de função e valor de variável sem teto MUDO (JINV-086)" bash tests/test_jinv086_silent_caps.sh
run "IFS divide expansão NÃO CITADA; literal e citado não (JINV-087)" bash tests/test_jinv087_ifs_quoted.sh
run "cmdsub: corpo de comando nao trunca (JINV-088)" bash tests/test_jinv088_cmdsub_body.sh
run "o cifrao que NAO abre expansao e DADO (JINV-089)" bash tests/test_jinv089_dollar_as_data.sh
run "o VALOR expandido nao volta a ser sintaxe (JINV-090)" bash tests/test_jinv090_eval_no_reexpand.sh
run "delimitador de heredoc: a barra invertida CITA (JINV-092)" bash tests/test_jinv092_heredoc_delim.sh
run "operador CITADO (\\| \\> \\&) sobrevive à expansão (JINV-093)" bash tests/test_jinv093_escaped_operator.sh
run "operador no FIM da linha pede a linha seguinte (JINV-094)" bash tests/test_jinv094_operator_continuation.sh
run "o SEGUNDO heredoc (e o segundo < arquivo) chega (JINV-095)" bash tests/test_jinv095_multiple_heredocs.sh
run "nome de função é PALAVRA: kebab-case no boot (JINV-096)" bash tests/test_jinv096_function_word_names.sh
run "CODEMAP freshness check" bash tests/test_codemap.sh
run "Agent-experience wave suite" bash tests/test_agent_wave.sh
run "Conformance vs system tools (ADR-0036 W1)" bash tests/conformance/run_conformance.sh
run "argv migration ratchet (ADR-0036 W2)" bash tests/test_argv_ratchet.sh
run "Beyond port regression guards" bash tests/test_beyond_port_regressions.sh
run "agent-guide 100% surface coverage (ADR-0037)" bash tests/test_agent_guide.sh
run "BUG-059 multi-line/eval quoting parity vs bash" bash tests/test_bug059.sh
run "read memoization (ADR-0038 W1)" bash tests/test_memo.sh
run "files_changed report (ADR-0038 W2)" bash tests/test_fschange.sh
run "stale-write guard (ADR-0038 W3)" bash tests/test_staleguard.sh
run "error folding (ADR-0038 W4)" bash tests/test_errfold.sh
run "test impact map (ADR-0038 W5)" bash tests/test_impact.sh
run "serve exec daemon (ADR-0038 W6)" bash tests/test_serve.sh
run "jash-agentd agent-strict source profile" bash tests/test_jash_agentd_strict.sh
run "agent runtime protocol v2 streaming and binary CAS" bash tests/test_agent_protocol_v2.sh
run "agent runtime process supervisor and execution IR" bash -c \
    'cmake --build build --target test_agent_process_ir >/dev/null && build/test_agent_process_ir'
run "agent runtime jobs and PTY protocol" bash tests/test_agent_jobs_protocol.sh
run "interactive history and O(n) autosuggest" bash tests/test_history_autosuggest.sh
run "idle Sigil Defender PTY integration" bash tests/test_sigil_defender.sh
run "PATH cache incremental persistence" bash tests/test_path_cache_io.sh
run "array aggregate auxiliary index" bash tests/test_array_index.sh
run "world delta linear diff and bounded load" bash tests/test_world_linear.sh
run "shared world and fschange startup snapshot" bash tests/test_shared_snapshot.sh
run "independent job receipt verification" bash tests/test_libjef_job_receipt.sh
run "independent job specification verification" bash tests/test_libjef_job_spec.sh
run "libjef bounded parsing and sanitized Ed25519" env JEF_SANITIZE=1 JEF_ADJACENT=1 python3 -B tests/test_libjef_input_bounds.py
run "experimental verifiable autonomy (Linux integration)" "${autonomy_gate[@]}"
run "static musl agent dependency closure" bash tests/test_static_agent_runtime.sh
run "agent structured read fast path (JINV-135)" bash tests/test_jinv135_agent_read_fastpath.sh
run "agent durable completion append (JINV-136)" bash tests/test_jinv136_agent_durable_append.sh
run "persistent sealed jef verifier (JINV-137)" bash tests/test_jinv137_jef_persistent.sh
run "Git identity history uses current contact" bash tests/test_identity_history.sh
run "provenance firewall (ADR-0038 W7)" bash tests/test_provenance.sh
run "session handoff compiler (ADR-0038 W8)" bash tests/test_handoff.sh
run "process substitution releases consumed descriptors" bash tests/test_procsub_resources.sh
run "jash vs bash ten-wave differential hardening" python3 -B tests/test_jash_vs_bash_waves.py
run "startup positionals, EXIT traps and heredoc paths" python3 -B tests/test_startup_waves.py
run "nested redirection descriptors and output routing" python3 -B tests/test_redir_waves.py
run "performance waves: allocation, syscall and semantic guards" bash tests/test_perf_waves.sh
run "functional battery (deep feature coverage)" bash tests/test_functional_battery.sh
run "CI gate matrix: -Werror + bench thresholds (ADR-0036 W6)" bash tests/test_gates.sh
run "adoption gate: parity sweep + A/B harness (ADR-0040 W7)" bash tests/ci_adoption_gate.sh
run "world-delta between -c calls (ADR-0042 F2)" bash tests/test_world_delta.sh
run "tread/tdiff time-travel reads (ADR-0042 F8)" bash tests/test_tread.sh
run "loopguard: repeated-failure breaker (ADR-0042 F3)" bash tests/test_loopguard.sh
run "memoΔ structural output diff (ADR-0042 F1)" bash tests/test_memo_delta.sh
run "mesh claim: file intent-locks (ADR-0042 F9)" bash tests/test_mesh_claim.sh
run "bg jobs: cross-c async (ADR-0042 F6)" bash tests/test_bg_jobs.sh
run "salient output governor (ADR-0042 F4)" bash tests/test_salience.sh
run "blame-file: attested authorship (ADR-0042 F7)" bash tests/test_blamefile.sh
run "distill/recipe: session→recipe (ADR-0042 F10)" bash tests/test_distill.sh
run "prefetch: anticipatory shell (ADR-0042 F5)" bash tests/test_prefetch.sh
run "drift: env-drift forensics (ADR-0043 W1)" bash tests/test_drift.sh
run "builtin contract ratchet (ADR-0044 W0)" bash tests/test_builtin_contract.sh
run "gold contract: shared gold-builtin capabilities (ADR-0044 A0)" bash tests/test_gold_contract.sh
run "ls --delta: session-aware change listing (ADR-0044 A1)" bash tests/test_ls_delta.sh
run "rm safety: symlink lstat, -i/-I, -d, -- (ADR-0044 Arc A)" bash tests/test_rm_safety.sh
run "security audit confirmed findings" bash tests/test_security_audit_fixes.sh
run "security: cp/mv metadata and kill targets" bash tests/test_jinv_security_cp_mv_kill.sh
run "incomplete audit: behavioral regressions and false-positive evidence" python3 tests/test_incomplete_audit.py
run "native parser: lexer bounds and AST extraction" bash tests/test_parse.sh
run "agent contract: inventory, shared validation and JSON writer" python3 tests/test_agent_contract_inventory.py
run "agent JSON: exact envelopes, byte paths and delivery failures" python3 tests/test_agent_json_roundtrip.py
run "native discovery data: glob, find and file pages" python3 tests/test_discovery_native_data.py
run "shell state: structured reports reflect state and effects" python3 tests/test_shell_structured.py
run "shell state: native kinds across both execution profiles" python3 tests/test_shell_native_profiles.py
run "repository identity: module and core evidence scope agree" python3 tests/test_repo_identity.py
run "disk usage: native observations and cache coherence" python3 tests/test_du_native_data.py
run "grep: native byte spans and checked observations" python3 tests/test_grep_native_data.py
run "grep: stream, context, predicate and file-level native outcomes" python3 tests/test_grep_semantic_data.py
run "adapters: explicit JSON/JTV2 and exact execution plans" python3 tests/test_structured_adapters.py
run "df: native durable baseline receipts" python3 tests/test_df_baseline_data.py
run "SDK: v2 bytes and scoped native results" python3 tests/test_structured_sdk.py
run "trash: native reversible effects and metadata" python3 tests/test_trash_native.py
run "mesh: native coordination and checked stores" python3 tests/test_mesh_native_data.py
run "learning: persisted native observations and actions" python3 tests/test_structured_learning.py
run "recovery: native domain inverses and binary integrity" python3 tests/test_recovery_extra_native.py
run "memory: native kv intent and resume" python3 tests/test_memory_native.py
run "recall: native history and overflow pages" python3 tests/test_recall_native.py
run "contract: bounded child output and assertions" python3 tests/test_contract_native.py
run "graph: native observations and publication" python3 tests/test_graph_native.py
run "context: native source and recorded observations" python3 tests/test_context_native.py
run "change: native plans, stores and observed effects" python3 tests/test_change_native.py
run "impact: native observations and executed outcomes" python3 tests/test_impact_native.py
run "expect: native predicates, retries and execution receipts" python3 tests/test_expect_native.py
run "assert: native evidence and verifier receipts" python3 tests/test_assert_native.py
run "ls: advanced queries and checked delta publication" python3 tests/test_ls_extended.py
run "debug: native parsing, locals and profiling" python3 tests/test_debug_native.py
run "sigil: observed recovery and ratchet transitions" python3 tests/test_sigil_transitions_native.py
run "recipes: distill and checked step execution" python3 tests/test_structured_recipes.py
run "analysis: lint, explain and sense native outcomes" python3 tests/test_analysis_native.py
run "digest: native inventory, verified content and publication" python3 tests/test_digest_native.py
run "mem: native engine, stores and publication receipts" python3 tests/test_mem_native.py
run "gap: native verification and ordered effect receipts" python3 tests/test_structured_gap.py
run "sigil: native queries and chain observations" python3 tests/test_sigil_queries_native.py
run "data: presentation budget isolation and physical limits" python3 tests/test_structured_data_budget.py
run "sigil: native actions and independent proof verification" python3 tests/test_sigil_actions_native.py
run "handoff: native session and repository observations" python3 tests/test_handoff_native.py
run "dashboard: native observations and unavailable sources" python3 tests/test_structured_dashboard.py
run "filesystem: schemas and exact values in both profiles" python3 tests/test_fs_contract_schemas.py
run "history: native why blame and journal evidence" python3 tests/test_history_native.py
run "guidance: native guide and observed command cost" python3 tests/test_structured_guidance.py
run "policies: native state and persistence failures" python3 tests/test_structured_models.py
run "coordination: native channels and execution receipts" python3 tests/test_coordination_data.py
run "proc: native snapshot observations and restore scope" python3 tests/test_proc_snapshot_data.py
run "serve: exact delivery and failure boundaries" python3 tests/test_serve_delivery.py
run "CLI adapters: native byte-safe values" python3 tests/test_structured_cli_adapters.py
run "awk: native semantic evaluation" python3 tests/test_awk_native_data.py
run "process control: native receipts and admission" python3 tests/test_process_control_data.py
run "recovery: native actions and actual state" python3 tests/test_recovery_native.py
run "process: native OSIF observations" python3 tests/test_process_native_data.py
run "shell: response admission precedes effects" python3 tests/test_shell_admission.py
run "management: response admission and partial delivery" python3 tests/test_management_admission.py
run "calc: exact rational and binary64 results" python3 tests/test_structured_calc.py
run "diff: native hunks and file consistency" python3 tests/test_diff_native_data.py
run "tread: native snapshot pages" python3 tests/test_tread_native_data.py
run "management: native alias, registry and persistence reports" python3 tests/test_management_builtins.py
run "mutations: native receipts and admission before effects" python3 tests/test_agent_mutations_data.py
run "query cancellation: terminal after the final IO" python3 tests/test_query_native_cancel.py
run "discovery builtins: native records, bytes, resolution and sinks" python3 tests/test_discovery_builtins.py
run "native query data: stat, wc and df semantic parity" python3 tests/test_query_native_data.py
run "builtin modes: scoped contracts and independent schemas" python3 tests/test_builtin_mode_catalog.py
run "serve capture: UTF-8, NUL and arbitrary bytes" python3 tests/test_serve_capture_bytes.py
run "builtin JSON: exact values, budgets and sink failures" python3 tests/test_builtin_json_integrity.py
run "typed operators: strict JSONL, exact integers and JTV2" python3 tests/test_structured_pipe.py
run "ls argv: literal operands across profiles" python3 tests/test_ls_argv_boundaries.py
run "df errors: OSIF failure remains structured" python3 tests/test_df_osif_errors.py
run "agent mutations: native reports, preview and sink boundaries" python3 tests/test_agent_mutations_json.py
run "agent recovery: guarded durable refs, conflicts and partial restore" python3 tests/test_agent_recovery_refs.py
run "agent queries: exact df quantities and live pages" python3 tests/test_agent_queries_json.py
run "agent state: exact timeline identities and pages" python3 tests/test_agent_state_evidence.py
run "agent matrix: profile denominators and observed effects" python3 tests/test_agent_contract_matrix.py
run "agent local sessions: peer scope and client isolation" python3 tests/test_agent_local_sessions.py
run "agent storage: protected pins and consumed-only GC" python3 tests/test_agent_storage_recovery.py
run "agent atomic mutations: commit, rollback and retained recovery" python3 tests/test_agent_atomic_mutations.py
run "agent builtin cancellation: control, effects, receipts and session reuse" python3 tests/test_agent_builtin_cancel.py
run "agent local cancellation: private endpoints, isolation and reconnect" python3 tests/test_agent_local_cancel.py
run "agent job disconnect: descendants, archived receipts and recovery" python3 tests/test_agent_job_disconnect.py
run "agent listener shutdown: drain, signals, parent loss and CAS publication" python3 tests/test_agent_listener_shutdown.py
run "agent artifact store: roots, leases, quota and recovery-aware GC" python3 tests/test_agent_artifact_store.py
run "agent maintenance: signed intent, replay and explicit retention release" python3 tests/test_agent_maintenance_transactions.py
run "W09 agent budget: managed scope and unknown measurements" python3 tests/test_agent_data_plane.py Budget
run "W10 data policy: typed revisions and publication" python3 tests/test_agent_data_plane.py Config
run "W11 data OSIF: probes and measured baseline" python3 tests/test_agent_data_plane.py Transport
run "W12 data descriptors: private framing and ownership" python3 tests/test_agent_data_plane.py FD
run "W13 sealed buffers: byte ranges, quotas and lifetime" python3 tests/test_agent_data_plane.py Buffers
run "W14 artifact handoff: durable replay and verification" python3 tests/test_agent_data_plane.py Artifacts
run "W15 binary streams: credits, fan-out, half-close and backpressure" python3 tests/test_agent_binary_streams.py
run "W16 native artifact pipeline: immutable refs to external stdin" env PYTHONDONTWRITEBYTECODE=1 python3 tests/test_agent_artifact_pipeline.py
run "security: mutator commit and cleanup" bash tests/test_jinv_security_mutators.sh
run "security: key, snapshot, and heredoc state" bash tests/test_jinv_security_state.sh
run "builtin output governor: cap+spill, pipe-safe (ADR-0044)" bash tests/test_builtin_governor.sh
run "sed -i undoable: destructive edit + undo (ADR-0044)" bash tests/test_sed_undo.sh
run "stat --json: portable file metadata (ADR-0044)" bash tests/test_stat_json.sh
run "xargs: batch-build, --plan preview, attested exec, -0 rm (ADR-0044)" bash tests/test_xargs.sh
run "diff: Myers O(ND) engine, round-trip correctness, --json hunks (ADR-0044)" bash tests/test_diff.sh
run "awk: narrow in-process slice, --json aggregation gold (ADR-0044)" bash tests/test_awk.sh
run "grep gold: parity fixes, --json, --since (ADR-0044 Arc A2)" bash tests/test_grep_gold.sh
run "timeout: wall-clock limit, process-group kill, --json (ADR-0044)" bash tests/test_timeout.sh
run "weave: flow integration, envelope, cap (ADR-0099 W6.1)" bash tests/test_weave.sh
run "breathe: stuck detection, baseline, kill rails (ADR-0099 W6.2)" bash tests/test_breathe.sh
run "rewind --scope: revert sob diretorio, keep fora (PATCH-SPEC)" bash tests/test_rewind_scope.sh
run "undo waves consolidado (ADR-0050 W18)" bash tests/test_undo_waves.sh
run "recall waves consolidado (ADR-0051 W9)" bash tests/test_recall_waves.sh
run "expect waves consolidado (ADR-0052)" bash tests/test_expect_waves.sh
run "risk-gated auto-checkpoint (ADR-0050 W12/JINV-097)" bash tests/test_jinv097_risk_checkpoint.sh
run "gap: capability-gap radar" bash tests/test_gap.sh
run "living graph (ADR-0053)" bash tests/test_graph.sh
run "mem: session memory from the attested chain (ADR-0054)" bash tests/test_mem.sh
run "bug061: heredoc quoted cmdsub" bash tests/test_bug061_heredoc_quoted_cmdsub.sh
run "jinv096: function word names" bash tests/test_jinv096_function_word_names.sh
run "jinv096: value not syntax" bash tests/test_jinv096_value_not_syntax.sh
run "speculate scope" bash tests/test_speculate_scope.sh
run "mesh fleet (ADR-0099 W5)" bash tests/test_mesh_fleet.sh
run "edge builtins: chown/forge/gossip coverage" bash tests/test_edge_builtins.sh
run "explain --risk (ADR-0099 W6.3)" bash tests/test_explain_risk.sh
run "jinv104: grep --include union" bash tests/test_jinv104_grep_include_union.sh
run "jinv105: BRE/ERE grammar flip" bash tests/test_jinv105_bre_ere_flip.sh
run "grep grammar differential fuzzer" bash tests/fuzz_grep_grammar_diff.sh
run "jinv106: sigil boundary trust" bash tests/test_jinv106_sigil_boundary_trust.sh
run "jinv111: chmod symbolic modes" bash tests/test_jinv111_chmod_symbolic.sh
run "jinv114: slash is pathname" bash tests/test_jinv114_slash_is_pathname.sh
run "jinv116/117: quote-aware scanners" bash tests/test_jinv116_quote_aware_scanners.sh
run "jinv121: memo identity" bash tests/test_jinv121_memo_identity.sh
run "jinv122: sigil scan checkpoint" bash tests/test_jinv122_scan_checkpoint.sh
run "jinv123: heredoc in quotes" bash tests/test_jinv123_heredoc_in_quotes.sh
run "jinv110: compound pipeline split" bash tests/test_jinv110_compound_pipeline.sh
run "jinv124: arith in pipeline" bash tests/test_jinv124_arith_in_pipeline.sh
run "jinv126: in-session env mirror" bash tests/test_jinv126_env_mirror.sh
run "jinv127: grep clustered flags" bash tests/test_jinv127_grep_cluster.sh
run "jinv128: suite pinned mid-run" bash tests/test_jinv128_suite_pinned.sh
run "master suite runner isolation and evidence" python3 -B tests/test_suite_runner.py
run "jinv131: argv ceiling (wide glob)" bash tests/test_jinv131_argv_ceiling.sh
run "jinv132: glob ceiling (count + path length)" bash tests/test_jinv132_glob_ceiling.sh
run "jinv133: vendored build revision" bash tests/test_jinv133_vendor_revision.sh
run "jinv134: reproducible build timestamp" bash tests/test_jinv134_reproducible_timestamp.sh
run "builtin sweep: os 170 respondem, nenhum trava" bash tests/test_builtin_sweep.sh
run "jinv112: sigil chain verify passa num ledger real" bash tests/test_jinv112_chain_verify.sh
run "W7: nenhum binario rastreado no git" bash tests/test_no_tracked_binaries.sh
run "W6: rotulo-como-prova (diff)" bash tools/lint-label-vs-fact.sh
run "W8: contrato de saida do memo" bash tests/test_memo_contract.sh
run "W9: comandos sobrevivem ao preambulo de um harness" bash tests/test_harness_integration.sh

# ── W5 — 39 gates que existiam e nao rodavam ────────────────────────────────
# 53 arquivos de teste estavam fora da suite. Um teste que nao roda e pior que
# um teste ausente: consome manutencao e produz sensacao de cobertura. Estes 39
# passam hoje; os 14 que falham ficaram em tests/test_debt.txt com data e dono.
run "orfao: ab adoption" bash tests/test_ab_adoption.sh
run "orfao: agent guard" bash tests/test_agent_guard.sh
run "orfao: agent guard demo" bash tests/test_agent_guard_demo.sh
run "orfao: bash parity" bash tests/test_bash_parity.sh
run "orfao: brain goal" bash tests/test_brain_goal.sh
run "orfao: budget builtin" bash tests/test_budget_builtin.sh
run "orfao: bypass nudge" bash tests/test_bypass_nudge.sh
run "orfao: change wave1" bash tests/test_change_wave1.sh
run "orfao: change wave2" bash tests/test_change_wave2.sh
run "orfao: change wave3" bash tests/test_change_wave3.sh
run "orfao: change wave4" bash tests/test_change_wave4.sh
run "orfao: change wave5" bash tests/test_change_wave5.sh
run "orfao: change wave6" bash tests/test_change_wave6.sh
run "orfao: checkpoint" bash tests/test_checkpoint.sh
run "orfao: cmdsub assign" bash tests/test_cmdsub_assign.sh
run "orfao: continuation" bash tests/test_continuation.sh
run "orfao: cost contract" bash tests/test_cost_contract.sh
run "orfao: exec json" bash tests/test_exec_json.sh
run "orfao: expect" bash tests/test_expect.sh
run "orfao: explain" bash tests/test_explain.sh
run "orfao: governor" bash tests/test_governor.sh
run "orfao: herestring" bash tests/test_herestring.sh
run "orfao: json adapter" bash tests/test_json_adapter.sh
run "orfao: json builtins" bash tests/test_json_builtins.sh
run "orfao: libjef" bash tests/test_libjef.sh
run "orfao: libjef verify" bash tests/test_libjef_verify.sh
run "orfao: llm invoice" bash tests/test_llm_invoice.sh
run "orfao: ls cache" bash tests/test_ls_cache.sh
run "orfao: ls json" bash tests/test_ls_json.sh
run "orfao: mesh" bash tests/test_mesh.sh
run "orfao: nudge" bash tests/test_nudge.sh
run "orfao: quote escape" bash tests/test_quote_escape.sh
run "orfao: quoted cmd" bash tests/test_quoted_cmd.sh
run "orfao: recall" bash tests/test_recall.sh
run "orfao: revert" bash tests/test_revert.sh
run "orfao: sigil" bash tests/test_sigil.sh
run "orfao: speculate" bash tests/test_speculate.sh
run "orfao: state" bash tests/test_state.sh
run "orfao: trash toggle" bash tests/test_trash_toggle.sh
run "unit: append backup failure paths" bash tests/test_append_backup_unit.sh
run "unit: inert word layer" bash tests/test_inert_word_layer.sh
run "unit: environment and filesystem change cache" bash tests/test_perf_env_fschange.sh
run "unit: regular-file read failures" bash tests/test_read_regular_unit.sh
run "unit: SHA-256 backend parity" bash tests/test_sha256_backends.sh
run "AI native read-only list/count observations" python3 -B tests/test_ai_typed_observation.py
run "AI AUTO bounded execution and scoped rewind" python3 -B tests/test_ai_auto_session.py
run "AI provider usage and latency cells" python3 -B tests/test_ai_provider_usage.py
run "AI observation authorized durable promotion" python3 -B tests/test_ai_ref_promotion.py
run "Python scoped local SDK and capability denial" python3 -B tests/test_python_local_client.py
run "native spinner builtin state transitions" bash tests/test_spinner_builtin.sh
run "AI tool adapters for read-only versioned batches" python3 -B tests/test_ai_tool_adapters.py
run "MCP stdio read-only native tool adapter" python3 -B tests/test_jash_mcp.py
run "mandatory warning-free CMake and module builds" bash tests/test_werror_required.sh
run "native bounded source page and binary bytes" bash -c 'cmake --build build --target test_ai_source_page >/dev/null && build/test_ai_source_page'
run "native bounded project digest and generated build exclusion" bash -c 'DIGEST_BINARY="$PWD/build/jash" DIGEST_UNIT_BINARY="$PWD/build/test_digest_engine_unit" python3 -B tests/test_digest_project.py'
run "process observation: checked namespace, identity and descendant source" python3 tests/test_proc_observation.py
run "state: exact local facts and checked Git observations" python3 tests/test_state_native.py
run "speculate: bounded copied workspace and actual execution receipts" python3 tests/test_speculate_native.py
run "context ask: lexical retrieval compatibility" bash tests/test_context_ask.sh
run "supervisor: checked descendant discovery and permanent cleanup errors" bash -c 'cmake --build build --target test_agent_process_children >/dev/null && build/test_agent_process_children'
run "osif: probes, reports, matrix and port scaffolds" python3 tests/test_osif_native.py

}
registrations
