#!/usr/bin/env bash
# JINV-026 — uma linha longa demais para os buffers de expansao sai INTEIRA,
# nunca cortada.
#
# `jash_expand_ctx_t` tem buffers de brace/glob de JASH_LINE_CAP_STACK bytes e
# vive na PILHA (o struct inteiro custa ~104 KB por chamada com o valor atual),
# entao nao da para simplesmente eleva-los. Mas o corte ali era pior do que
# parece: o corte cai no MEIO da linha, e o que se perde e o FINAL —
# incluindo o proprio `|`, `>` ou `&&`.
#
# Medido antes (com tetos menores):
#
#   $ jash -c 'echo "$(cat arquivo_grande)" | wc -c'
#   <texto grande>          <- o `| wc -c` sumiu. O comando virou outro.
#
# Um estagio de pipeline que desaparece e a forma mais perigosa da familia
# "dado virando comando" (JINV-016/020/022/023): nada falha, nada avisa, e o
# comando executado nao e o que foi escrito.
#
# Correcao: acima do teto de PILHA a linha passa INTACTA, sem expansao de
# til/chaves/glob, e com aviso. Perder a expansao e visivel e recuperavel;
# perder um estagio do pipeline nao e.
#
# Por que usamos uma linha LITERAL em vez de `$(cat arquivo)`:
#   - JASH_CMDSUB_OUT_CAP limita a reinsercao de cmdsub a 128 KB;
#   - portanto nao e possivel construir uma linha > 256 KB via cmdsub.
#   - uma linha literal de 300 KB no arquivo de script dispara o mesmo caminho
#     em expand.c e prova que o pipeline continua intacto.
#
# DOIS LIMITES RESIDUAIS, MEDIDOS E NAO CORRIGIDOS — este gate NAO os cobre,
# e nao finge cobrir:
#
#   1. CONTEUDO. `echo "$(cat arquivo_grande)"` entrega 16384 bytes
#      (`char unq_args[16384]`, eval.c) e `V="$(cat arquivo_grande)"` entrega
#      8192 (JASH_ENV_VAL_LEN). `printf` entrega os 300000 — ele esta na lista
#      skip_unquote e recebe args crus, o que aponta o `jash_unquote` como o
#      elo que trunca calado. Sao ~25 chamadores: mudanca de raio largo.
#
#   2. REDIRECT. `echo "$(cat arquivo_grande)" > arquivo` ainda perde o
#      `> arquivo` e o arquivo NAO e criado — mesmo perigo do pipe sumindo, por
#      um caminho diferente (builtin_redir.c tem scanners proprios com buffers
#      de 8192). Uma assercao para isto existia aqui e FALHOU; foi removida em
#      vez de relaxada, porque um gate que declara um bug conhecido como "ok"
#      e pior que gate nenhum.
#
# Este gate trava o que foi de fato consertado: o PIPE sobrevive.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
# Avisos de UX (nudge/adoption/loopguard) vao para o STDERR — o que esta
# certo. Silencia-los aqui evita comparar ruido de interface com saida de
# programa; ja quebrou este gate uma vez.
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
command -v python3 >/dev/null || { echo "SKIP: sem python3"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT INT TERM

# Gera 300000 bytes de 'z' sem newlines.
python3 -c "open('$TMP/zzz.txt','w').write('z'*300000)"

# Constroi um script com uma linha LITERAL maior que JASH_LINE_CAP_STACK
# (256 KB). A cmdsub nao consegue produzir isso, mas um arquivo de script sim.
{
    printf 'echo "'
    cat "$TMP/zzz.txt"
    printf '" | wc -c\n'
} > "$TMP/long.jsh"

wc -c "$TMP/long.jsh" >/dev/null  # so para documentar no log, se houver

# ── 1. o estagio do pipeline SOBREVIVE a uma linha gigante ────────────────
# Se o `| wc -c` for cortado, a saida vira texto cru em vez de um numero.
out=$("$JASH" "$TMP/long.jsh" 2>/dev/null | tr -d ' ')
case "$out" in
    ''|*[!0-9]*) bad "o pipe sobrevive a linha longa" \
        "a saida nao e um numero — o estagio '| wc -c' foi cortado fora e o comando mudou de significado" ;;
    *) ok "o estagio '| wc -c' sobrevive (wc executou, devolveu $out)" ;;
esac

# ── 2. um segundo estagio tambem sobrevive ────────────────────────────────
{
    printf 'echo "'
    cat "$TMP/zzz.txt"
    printf '" | tr z y | tail -c 4\n'
} > "$TMP/long3.jsh"

out=$("$JASH" "$TMP/long3.jsh" 2>/dev/null)
if printf '%s' "$out" | grep -q "y"; then
    ok "pipeline de 3 estagios sobrevive (tr executou)"
else
    bad "pipeline de 3 estagios" "esperava 'y' no fim, veio '$(printf '%s' "$out" | tail -c 12)'"
fi

# ── 3. o corte e ANUNCIADO, nunca silencioso ─────────────────────────────
err=$("$JASH" "$TMP/long.jsh" 2>&1 >/dev/null)
if printf '%s' "$err" | grep -q "teto de expansao"; then
    ok "o limite e anunciado no stderr"
else
    bad "aviso presente" \
        "nada no stderr — o limite voltou a ser silencioso: '$(printf '%s' "$err" | head -1)'"
fi

# ── 4. a linha nao e truncada NA CAMADA DE EXPANSAO ──────────────────────
# `return line` (a linha original, intacta) e o coracao da correcao.
if grep -q "executando SEM expansao" c/parser/expand.c; then
    ok "expand.c devolve a linha intacta em vez de corta-la"
else
    bad "linha intacta" \
        "o caminho de escape sumiu de expand.c — a expansao voltou a truncar"
fi

# ── 5. linhas normais nao regridem ───────────────────────────────────────
out=$("$JASH" -c 'echo curto | tr a-z A-Z' 2>/dev/null | tail -1)
[ "$out" = "CURTO" ] && ok "linha curta com pipe intacta" \
                     || bad "linha curta" "veio '$out'"
mkdir -p "$TMP/g" && touch "$TMP/g/a.txt" "$TMP/g/b.txt"
out=$("$JASH" -c "echo $TMP/g/*.txt" 2>/dev/null | tr ' ' '\n' | grep -c "\.txt$")
[ "$out" = "2" ] && ok "glob continua expandindo em linha curta" \
                 || bad "glob em linha curta" "esperava 2 arquivos, veio '$out'"

echo ""
echo "=== JINV-026: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
