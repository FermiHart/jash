#!/usr/bin/env bash
# JINV-095 — o SEGUNDO heredoc (e o segundo `cmd < arquivo`) tem que chegar.
#
# ═══ A PROPRIEDADE ══════════════════════════════════════════════════════
#
# Um script pode abrir quantos heredocs quiser. TODOS entregam o corpo, na
# ordem, com a mesma fidelidade do primeiro — em sequência, na mesma linha
# lógica, no mesmo comando, dentro de `if`/`for`/`while`/função/`case` e
# dentro de um comando substituído. A mesma frase vale para o mecanismo que
# os carrega: `cmd < arquivo` funciona todas as vezes, não só a primeira.
#
# ═══ O DEFEITO, medido ══════════════════════════════════════════════════
#
# **Só o PRIMEIRO heredoc de cada INVOCAÇÃO era entregue.** Do segundo em
# diante o comando rodava com entrada VAZIA, em silêncio e com rc 0:
#
#     cat <heredoc A>      bash: `alpha` `beta`      jash: `alpha`
#     alpha
#     A
#     cat <heredoc B>
#     beta
#     B
#
# Valia para as quatro formas de delimitador (nu, aspas simples, aspas
# duplas, barra invertida), para o modo indentado, para delimitadores
# diferentes, dentro de laços e dentro de comando substituído — e o corpo
# ESTAVA no arquivo temporário, correto e inteiro (medido byte a byte). O que
# não acontecia era a LEITURA. Um shell que perde o corpo de um heredoc e diz
# que deu certo é pior que um que falha.
#
# Escopo real, medido: por INVOCAÇÃO, não por script nem por linha lógica —
# e o gatilho não é o heredoc, é o `< arquivo` em que ele é reescrito:
#
#     cat < f1         bash: `aaa` `bbb`         jash: `aaa`
#     cat < f2
#     wc -c < f1       bash: `4` `4`             jash: `4` `0`   ← zero, não erro
#     wc -c < f2
#
# ═══ A CAUSA: o `dup2` não alcança o `FILE*` ════════════════════════════
#
# Um heredoc vira `cmd < /tmp/jash_heredoc_XXXX` no pré-processamento
# (c/main.c). `jash_eval_builtin_with_redir` roda o builtin NO PROCESSO DO
# SHELL: salva o fd alvo, faz `dup2(arquivo, 0)`, chama, restaura. O fd está
# certo o tempo todo — mas `cat`, `wc`, `grep`, `sed`, `awk`, `xargs` e
# `mapfile` leem pelo `FILE* stdin` do C, e o `FILE*` guarda estado que o
# `dup2` não toca: depois que o primeiro builtin lê até o fim, o sinalizador
# de fim-de-arquivo fica ACESO no stream e `fread` passa a devolver 0
# IMEDIATAMENTE, sem nunca mais encostar no fd 0. Por isso o segundo `cat`
# não lê `f2`: ele não lê NADA, até o processo morrer.
#
# Comando EXTERNO nunca sofreu (nasce com stdio virgem) — por isso o defeito
# era invisível para quem testasse com `/bin/cat`.
#
# Conserto: `jash_stdin_stream_sync()`, em c/parser/builtin_redir.c — UMA
# função, chamada por todo sítio que troca o fd 0 debaixo de um builtin
# (builtin, função, `exec` permanente e o `lastpipe` do pipeline, que tinha a
# única cópia inline do remédio e de quebra vazava um `FILE*` por pipeline).
#
# ═══ E MAIS DOIS, DA MESMA FAMÍLIA, NO MESMO CAMINHO ════════════════════
#
#   · UMA LINHA, VÁRIOS HEREDOCS. Dois operadores no mesmo comando, ou dois
#     comandos com heredoc separados por `;` na mesma linha, só coletavam o
#     primeiro; o segundo operador chegava ao avaliador como texto literal,
#     sem corpo, e as linhas do corpo dele voltavam ao parser como COMANDOS
#     (`beta: command not found`). Família JINV-016/020/022/092: DADO
#     VIRANDO COMANDO.
#
#   · TETO MUDO DE 256 BYTES na cauda da linha depois do delimitador. O corte
#     deixava o ponteiro PARADO NO MEIO DA LINHA: a cauda virava corpo e o
#     corpo virava comando. Medido: com 400 bytes depois do delimitador a
#     saída sai fora de ordem e partida ao meio. JINV-029 sobrevivendo ali.
#
# ═══ POR QUE ISTO NÃO É TEORIA ══════════════════════════════════════════
#
# Os 19 `configure` de autoconf desta máquina têm **2.559 linhas que abrem
# heredoc** (medido: uma varredura por linha do corpus). Até aqui, 1 por
# invocação era entregue. Qualquer instalador sério tem dezenas.
#
# ═══ O ORÁCULO E O CÃO DE GUARDA ════════════════════════════════════════
#
# TODA asserção compara contra `/bin/bash` rodando o MESMO arquivo no MESMO
# diretório. Nenhuma afirma nada por conta própria.
#
# Todo programa termina com um `echo AFTER` numa linha só dele. É a asserção
# que mais importa: se um corpo vazar para o parser, o `AFTER` some do
# stdout. Sem ela, um corpo que por acaso não contivesse nada executável
# passaria despercebido.
#
# O cão de guarda é `tools/deadline.sh`, NUNCA `timeout`: `timeout(1)` não
# existe nesta máquina e devolve 127 sob bash — um teste que veja rc != 124
# concluiria "não travou" de um travamento real. A asserção 0 prova que o cão
# está VIVO antes que qualquer coisa confie nele.
#
# O binário sob teste vem de `JASH_UNDER_TEST` — **nunca de `JASH_BIN`**. O
# próprio jash exporta `JASH_BIN` no ambiente dos agentes, então um gate que a
# leia mede o binário INSTALADO e fica verde contra qualquer mutante.
#
# NOTA DE FORMA (JINV-061): nenhum rótulo escreve o operador de heredoc, e
# todo delimitador de fechamento fica sozinho na sua linha do FONTE. O lint de
# asserção infalsificável varre este arquivo linha a linha e trata um
# `<`+`<`+palavra como heredoc DELE; um fechamento que não fique sozinho
# (porque a aspa da string do teste está colada nele) deixa o lint CEGO daqui
# para baixo — e um lint cego é o mesmo defeito que ele existe para caçar.
#
# PROVA NEGATIVA: registrado em SRC_MUTANTS e NULL_GATES de
# tools/mutation-check.sh. Dois mutantes de fonte deixam este gate VERMELHO —
# o helper virando no-op (25 asserções) e um heredoc por linha (2).
#
# Gate: bash tests/test_jinv095_multiple_heredocs.sh

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

