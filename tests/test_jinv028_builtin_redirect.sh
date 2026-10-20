#!/usr/bin/env bash
# JINV-028 — o redirect de um builtin não pode DESAPARECER.
#
# `char clean[1024]` em `jash_eval_builtin_with_redir` (builtin_redir.c) era o
# destino de `parse_redirs_inplace`. Acima do teto, o redirect era perdido e o
# builtin escrevia em **stdout** como se nenhum `>` existisse — sem uma linha
# de aviso.
#
# Medido por bisseção, antes da correção:
#
#   echo "$(cat 1019_bytes)" > saida.txt   →  arquivo criado
#   echo "$(cat 1020_bytes)" > saida.txt   →  arquivo NÃO criado, stderr VAZIO
#
# **1020 bytes.** Não é caso extremo: é `echo "$(cat config.json)" > out.txt`.
# Com um comando EXTERNO o mesmo redirect funcionava até 16 KB — o defeito era
# só do caminho de builtin, o mais usado.
#
# Mesma família do JINV-022 (corpo de heredoc virando comando) e do JINV-026
# (o `|` sumindo): quando o corte cai numa linha de comando, o que se perde é o
# FINAL — e o final é onde moram o `>`, o `|` e o `&&`. O comando executado
# deixa de ser o comando escrito, e o output vai para o lugar errado.
#
# A correção não precisa de teto: `clean` é sempre um SUBCONJUNTO de `args` (é
# `args` menos os tokens de redirect), então `strlen(args) + 1` é um tamanho
# EXATO que jamais estoura.
#
# O elo do CONTEÚDO era um irmão: `char unquoted[1024]`, o destino da remoção
# de aspas no mesmo arquivo (JINV-042). Com ele, `echo "$(cat 5KB)" > saida`
# gravava 1024 bytes enquanto `printf` — que está em skip_unquote e não passa
# por ali — gravava os 5000. A diferença entre dois builtins no MESMO redirect
# foi o que denunciou. `unquoted` é sempre <= `clean` (remoção de aspas só
# encurta), então `strlen(clean)+1` é exato e jamais estoura.

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

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM

# ── 1. o redirect SOBREVIVE em todo tamanho (o perigo real) ───────────────
# Se ele sumir, o output vai para stdout e o arquivo nao existe.
for n in 500 1019 1020 4000 16000 60000; do
    python3 -c "open('$T/in.txt','w').write('z'*$n)"
    rm -f "$T/out.txt"
    "$JASH" -c "echo \"\$(cat $T/in.txt)\" > $T/out.txt" >/dev/null 2>&1
    if [ -f "$T/out.txt" ]; then
        ok "redirect honrado com args de $n bytes (arquivo criado)"
    else
        bad "redirect com $n bytes" \
            "o arquivo NAO foi criado — o '> arquivo' sumiu e o output foi para stdout"
    fi
done

# ── 2. o teto exato de 1020 morreu ───────────────────────────────────────
# Este era o limiar: 1019 funcionava, 1020 nao. Uma regressao volta a mata-lo
# exatamente aqui.
python3 -c "open('$T/in.txt','w').write('z'*1020)"
rm -f "$T/out.txt"
"$JASH" -c "echo \"\$(cat $T/in.txt)\" > $T/out.txt" >/dev/null 2>&1
if [ -f "$T/out.txt" ]; then
    ok "o limiar de 1020 bytes nao existe mais"
else
    bad "limiar de 1020" "voltou o teto de char clean[1024]"
fi

# ── 3. o output NAO vaza para stdout quando ha redirect ──────────────────
# Este e o dano concreto: com o redirect perdido, o texto ia para o terminal.
python3 -c "open('$T/in.txt','w').write('z'*4000)"
saida=$("$JASH" -c "echo \"\$(cat $T/in.txt)\" > $T/out2.txt" 2>/dev/null | wc -c | tr -d ' ')
if [ "$saida" -le 1 ]; then
    ok "nada vaza para stdout quando ha '>' (stdout vazio)"
else
    bad "vazamento para stdout" \
        "$saida bytes sairam no stdout apesar do '> arquivo' — o redirect foi ignorado"
fi

# ── 3b. o CONTEÚDO chega inteiro (JINV-042) ──────────────────────────────
for n in 500 1000 5000 60000; do
    python3 -c "open('$T/in.txt','w').write('z'*$n)"
    rm -f "$T/c.txt"
    "$JASH" -c "echo \"\$(cat $T/in.txt)\" > $T/c.txt" >/dev/null 2>&1
    got=$(wc -c < "$T/c.txt" 2>/dev/null | tr -d ' ')
    esperado=$((n + 1))
    if [ "$got" = "$esperado" ]; then
        ok "conteudo de $n bytes gravado inteiro ($got)"
    else
        bad "conteudo de $n bytes" \
            "gravou $got, esperado $esperado — voltou um teto no caminho de unquote"
    fi
done

# E o printf (que nao passa por unquote) tem que concordar com o echo: dois
# builtins divergindo no MESMO redirect e o sintoma classico de um teto so
# num dos caminhos.
python3 -c "open('$T/in.txt','w').write('z'*5000)"
rm -f "$T/pe.txt" "$T/pp.txt"
"$JASH" -c "echo \"\$(cat $T/in.txt)\" > $T/pe.txt" >/dev/null 2>&1
"$JASH" -c "printf '%s' \"\$(cat $T/in.txt)\" > $T/pp.txt" >/dev/null 2>&1
e=$(wc -c < "$T/pe.txt" 2>/dev/null | tr -d ' ')
pf=$(wc -c < "$T/pp.txt" 2>/dev/null | tr -d ' ')
if [ "$e" = "$((pf + 1))" ]; then
    ok "echo e printf concordam no mesmo redirect ($e vs $pf+newline)"
else
    bad "echo vs printf" "echo=$e printf=$pf"
fi
# ── 4. o dimensionamento e EXATO, nao um teto novo ───────────────────────
if grep -q "strlen(args) + 1" c/parser/builtin_redir.c || \
   grep -q "args_len + 1" c/parser/builtin_redir.c; then
    ok "clean e dimensionado por strlen(args) (exato, nunca estoura)"
else
    bad "dimensionamento exato" \
        "voltou um teto fixo — 'clean' e subconjunto de 'args', o tamanho certo e strlen(args)+1"
fi

# ── 5. os modos de redirect continuam funcionando ────────────────────────
"$JASH" -c "echo primeiro > $T/m.txt; echo segundo >> $T/m.txt" >/dev/null 2>&1
n=$(wc -l < "$T/m.txt" 2>/dev/null | tr -d ' ')
[ "$n" = "2" ] && ok "'>' e '>>' continuam corretos (2 linhas)" \
               || bad "modos de redirect" "esperava 2 linhas, veio '$n'"

"$JASH" -c "echo conteudo > $T/r.txt; cat < $T/r.txt" 2>/dev/null | grep -q conteudo \
    && ok "'<' continua correto" || bad "redirect de entrada" "cat < arquivo falhou"

out=$("$JASH" -c "ls /caminho/que/nao/existe 2> $T/e.txt; wc -c < $T/e.txt" 2>/dev/null | tr -d ' ')
if [ -n "$out" ] && [ "$out" -gt 0 ]; then
    ok "'2>' captura stderr ($out bytes)"
else
    bad "redirect de stderr" "nada capturado em 2> arquivo"
fi

echo ""
echo "=== JINV-028: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
