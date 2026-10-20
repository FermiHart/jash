#!/usr/bin/env bash
# W8 — o contrato de saida do memo (Docs/CONTRATO-MEMO.md).
#
# O memo substitui os bytes por uma linha atestada quando a leitura se repete.
# E o diferencial para um agente que LE — e uma resposta errada para quem
# CAPTURA. Nao ha sinal automatico que separe os dois, entao a fronteira e
# declarada (JASH_MEMO=0) e travada aqui.
#
# Medido em 2026-08-05: um `grep -c` legitimo devolveu saida VAZIA com rc de
# erro atraves do Bash tool do Claude Code, e seis chamadas foram gastas
# investigando um grep "quebrado" que na verdade nunca rodou.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$ROOT/build/jash}"
runs() { [ -x "$1" ] && "$1" -c 'exit 0' >/dev/null 2>&1; }
runs "$JASH" || { echo "SKIP: nenhum jash executavel"; exit 0; }

ok=0; fail=0
pass() { ok=$((ok+1)); printf '  \033[32mok\033[0m   %s\n' "$1"; }
no()   { fail=$((fail+1)); printf '  \033[31mFAIL\033[0m %s\n       %s\n' "$1" "${2:-}"; }

SB="$(mktemp -d "${TMPDIR:-/tmp}/memoc.XXXXXX")"
trap 'rm -rf "$SB"' EXIT
printf 'a\nb\nc\n' > "$SB/f.txt"
run() { HOME="$SB" JASH_NUDGE=0 JASH_BRIEF=0 JASH_AGENT=1 "$JASH" -c "$1" 2>/dev/null; }

# ── 1. JASH_MEMO=0 SEMPRE devolve os bytes, quantas vezes rodar ─────────────
# Esta e a garantia de que a fronteira declarada funciona. Se ela quebrar,
# nenhuma integracao com harness tem saida.
a=$(HOME="$SB" JASH_MEMO=0 JASH_NUDGE=0 JASH_BRIEF=0 JASH_AGENT=1 "$JASH" -c "wc -l < $SB/f.txt" 2>/dev/null)
b=$(HOME="$SB" JASH_MEMO=0 JASH_NUDGE=0 JASH_BRIEF=0 JASH_AGENT=1 "$JASH" -c "wc -l < $SB/f.txt" 2>/dev/null)
c=$(HOME="$SB" JASH_MEMO=0 JASH_NUDGE=0 JASH_BRIEF=0 JASH_AGENT=1 "$JASH" -c "wc -l < $SB/f.txt" 2>/dev/null)
if [ "$a" = "$b" ] && [ "$b" = "$c" ] && [ -n "$a" ]; then
    pass "JASH_MEMO=0: os bytes, em toda repeticao (3x '$a')"
else
    no "JASH_MEMO=0: os bytes em toda repeticao" "1='$a' 2='$b' 3='$c'"
fi

# ── 2. o escape hatch nunca some do banner ──────────────────────────────────
# Quem tropeca no memo precisa achar a saida NA PROPRIA MENSAGEM. Sem isso, a
# descoberta custa o que custou em 2026-08-05: seis chamadas de ferramenta.
run "cat $SB/f.txt" >/dev/null
out=$(run "cat $SB/f.txt")
case "$out" in
  *"jash memo"*)
    case "$out" in
      *JASH_MEMO=0*) pass "o banner do memo nomeia JASH_MEMO=0" ;;
      *) no "o banner do memo nomeia JASH_MEMO=0" "banner sem escape hatch: $out" ;;
    esac ;;
  *) pass "sem memo neste ambiente (banner nao se aplica)" ;;
esac

# ── 3. substituicao de comando aninhada NUNCA recebe a linha do memo ────────
# memo_is_top() so memoiza o writer de topo. Se isso regredir, todo
# `x=$(comando)` dentro de um script jash passa a receber texto descritivo no
# lugar dos dados — e o script continua, com o valor errado.
run "v=\$(wc -l < $SB/f.txt); echo \$v" >/dev/null
nested=$(run "v=\$(wc -l < $SB/f.txt); echo \$v")
case "$nested" in
  *"jash memo"*) no "cmdsub aninhado nao recebe banner" "recebeu: $nested" ;;
  "") no "cmdsub aninhado nao recebe banner" "veio VAZIO — pior que errado" ;;
  *) pass "cmdsub aninhado recebe os bytes ('$nested')" ;;
esac

# ── 4. BUG-067: com JASH_MEMO=0 a captura recebe os BYTES, sempre ───────────
# O caso do campo: `grep -c PADRAO arquivo` devolveu a linha do memo em vez do
# numero, 30 de 30 vezes, e o consumidor concluiu "nao existe" com confianca.
# O relato virou uma tabela errada de quais scripts tinham um recurso.
#
# A fronteira e DECLARADA, nao adivinhada: quem captura poe JASH_MEMO=0. Tentei
# inferir por isatty() — "ninguem le um pipe" — e a regra e verdadeira, mas
# desligou o memo em 9 gates que o exercitam com a saida capturada por
# construcao. O lugar de aplicar a regra e o ambiente da integracao.
printf 'x\ny\nz\n' > "$SB/n.txt"
cap1=$(HOME="$SB" JASH_MEMO=0 JASH_NUDGE=0 JASH_BRIEF=0 JASH_AGENT=1 "$JASH" -c "grep -c x $SB/n.txt" 2>/dev/null)
cap2=$(HOME="$SB" JASH_MEMO=0 JASH_NUDGE=0 JASH_BRIEF=0 JASH_AGENT=1 "$JASH" -c "grep -c x $SB/n.txt" 2>/dev/null)
case "$cap1$cap2" in
  *"jash memo"*) no "JASH_MEMO=0: captura recebe os BYTES" "recebeu o banner: [$cap2]" ;;
  "11")          pass "JASH_MEMO=0: captura recebe os BYTES nas duas vezes" ;;
  *)             no "JASH_MEMO=0: captura recebe os BYTES" "esperado '1' 2x, veio [$cap1][$cap2]" ;;
esac

echo
echo "=== contrato do memo: $ok ok / $fail fail ==="
[ "$fail" -eq 0 ]
