#!/usr/bin/env bash
# tests/test_builtin_contract.sh — ADR-0044 W0: the Builtin Contract Ratchet.
#
# Four mechanical checks against the compiled profile and tracked source:
#   1. one denominator — builtins enumerate from JASH_BUILTINS[] alone;
#      the registry carries no duplicate names.
#   2. rot lint — no jash_b_* symbol defined in two .c files (the
#      builtin_ls.c.bak class of collision); no tracked *.bak/*.orig/*.rej.
#   3. BMM bit 11 — every builtin is exercised by some tests/*.sh, or is
#      registered as debt in tests/builtin_gold.txt. The debt list only
#      shrinks: a newly-covered (or vanished) entry left in the list fails.
#   4. anti-orphan — every tests/test_*.sh is reachable from test_suite.sh,
#      or registered as debt in tests/test_debt.txt. Same shrink rule.
#
# Ratchet semantics: adding debt is a gate failure; removing debt is the only
# legal edit. Registered debt is documented, not forgiven.
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"
REG=c/builtins/registry.c
GOLD=tests/builtin_gold.txt
DEBT=tests/test_debt.txt

pass=0; fail=0
ok() { pass=$((pass+1)); echo "  ✓ $1"; }
no() { fail=$((fail+1)); echo "  ✗ $1"; }

# ── 1. one denominator ──────────────────────────────────────────────────
builtin_bin="${JASH_BUILD_BIN:-$PROJECT_ROOT/build/jash}"
builtin_json="$("$builtin_bin" --builtins-json)"
names="$(printf '%s\n' "$builtin_json" | python3 -c '
import json, sys
document = json.load(sys.stdin)
print("\n".join(row["name"] for row in document["builtins"]))
')"
total=$(printf '%s\n' "$names" | sed '/^$/d' | wc -l | tr -d ' ')
uniqn=$(printf '%s\n' "$names" | sed '/^$/d' | sort -u | wc -l | tr -d ' ')
if [[ "$total" -gt 0 ]]; then ok "registry enumerates $total builtins"; else no "registry enumeration came up empty"; fi
if [[ "$total" -eq "$uniqn" ]]; then ok "no duplicate names in registry"; else
    no "duplicate registry names: $(printf '%s\n' "$names" | sort | uniq -d | tr '\n' ' ')"; fi
codemap_n="$(grep -oE '^[0-9]+ builtins, from' CODEMAP.md | grep -oE '^[0-9]+')"
if [[ "$codemap_n" == "$total" ]]; then ok "CODEMAP agrees on the denominator ($codemap_n)"; else
    no "CODEMAP says '$codemap_n' builtins, registry says $total — regenerate CODEMAP"; fi

# ── 2. rot lint ─────────────────────────────────────────────────────────
# Scan the checked-in C source set. Experimental untracked OSIF ports may carry
# mutually exclusive or unfinished builtin drafts and are not build inputs for
# this profile; they must not poison the production-source collision ratchet.
dups="$(git grep -E '^int jash_b_[a-zA-Z0-9_]+\(' -- 'c/*.c' 'c/**/*.c' | sed -E 's/:int (jash_b_[a-zA-Z0-9_]+)\(.*/ \1/' | sort -u | awk '{print $2}' | sort | uniq -d)"
if [[ -z "$dups" ]]; then ok "no jash_b_* symbol defined twice"; else
    no "duplicate symbol definitions: $(echo "$dups" | tr '\n' ' ')"; fi
junk="$(git ls-files '*.bak' '*.orig' '*.rej' 2>/dev/null)"
if [[ -z "$junk" ]]; then ok "no tracked *.bak/*.orig/*.rej"; else
    no "tracked rot: $(echo "$junk" | tr '\n' ' ')"; fi

# ── 3. BMM bit 11: dedicated-test ratchet ───────────────────────────────
[ -f "$GOLD" ] || touch "$GOLD"
uncovered=""
while IFS= read -r n; do
    [[ -z "$n" ]] && continue
    # test_builtin_sweep.sh e excluido de proposito. Ele varre a LARGURA (todos
    # os 170 builtins respondem, nenhum trava) gerando a lista do registry, e
    # menciona cada nome por construcao. Se contasse como cobertura, todo
    # builtin novo nasceria "coberto" e builtin_gold.txt — que rastreia divida
    # de teste SEMANTICO — perderia o sentido no dia em que foi criado.
    grep -rlwF --include='*.sh' --exclude=test_builtin_sweep.sh -- "$n" tests >/dev/null 2>&1 || uncovered="$uncovered$n"$'\n'
done < <(printf '%s\n' "$names" | sort -u)
new_gold="$(comm -23 <(printf '%s' "$uncovered" | sort -u) <(sort -u "$GOLD"))"
stale_gold="$(comm -13 <(printf '%s' "$uncovered" | sort -u) <(sort -u "$GOLD"))"
if [[ -z "$new_gold" ]]; then ok "no builtin lost test coverage (debt: $(sed '/^$/d' "$GOLD" | wc -l | tr -d ' '))"; else
    no "uncovered builtins not in $GOLD: $(echo "$new_gold" | tr '\n' ' ')"; fi
if [[ -z "$stale_gold" ]]; then ok "builtin debt list is tight (ratchet)"; else
    no "stale entries in $GOLD (now covered — remove them): $(echo "$stale_gold" | tr '\n' ' ')"; fi

# ── 4. anti-orphan gate ─────────────────────────────────────────────────
[ -f "$DEBT" ] || touch "$DEBT"
# JINV-003 — o ratchet anti-órfão tem que enxergar TODOS os harnesses.
# O glob era só `tests/test_*.sh`, então tudo que usa a outra convenção de nome
# (`*_test.sh`) ficava invisível: `asan_test.sh` (o ÚNICO harness de sanitizer)
# e `bash_compat_test.sh` (a ÚNICA suíte diferencial contra o bash) podiam
# apodrecer ou sumir sem que nada reprovasse. Benchmarks/profilers ficam de
# fora de propósito — não são testes.
orphans=""
for f in tests/test_*.sh tests/*_test.sh; do
    [ -e "$f" ] || continue
    b="$(basename "$f")"
    [[ "$b" == test_suite.sh ]] && continue
    case "$b" in bench*|benchmark*|profile*) continue;; esac
    grep -qF "$b" tests/test_suite.sh || orphans="$orphans$b"$'\n'
done
new_debt="$(comm -23 <(printf '%s' "$orphans" | sort -u) <(sed '/^[[:space:]]*#/d; /^[[:space:]]*$/d' "$DEBT" | sort -u))"
stale_debt="$(comm -13 <(printf '%s' "$orphans" | sort -u) <(sed '/^[[:space:]]*#/d; /^[[:space:]]*$/d' "$DEBT" | sort -u))"
if [[ -z "$new_debt" ]]; then ok "no new orphan tests (debt: $(sed '/^$/d' "$DEBT" | wc -l | tr -d ' '))"; else
    no "orphan tests not in $DEBT: $(echo "$new_debt" | tr '\n' ' ')"; fi
if [[ -z "$stale_debt" ]]; then ok "orphan debt list is tight (ratchet)"; else
    no "stale entries in $DEBT (now wired/deleted — remove them): $(echo "$stale_debt" | tr '\n' ' ')"; fi

echo "── builtin contract (ADR-0044 W0) ──"
echo "PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ $fail -eq 0 ]]
