#!/usr/bin/env bash
# W9 — comandos sobrevivem ao PREAMBULO de um harness real.
#
# O produto e "shell para agentes", e a integracao com o principal harness
# estava capada sem ninguem saber. O Claude Code prefixa CADA comando com:
#
#   source ~/.claude/shell-snapshots/snapshot-bash-*.sh 2>/dev/null || true
#     && { shopt -u extglob || setopt ...; } >/dev/null 2>&1 || true
#     && { \builtin unalias -- '...'; \builtin unset -f -- '...'; } ...
#     && <o comando de verdade>
#
# Esse snapshot e bash puro: define funcoes que sombreiam `grep` e `find` para
# o ugrep/bfs embutidos, mexe em shopt/set -o, e usa `exec -a`. Descobrimos na
# marra, gastando seis chamadas de ferramenta atras de um `grep` "quebrado" que
# na verdade morria em rc 127 no preambulo — ANTES do comando rodar.
#
# Este gate roda comandos atraves de um preambulo com as mesmas construcoes e
# falha se a saida SUMIR. Saida vazia com rc de erro e o pior resultado
# possivel: quem captura ve o vazio e segue.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$ROOT/build/jash}"
runs() { [ -x "$1" ] && "$1" -c 'exit 0' >/dev/null 2>&1; }
runs "$JASH" || { echo "SKIP: nenhum jash executavel"; exit 0; }

ok=0; fail=0
pass() { ok=$((ok+1)); printf '  \033[32mok\033[0m   %s\n' "$1"; }
no()   { fail=$((fail+1)); printf '  \033[31mFAIL\033[0m %s\n       %s\n' "$1" "${2:-}"; }

SB="$(mktemp -d "${TMPDIR:-/tmp}/harness.XXXXXX")"
trap 'rm -rf "$SB"' EXIT
printf 'alpha\nbeta\ngamma\n' > "$SB/f.txt"

# Preambulo com as construcoes reais do snapshot do Claude Code.
cat > "$SB/preamble.sh" <<'PRE'
unalias -a 2>/dev/null || true
shopt -u autocd
shopt -s checkwinsize
shopt -s varredir_close
set -o braceexpand
set -o interactive-comments
if ! (unalias zz 2>/dev/null; command -v zz) >/dev/null 2>&1; then
  function zz {
    local _b="/bin/echo"
    (exec -a zzalias "$_b" ${1+"$@"})
  }
fi
PRE

# JASH_MEMO=0: o contrato (W8) — quem CAPTURA quer os bytes. Sem isso este gate
# mede a memoizacao em vez da integracao.
h() { HOME="$SB" JASH_MEMO=0 JASH_NUDGE=0 JASH_BRIEF=0 "$JASH" \
        -c "source $SB/preamble.sh >/dev/null 2>&1 || true; $1" 2>/dev/null; }

t() {  # t <nome> <comando> <esperado>
    o=$(h "$2"); r=$?
    if [ -z "$o" ] && [ -n "$3" ]; then
        no "$1" "saida VAZIA (rc=$r) — o comando nao chegou a rodar"
    elif [ "$o" = "$3" ]; then pass "$1"
    else no "$1" "got=[$o] want=[$3] rc=$r"; fi
}

t "comando simples apos o preambulo"  "echo ola"                    "ola"
t "grep (o que morria em rc 127)"     "grep -c alpha $SB/f.txt"     "1"
t "wc"                                "wc -l < $SB/f.txt"           "3"
t "pipe"                              "cat $SB/f.txt | grep -c a"   "3"
t "cmdsub"                            "v=\$(wc -l < $SB/f.txt); echo \$v"  "3"
t "rc propagado atraves do preambulo" "false; echo \$?"             "1"

# A funcao definida DENTRO do if do preambulo (BUG-063: o jash executa o corpo
# na definicao em vez de defini-la). Nao falha o gate — esta registrado — mas
# reporta, para o dia em que for corrigido nao passar despercebido.
o=$(h "zz viva" 2>/dev/null)
if [ "$o" = "viva" ]; then pass "funcao definida dentro de if (BUG-063 resolvido!)"
else printf '  \033[33mconhecido\033[0m BUG-063: funcao dentro de if — got=[%s]\n' "$o"; fi

echo
echo "=== integracao com harness: $ok ok / $fail fail ==="
[ "$fail" -eq 0 ]
