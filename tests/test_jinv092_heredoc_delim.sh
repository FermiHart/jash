#!/usr/bin/env bash
# JINV-092 — `<<\EOF`: a barra invertida CITA o delimitador do heredoc.
#
# ═══ A PROPRIEDADE ══════════════════════════════════════════════════════
#
# POSIX XCU 2.7.4 é uma frase só: *se QUALQUER caractere da palavra
# delimitadora estiver citado*, o delimitador é a palavra APÓS REMOÇÃO DE
# CITAÇÃO, e o corpo NÃO sofre expansão. Não é "começa e acaba com aspas" — é
# *qualquer* caractere, por *qualquer* das três citações:
#
#     <<'EOF'    <<"EOF"    <<\EOF    <<E"OF"    <<EO\F
#
# As cinco valem, e as cinco fecham numa linha que diz exatamente `EOF`.
#
# ═══ O DEFEITO: duas leituras, as duas cegas para a barra ═══════════════
#
# Existiam DUAS leituras independentes da palavra delimitadora — c/main.c (a
# que materializa o corpo num arquivo) e `jash_protect_quoted_heredocs`
# (c/parser/heredoc.c, a que blinda o corpo da expansão) — e as duas só
# reconheciam `'…'` e `"…"` inteiriças. `<<\EOF` errava nas DUAS, e cada uma
# de um jeito:
#
#   1. o BLINDADOR não via citação nenhuma, então `$HOME` no corpo era
#      EXPANDIDO — exatamente o que o autor citou para impedir;
#
#   2. o MATERIALIZADOR guardava o delimitador como `\EOF` e depois procurava
#      uma linha `\EOF` que nunca chega. O laço de captura corria até o FIM DA
#      ENTRADA, e todo o texto que o autor escreveu como DADO voltava ao
#      parser e era EXECUTADO COMO COMANDO.
#
# O (2) é a família do JINV-016/020/022: DADO VIRANDO COMANDO. E o sintoma
# que ele produz não se parece nada com a causa — o corpo vazado contém
# `if`/`fi` soltos, e o que o usuário vê é
#
#     jash: syntax error: unexpected end of if statement (missing fi)
#
# ═══ POR QUE ISTO NÃO É TEORIA ══════════════════════════════════════════
#
# `<<\EOF` / `<<\_ACEOF` é o ESTILO DA CASA do autoconf: 15 a 20 ocorrências
# por `configure`. Era a causa medida da classe `missing fi`, **20 das 38
# invocações do corpus de 19 `configure` desta máquina** — a maior classe de
# falha que restava.
#
# ═══ A RÉGUA: os dois lados da citação ══════════════════════════════════
#
# Um conserto que só olhasse a barra passaria metade. As asserções vêm em
# pares — o citado NÃO expande e FECHA; o não-citado EXPANDE e também fecha:
#
#   <<\EOF   com `$HOME` no corpo → sai `$HOME` literal, e `AFTER` roda
#   <<EOF    com `$HOME` no corpo → sai o valor,        e `AFTER` roda
#
# O `AFTER` é a asserção que mais importa: é ela que prova que o corpo NÃO
# vazou para o parser. Sem ela, um delimitador que nunca casa passa despercebido
# sempre que o corpo por acaso não contiver nada executável.
#
# ═══ O ORÁCULO E O CÃO DE GUARDA ════════════════════════════════════════
#
# TODA asserção compara contra `/bin/bash` rodando o MESMO arquivo no MESMO
# diretório. Nenhuma afirma nada por conta própria.
#
# O cão de guarda é `tools/deadline.sh`, NUNCA `timeout`: `timeout(1)` não
# existe nesta máquina e devolve 127 sob bash — um teste que veja rc≠124
# concluiria "não travou" de um travamento real. E `command -v timeout` MENTE
# quando quem pergunta é o jash (lá é builtin). Um delimitador que nunca casa
# é um candidato natural a travar, então a asserção 0 prova que o cão está
# VIVO antes que qualquer coisa confie nele.
#
# O binário sob teste vem de `JASH_UNDER_TEST` — **nunca de `JASH_BIN`**. O
# próprio jash exporta `JASH_BIN` no ambiente dos agentes, então um gate que a
# leia mede o binário INSTALADO e fica verde contra qualquer mutante.
#
# PROVA NEGATIVA: JASH_UNDER_TEST=<binário sem o conserto> deixa este gate
# VERMELHO (JINV-061). Registrado em SRC_MUTANTS de tools/mutation-check.sh.
#
# ═══ O QUE ESTE GATE NÃO ACUSA (medido, PRÉ-EXISTENTE) ══════════════════
#
# **Só o PRIMEIRO heredoc de um script é materializado.** Do segundo em
# diante o corpo some em silêncio e o comando roda sem entrada:
#
#     cat <<EOF        bash: `um` `dois` `AFTER`
#     um               jash: `um`        `AFTER`   ← `dois` sumiu, rc 0
#     EOF
#     cat <<EOF
#     dois
#     EOF
#     echo AFTER
#
# Vale para as TRÊS formas (`<<EOF`, `<<'EOF'`, `<<\EOF`) e para
# delimitadores diferentes entre si, e o binário anterior a este conserto
# erra igual — logo não é desta família nem é regressão daqui. Antes do
# conserto o caso `<<\EOF` era PIOR (o corpo virava comando); agora ele
# apenas some, como os outros dois já sumiam. É o próximo bloqueador medido
# dos `configure`, e não é asserido aqui porque seria outro conserto, com
# outra medida.

