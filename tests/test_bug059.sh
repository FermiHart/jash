#!/usr/bin/env bash
# tests/test_bug059.sh — BUG-059: multi-line -c / eval quoting parity vs bash.
#
# The class that drove LLM agents off jash: multi-line commands (what every
# agent harness emits, usually wrapped as `eval '<cmd>'`) were mangled by
# (a) compound parsers demanding ';' where bash accepts newline,
# (b) eval re-parsing raw args instead of doing one-layer POSIX quote removal,
# (c) param expansion eating the backslash of \' (turning it into a live
#     quote delimiter downstream), in eval.c AND builtin_redir.c paths,
# (d) naive one-pair quote strip on assignment values.
# Every case here runs the SAME command in jash and /bin/bash and diffs.
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$PROJECT_ROOT/build/jash-release}"
if ! { [ -f "$JASH" ] && JASH_SIGIL=0 "$JASH" -c 'exit 0' >/dev/null 2>&1; }; then
  JASH="$PROJECT_ROOT/build-speed/jash-release"
fi
if ! { [ -f "$JASH" ] && JASH_SIGIL=0 "$JASH" -c 'exit 0' >/dev/null 2>&1; }; then
  echo "SKIP: no runnable jash binary"; exit 0
fi

NL=$'\n'
pass=0 fail=0
t() { local desc="$1" cmd="$2" jout bout
  # JINV-061: a comparacao abaixo e por IGUALDADE EXATA sobre uma captura com
  # `2>&1`. JASH_NUDGE/JASH_LOOPGUARD faltavam aqui: o scorecard de adoption sai
  # no stderr a cada 10o comando (contador PERSISTENTE entre invocacoes), entra
  # na captura e quebra a paridade por sorte, nao por merito — a mesma doenca
  # que manteve o JINV-019 verde sem nunca ter sido testado.
  jout=$(JASH_AGENT=0 JASH_SIGIL=0 JASH_BRIEF=0 JASH_NUDGE=0 JASH_LOOPGUARD=off \
         JASH_GUARD_BYPASS=0 JASH_GUARD_BYPASS_STRICT=0 "$JASH" -c "$cmd" 2>&1)
  bout=$(/bin/bash -c "$cmd" 2>&1)
  if [ "$jout" = "$bout" ]; then
    echo "✓ $desc"; pass=$((pass+1))
  else
    echo "✗ $desc"
    echo "    jash: $(printf '%s' "$jout" | head -3 | tr '\n' '|')"
    echo "    bash: $(printf '%s' "$bout" | head -3 | tr '\n' '|')"
    fail=$((fail+1))
  fi
}

# newline-as-separator in compounds (bash `linebreak` grammar)
t "for multi-linha sem ;"            "for i in 1 2${NL}do echo \$i${NL}done"
t "while multi-linha sem ;"          "i=0${NL}while [ \$i -lt 2 ]${NL}do i=\$((i+1))${NL}echo \$i${NL}done"
t "if multi-linha sem ;"             "if [ 1 = 1 ]${NL}then echo yes${NL}fi"

# quoted newlines survive the logical-line reader
t "newline em aspas simples"         "echo 'a${NL}b'"
t "newline em aspas duplas"          "echo \"a${NL}b\""
t "string com for (data)"            "s='for i in 1 2${NL}do x${NL}done'${NL}printf '%s\\n' \"\$s\""

# eval: one-layer POSIX quote removal + multiline routing
t "eval script multi-linha"          "eval 'echo a${NL}echo b'"
t "eval com cmdsub e var"            "eval 'x=\$(echo hi)${NL}echo \$x'"
t "eval for multi-linha"             "eval 'for i in 1 2${NL}do echo \$i${NL}done'"
t "eval nested bash quoted"          "eval '/bin/bash -c '\\''v=\$(echo inner)${NL}echo \$v'\\'''"
t "eval nested + redirect (harness)" "eval '/bin/bash -c '\\''for b in x y${NL}do echo got=\$b${NL}done'\\''' < /dev/null"

# backslash-quote preservation through expansion
t "echo backslash-quote"             "echo \\'"
t "quote-in-word"                    "echo don\\'t"
t "assignment com escaped quote"     "v=a\\'b; echo \"\$v\""
t "assignment mixed quotes"          "v=a'b c'; echo \"\$v\""

# nested bash multi-line (no eval)
t "bash aninhado multi-linha"        "/bin/bash -c 'v=\$(echo inner)${NL}echo \$v'"
t "bash aninhado com for"            "/bin/bash -c 'for i in 1 2${NL}do echo \$i${NL}done'"

# BUG-060: ' inside "..." is a literal char — expansion must still run
t "var em squote dentro de dquote"     "G=xyz${NL}echo \"a: '\$G'\""
t "cmdsub em squote dentro de dquote"  "echo \"a: '\$(echo hi)'\""
t "arith em squote dentro de dquote"   "echo \"a: '\$((1+2))'\""
t "squote real ainda suprime"          "echo 'a: \$G \$(echo hi) \$((1+2))'"

echo "═══════════════════════════════════════════════════════════════"
echo "  BUG-059 quoting parity RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[ "$fail" -eq 0 ] && { echo "  ✅ BUG-059 parity PASSED"; exit 0; } || { echo "  ✗ BUG-059 parity FAILED"; exit 1; }
