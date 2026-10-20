#!/usr/bin/env bash
# JINV-022 — o corpo de um heredoc é DADO por inteiro, nunca vira comando.
#
# `c/main.c` guardava o corpo num `char content[4096]` FIXO. Acima disso o
# laço de leitura parava, o ponteiro ficava PARADO NO MEIO DO CORPO, e todo o
# resto — texto que o autor escreveu como dado — voltava ao parser e era
# EXECUTADO COMO COMANDO, linha a linha.
#
# Medido antes da correção: corpo de 5.4 KB cortado em 4095 bytes, ~1.3 KB
# restantes executados. A saída delatava o mecanismo — um pedaço de "linha
# 0093 de teste" virava `ha: command not found`.
#
# Foi assim que este bug se revelou: um relatório em markdown, passado num
# heredoc para o python, teve o próprio texto executado como comandos. Nenhum
# aviso em nenhum momento.
#
# É a mesma classe do JINV-016 (argv[0]) e do JINV-020 (separador em aspas
# aninhadas): dado virando comando. Só que aqui o gatilho é o TAMANHO — o bug
# aparece exatamente quando o conteúdo é grande demais para alguém revisar,
# que é quando ele menos vai ser notado.
#
# O oráculo é o bash: o corpo entregue tem que ser byte a byte o mesmo.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
# Avisos de UX (nudge/adoption/loopguard) vao para o STDERR — o que esta
# certo. Silencia-los aqui evita comparar ruido de interface com saida de
# programa; ja quebrou este gate uma vez.
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT INT TERM

gen() { # gen <n_linhas> -> escreve $TMP/case.sh, ecoa o tamanho do corpo
    python3 - "$1" "$TMP" <<'PY'
import sys
n, tmp = int(sys.argv[1]), sys.argv[2]
body = "\n".join("linha %05d de teste com texto de enchimento" % i for i in range(n))
open(tmp + "/case.sh", "w").write("cat << 'EOF'\n" + body + "\nEOF\n")
print(len(body))
PY
}

command -v python3 >/dev/null || { echo "SKIP: sem python3"; exit 0; }

# ── 1. o corpo entregue bate com o bash, em toda faixa de tamanho ──────────
for n in 50 100 200 500; do
    esperado=$(gen "$n")
    gj=$("$JASH" -c "$(cat "$TMP/case.sh")" 2>/dev/null | wc -c | tr -d ' ')
    gb=$(bash    -c "$(cat "$TMP/case.sh")" 2>/dev/null | wc -c | tr -d ' ')
    if [ "$gj" = "$gb" ]; then
        ok "corpo de ~$esperado bytes entregue inteiro ($gj == bash)"
    else
        bad "corpo de ~$esperado bytes" \
            "jash entregou $gj, bash entregou $gb — cortado (o resto virou comando?)"
    fi
done

# ── 2. o resto do corpo NAO pode ser executado ────────────────────────────
# Corpo grande com uma linha que, se escapasse para o parser, criaria arquivo.
python3 - "$TMP" <<'PY'
import sys
tmp = sys.argv[1]
enche = "\n".join("linha %05d de enchimento para passar do teto antigo" % i
                  for i in range(200))
body = enche + "\ntouch " + tmp + "/VAZOU\n" + enche
open(tmp + "/leak.sh", "w").write("cat << 'EOF'\n" + body + "\nEOF\n")
PY
"$JASH" -c "$(cat "$TMP/leak.sh")" >/dev/null 2>&1
if [ -f "$TMP/VAZOU" ]; then
    bad "nada do corpo e executado" \
        "o arquivo VAZOU foi criado — uma linha do CORPO rodou como comando"
else
    ok "nenhuma linha do corpo virou comando (canario ausente)"
fi

# ── 3. o delimitador ainda encerra, e o que vem DEPOIS roda ───────────────
esperado=$(gen 300)
{ cat "$TMP/case.sh"; echo 'echo DEPOIS_DO_HEREDOC'; } > "$TMP/after.sh"
out=$("$JASH" -c "$(cat "$TMP/after.sh")" 2>/dev/null | tail -1)
if [ "$out" = "DEPOIS_DO_HEREDOC" ]; then
    ok "o comando depois do delimitador executa normalmente"
else
    bad "comando apos o heredoc" "esperado DEPOIS_DO_HEREDOC, veio '$out'"
fi

# ── 4. heredoc pequeno nao regrediu ───────────────────────────────────────
out=$("$JASH" -c "cat << 'EOF'
pequeno
EOF" 2>/dev/null | tail -1)
[ "$out" = "pequeno" ] && ok "heredoc pequeno intacto" \
                       || bad "heredoc pequeno" "veio '$out'"

# ── 5. delimitador NAO citado ainda expande, em corpo grande ──────────────
python3 - "$TMP" <<'PY'
import sys
tmp = sys.argv[1]
enche = "\n".join("preenchimento %05d para ultrapassar o teto antigo" % i
                  for i in range(200))
open(tmp + "/exp.sh", "w").write("V=expandiu\ncat << EOF\n" + enche +
                                 "\nvalor=$V\nEOF\n")
PY
out=$("$JASH" -c "$(cat "$TMP/exp.sh")" 2>/dev/null | grep -c "valor=expandiu")
if [ "$out" = "1" ]; then
    ok "expansao de \$VAR ainda funciona alem do teto antigo"
else
    bad "expansao em corpo grande" "esperava 1 linha 'valor=expandiu', achei $out"
fi

# ── 6. <<- (strip de tabs) preservado em corpo grande ─────────────────────
python3 - "$TMP" <<'PY'
import sys
tmp = sys.argv[1]
linhas = "\n".join("\tlinha %05d com tab inicial para o strip" % i
                   for i in range(200))
open(tmp + "/dash.sh", "w").write("cat <<- EOF\n" + linhas + "\n\tEOF\n")
PY
com_tab=$("$JASH" -c "$(cat "$TMP/dash.sh")" 2>/dev/null | grep -c "^	")
if [ "$com_tab" = "0" ]; then
    ok "<<- ainda remove tabs em corpo grande"
else
    bad "<<- em corpo grande" "$com_tab linhas ainda comecam com tab"
fi

echo ""
echo "=== JINV-022: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