set -u
cd "$(dirname "$0")/.." || exit 2
ROOT="$PWD"
JASH="${JASH_UNDER_TEST:-$ROOT/build/jash}"
DEADLINE="$ROOT/tools/deadline.sh"
ORACLE=/bin/bash
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0 JASH_MEMO=0
export JASH_ABSORB=0
[ -x "$JASH" ]     || { echo "SKIP: $JASH ausente"; exit 0; }
[ -x "$DEADLINE" ] || { echo "SKIP: $DEADLINE ausente"; exit 0; }
[ -x "$ORACLE" ]   || { echo "SKIP: $ORACLE ausente — sem oráculo não há gate"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

W=$(mktemp -d) || exit 2
trap 'rm -rf "$W"' EXIT INT TERM

# Um valor de $HOME próprio e improvável: se ele aparecer na saída, a expansão
# aconteceu; se `$HOME` aparecer literal, não aconteceu. Sem isto o teste
# dependeria do $HOME real da máquina.
export HOME=/CASA_DE_TESTE

# ── o juiz ─────────────────────────────────────────────────────────────
# O programa vai para um ARQUIVO e os dois shells rodam sobre ele. Arquivo, e
# não `-c`: é assim que um `configure` chega, e um `-c` acrescenta mais uma
# camada de citação do shell externo — justamente o que está sob teste.
oracle() { # rotulo  programa
    local label="$1" prog="$2" jo bo jr br
    printf '%s\n' "$prog" > "$W/p.sh"
    # stderr DESCARTADO, nunca fundido (JINV-061): o jash escreve avisos de UX
    # no stderr real (nudge/adoption/loopguard) com contador PERSISTENTE entre
    # invocações — fundi-los na captura faz o veredito depender de o contador
    # cair num múltiplo de 10. Verde por sorte, nunca por mérito. O que esta
    # propriedade afirma vive todo no stdout e no rc: se o delimitador não
    # casar, o corpo vaza para o parser e o `AFTER` some do stdout.
    jo=$( (cd "$W" && LC_ALL=C LANG=C HOME=/CASA_DE_TESTE "$DEADLINE" 10 "$JASH"   ./p.sh 2>/dev/null) ); jr=$?
    bo=$( (cd "$W" && LC_ALL=C LANG=C HOME=/CASA_DE_TESTE "$DEADLINE" 10 "$ORACLE" ./p.sh 2>/dev/null) ); br=$?
    if [ "$jr" = 124 ]; then
        bad "$label" "o jash TRAVOU (10 s) — delimitador que nunca casa? prog: $(printf '%s' "$prog" | tr '\n' '|')"
    elif [ "$br" = 124 ]; then
        ok "$label (o próprio bash travou — fora de alcance)"
    elif [ "$jo" = "$bo" ] && [ "$jr" = "$br" ]; then
        ok "$label"
    else
        bad "$label" "prog: $(printf '%s' "$prog" | tr '\n' '|')
        jash=[$(printf '%s' "$jo" | tr '\n' '|')] rc=$jr  ≠  bash=[$(printf '%s' "$bo" | tr '\n' '|')] rc=$br"
    fi
}

# ── 0. ANTI-INÉRCIA: o cão de guarda está vivo? ────────────────────────
"$DEADLINE" 2 sleep 30 >/dev/null 2>&1
if [ $? = 124 ]; then
    ok "cão de guarda late: travamento real devolve 124 (não 127)"
else
    bad "watchdog inerte" \
        "deadline.sh 2 sleep 30 não devolveu 124 — as asserções de prazo estão sem valor"
fi

# ── 1. O DEFEITO: a barra invertida cita ───────────────────────────────
# `$HOME` tem que sair LITERAL, e `AFTER` tem que rodar (o corpo não vazou).
oracle "<<\\EOF cita: corpo literal e o script continua" \
'cat <<\EOF
BODY $HOME
EOF
echo AFTER'
oracle "<<\\_ACEOF — o delimitador real do autoconf" \
'cat <<\_ACEOF
BODY $HOME
_ACEOF
echo AFTER'
oracle "<<-\\EOF — indentado E citado" \
'cat <<-\EOF
	BODY $HOME
	EOF
echo AFTER'

# ── 2. AS OUTRAS CITAÇÕES (o par que impede meio-conserto) ─────────────
oracle "<<'"'"'EOF'"'"' cita (controle que já passava)" \
'cat <<'"'"'EOF'"'"'
BODY $HOME
EOF
echo AFTER'
oracle "<<\"EOF\" cita (controle que já passava)" \
'cat <<"EOF"
BODY $HOME
EOF
echo AFTER'
oracle "<<E\"OF\" — citação PARCIAL também cita" \
'cat <<E"OF"
BODY $HOME
EOF
echo AFTER'
oracle "<<EO\\F — barra no MEIO da palavra" \
'cat <<EO\F
BODY $HOME
EOF
echo AFTER'

# ── 3. O OUTRO LADO: sem citação, EXPANDE ──────────────────────────────
# Sem esta, um conserto que simplesmente nunca mais expandisse passaria.
oracle "<<EOF sem citação EXPANDE o corpo" \
'cat <<EOF
BODY $HOME
EOF
echo AFTER'
oracle "<<EOF expande \$((1+1)) e \`cmd\`" \
'cat <<EOF
soma $((1+1))
EOF
echo AFTER'
oracle "<<\\EOF NÃO expande \$((1+1))" \
'cat <<\EOF
soma $((1+1))
EOF
echo AFTER'

# ── 4. O CORPO NÃO VAZA — a asserção que nomeia o sintoma ──────────────
# Um corpo com `if`/`fi` soltos: se o delimitador não casar, este texto volta
# ao parser e sai exatamente `missing fi`. É o elo entre a causa e o sintoma
# medido nos `configure`.
oracle "corpo com if/fi soltos NÃO volta ao parser (a origem do 'missing fi')" \
'cat <<\EOF
if test -z "$x"; then
  echo dentro
fi
EOF
echo AFTER'
oracle "corpo com aspas desbalanceadas não quebra o parser" \
'cat <<\EOF
uma " aspa sozinha e um '"'"' tambem
EOF
echo AFTER'

# ── 5. FRONTEIRA DA PALAVRA — o metacaractere CITADO faz parte ─────────
oracle "delimitador com metacaractere CITADO ('A;B')" \
'cat <<'"'"'A;B'"'"'
corpo
A;B
echo AFTER'
oracle "metacaractere NU termina a palavra (cat <<A; echo meio)" \
'cat <<A; echo meio
um
A
echo AFTER'

# ── 6. A CAUDA DEPOIS DO DELIMITADOR CONTINUA VIVA ─────────────────────
oracle "<<\\EOF com cano na mesma linha" \
'cat <<\EOF | tr a-z A-Z
body $HOME
EOF
echo AFTER'

echo ""
echo "=== JINV-092: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