# valor próprio e improvável: se aparecer na saída, a expansão aconteceu
export HOME=/CASA_DE_TESTE

printf 'aaa\n' > "$W/f1"
printf 'bbb\n' > "$W/f2"
printf 'ccc\n' > "$W/f3"

# O operador de heredoc montado a partir de uma variável nos poucos lugares em
# que o texto do programa é gerado por laço: escrevê-lo literalmente ali abriria
# um heredoc para o lint que nenhuma linha do FONTE fecha.
HD='<''<'

# ── o juiz ─────────────────────────────────────────────────────────────
# O programa vai para um ARQUIVO e os dois shells rodam sobre ele. Arquivo, e
# não `-c`: é assim que um `configure` chega, e um `-c` acrescenta mais uma
# camada de citação do shell externo — justamente o que está sob teste.
oracle() { # rotulo  programa
    local label="$1" prog="$2" jo bo jr br
    printf '%s\n' "$prog" > "$W/p.sh"
    # stderr DESCARTADO, nunca fundido (JINV-061): o jash escreve avisos de UX
    # no stderr real com contador PERSISTENTE entre invocações — fundi-los faz
    # o veredito depender de o contador cair num múltiplo de 10.
    jo=$( (cd "$W" && LC_ALL=C LANG=C HOME=/CASA_DE_TESTE "$DEADLINE" 20 "$JASH"   ./p.sh 2>/dev/null) ); jr=$?
    bo=$( (cd "$W" && LC_ALL=C LANG=C HOME=/CASA_DE_TESTE "$DEADLINE" 20 "$ORACLE" ./p.sh 2>/dev/null) ); br=$?
    if [ "$jr" = 124 ]; then
        bad "$label" "o jash TRAVOU (20 s) — prog: $(printf '%s' "$prog" | tr '\n' '|' | cut -c1-160)"
    elif [ "$br" = 124 ]; then
        ok "$label (o próprio bash travou — fora de alcance)"
    elif [ "$jo" = "$bo" ] && [ "$jr" = "$br" ]; then
        ok "$label"
    else
        bad "$label" "prog: $(printf '%s' "$prog" | tr '\n' '|' | cut -c1-200)
        jash=[$(printf '%s' "$jo" | tr '\n' '|' | cut -c1-200)] rc=$jr  !=  bash=[$(printf '%s' "$bo" | tr '\n' '|' | cut -c1-200)] rc=$br"
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

# ── 1. A CAUSA, NUA: `cmd < arquivo` mais de uma vez ───────────────────
# Sem estas, um conserto que só remendasse o heredoc passaria — e o defeito
# real (o `FILE*` que não acompanha o fd) continuaria vivo em todo `< arquivo`.
oracle "dois 'cat < arquivo' seguidos" \
'cat < f1
cat < f2
echo AFTER'
oracle "cinco '< arquivo' seguidos, alternando" \
'cat < f1
cat < f2
cat < f1
cat < f2
cat < f1
echo AFTER'
oracle "o MESMO arquivo duas vezes (não é cache de nome)" \
'cat < f1
cat < f1
echo AFTER'
oracle "builtin que conta bytes: 0 é pior que erro" \
'wc -c < f1
wc -c < f2
echo AFTER'
oracle "grep (builtin) duas vezes" \
'grep a < f1
grep b < f2
echo AFTER'
oracle "saída nunca sofreu: dois '> arquivo' (controle)" \
'echo x > o1
echo y > o2
cat o1
cat o2
echo AFTER'
oracle "externo depois de builtin e vice-versa" \
'cat < f1
/bin/cat < f2
cat < f3
echo AFTER'
oracle "redirect, cano, redirect (o lastpipe usa a MESMA função)" \
'cat < f1
echo meio | cat
cat < f2
echo AFTER'

# ── 2. HEREDOCS EM SEQUÊNCIA: 2, 3 e 20 ────────────────────────────────
oracle "dois heredocs" \
'cat <<A
alpha
A
cat <<B
beta
B
echo AFTER'
oracle "três heredocs" \
'cat <<A
1
A
cat <<B
2
B
cat <<C
3
C
echo AFTER'
prog20=""
i=1
while [ "$i" -le 20 ]; do
    prog20="${prog20}cat ${HD}H$i
linha $i
H$i
"
    i=$((i+1))
done
prog20="${prog20}echo AFTER"
oracle "vinte heredocs seguidos" "$prog20"

# ── 3. ESTILOS MISTURADOS ──────────────────────────────────────────────
oracle "os quatro estilos de delimitador na mesma sequência" \
"cat <<E1
um \$HOME
E1
cat <<'E2'
dois \$HOME
E2
cat <<\\E3
tres \$HOME
E3
cat <<-E4
	quatro \$HOME
	E4
echo AFTER"
oracle "duas vezes cada estilo, intercalados" \
"cat <<'Q'
a \$HOME
Q
cat <<U
b \$HOME
U
cat <<'Q'
c \$HOME
Q
cat <<U
d \$HOME
U
echo AFTER"

# ── 4. EXPANSÃO: o par que impede meio-conserto ────────────────────────
# Um conserto que entregasse o corpo mas perdesse a citação passaria sem isto.
oracle "citado NÃO expande, nu expande — os dois no mesmo script" \
"V=valor_da_variavel
cat <<A
\$V \$HOME
A
cat <<'B'
\$V \$HOME
B
cat <<A
\$V de novo
A
echo AFTER"
oracle "aritmética e comando substituído no SEGUNDO heredoc" \
'cat <<A
primeiro
A
cat <<B
soma $((1+1)) sub $(echo x)
B
echo AFTER'

# ── 5. CORPO VAZIO, E DELIMITADOR QUE APARECE NO CORPO ─────────────────
oracle "dois corpos VAZIOS seguidos, e o script continua" \
'cat <<A
A
cat <<B
B
echo AFTER'
oracle "corpo vazio entre dois corpos com conteúdo" \
'cat <<A
tem
A
cat <<B
B
cat <<C
tem tambem
C
echo AFTER'
oracle "delimitador aparece no corpo sem estar sozinho na linha" \
'cat <<A
xAy A z
A
cat <<B
xBy B
B
echo AFTER'

# ── 6. NA MESMA LINHA LÓGICA E NO MESMO COMANDO ────────────────────────
oracle "dois heredocs no MESMO comando: vence o último (bash)" \
'cat <<A <<B
alpha
A
beta
B
echo AFTER'
oracle "dois comandos com heredoc na MESMA linha, separados por ;" \
'cat <<A; cat <<B
alpha
A
beta
B
echo AFTER'
oracle "heredoc com cano na mesma linha, duas vezes" \
'cat <<A | tr a-z A-Z
alpha
A
cat <<B | tr a-z A-Z
beta
B
echo AFTER'
# O operador COLADO no que vem antes é forma legítima e comum, e a varredura
# por linha do conserto quase a perdeu: uma guarda de branco a mais teria feito
# o corpo voltar ao parser como comando. Fica gateado nas três formas.
oracle "operador colado no comando, duas vezes" \
'cat<<A
alpha
A
cat<<B
beta
B
echo AFTER'
oracle "operador colado, modo indentado" \
'cat<<-A
	alpha
	A
cat<<-B
	beta
	B
echo AFTER'
oracle "operador colado logo depois de um ponto-e-vírgula" \
'echo um;cat<<A
alpha
A
echo dois;cat<<B
beta
B
echo AFTER'
oracle "cauda de 400 bytes depois do delimitador (era teto mudo de 256)" \
"cat <<EOF ; echo $(printf 'y%.0s' $(seq 1 400))
body
EOF
echo AFTER"

# ── 7. DENTRO DE ESTRUTURA ─────────────────────────────────────────────
oracle "heredoc dentro de for, três voltas (corpo vê a variável do laço)" \
'for i in 1 2 3; do
cat <<EOF
iteracao $i
EOF
done
echo AFTER'
oracle "heredoc dentro de while" \
'n=0
while [ $n -lt 3 ]; do
n=$((n+1))
cat <<EOF
volta $n
EOF
done
echo AFTER'
oracle "heredoc dentro de if, e outro depois do fi" \
'if true; then
cat <<A
dentro
A
fi
cat <<B
depois
B
echo AFTER'
oracle "heredoc dentro de função, chamada duas vezes" \
'f() {
cat <<EOF
corpo de f
EOF
}
f
f
cat <<Z
fim
Z
echo AFTER'
oracle "heredoc dentro de case" \
'case x in
x)
cat <<A
ramo x
A
;;
esac
cat <<B
depois do esac
B
echo AFTER'

# ── 8. DENTRO DE COMANDO SUBSTITUÍDO ───────────────────────────────────
oracle "heredoc dentro de comando substituído, e outro fora" \
'v=$(cat <<A
de dentro
A
)
echo "v=$v"
cat <<B
de fora
B
echo AFTER'
oracle "dois comandos substituídos, cada um com heredoc" \
'a=$(cat <<A
um
A
)
b=$(cat <<B
dois
B
)
echo "$a $b"
echo AFTER'

# ── 9. CORPO GRANDE — nove tetos mudos já nasceram nesta casa ──────────
# 10 KB em cada um dos DOIS heredocs. Se algum teto voltar, o segundo corpo
# sai cortado e a comparação com o bash acusa. `big` NÃO leva nova-linha no
# fim: o delimitador precisa ficar sozinho na linha do FONTE (ver a nota de
# forma no cabeçalho).
big=""
i=1
while [ "$i" -le 200 ]; do
    big="${big}linha $i 0123456789012345678901234567890123456789
"
    i=$((i+1))
done
big="${big%
}"
oracle "dois corpos de ~10 KB, os dois inteiros" \
"cat <<A
${big}
A
cat <<B
${big}
B
echo AFTER"

echo ""
echo "=== JINV-095: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
