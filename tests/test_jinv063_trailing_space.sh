#!/usr/bin/env bash
# JINV-063 — o espaço em branco no FIM da linha é ESTRUTURA, nunca DADO.
#
#   $ echo x␣          bash: 'x\n'      jash: 'x \n'   ← vazava
#   $ echo "x "        bash: 'x \n'     jash: 'x \n'   ← certo, e tem que ficar
#
# O espaço final da LINHA DE COMANDO aparecia na SAÍDA. Cada espaço extra virava
# um espaço extra impresso.
#
# ═══ Por que isto importa ═══
#
# Não é cosmético: quebra toda comparação.
#
#     [ "$(echo x )" = "x" ]      # verdadeiro no bash, era FALSO no jash
#
# E atinge o caminho mais comum que existe — comando gerado por programa, colado
# de outro lugar, ou montado por um agente. Todos carregam, com frequência, um
# espaço final que ninguém vê. Um shell que muda o resultado por causa de um byte
# invisível não é um substrato confiável para automação.
#
# Mesma família do JINV-020/026/028/038: a fronteira entre DADO e ESTRUTURA
# apagada em silêncio.
#
# ═══ A propriedade que este gate protege ═══
#
#   Espaço/tab NÃO-CITADO e NÃO-ESCAPADO no fim é SEPARADOR — some.
#   Espaço CITADO ("x " / 'x ') ou ESCAPADO (x\ ) é DADO — fica.
#
# As duas metades têm que ser afirmadas JUNTAS. Um `rtrim` cego passa na
# primeira e reprova na segunda; um shell sem corte nenhum faz o inverso. Por
# isso cada bloco abaixo traz o caso positivo ao lado do negativo: sozinha,
# qualquer das metades transforma um conserto em regressão.
#
# ═══ Por que o oráculo é o /bin/bash ═══
#
# Nenhuma asserção codifica a saída esperada à mão. Cada linha é executada nos
# DOIS shells e comparada byte a byte. Expectativa escrita à mão é a mesma
# doença que o bug: alguém decide o que "deveria" sair, e o teste passa a
# proteger a opinião em vez do comportamento.
#
# ═══ Onde o conserto mora ═══
#
# `jash_rtrim_unquoted` (c/parser/quotes.c), chamado ANTES do unquote — quando
# `args` chega ao builtin, `echo "x "` e `echo x␣` já são a MESMA string `x `, e
# ali não há mais como distinguir. Ver o contrato em c/parser/quotes.h.
#
# O bloco 9 é estrutural: a auditoria encontrou CINCO cópias divergentes deste
# mesmo corte, e a divergência entre elas era o bug. O gate cobra que continue
# havendo uma função só.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0
# O banner de memoΔ escreve no MESMO canal da saída do builtin; sem isto ele
# entra na comparação e a falha aponta para o lugar errado.
export JASH_MEMO=0
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }

pass=0; fail=0
ok()   { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }
nota() { printf 'nota  %s\n' "$1"; }

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM

dump() { od -c < "$1" | head -3 | tr '\n' '|'; }

# par "<rótulo>" "<linha>" — roda a MESMA linha no bash e no jash e compara a
# saída byte a byte. stderr NÃO entra na comparação (avisos do shell não são
# saída do comando) e também não é engolido.
par() {
    local rot="$1" cmd="$2"
    ( cd "$T" && /bin/bash -c "$cmd" >bash.out )
    ( cd "$T" && "$JASH"     -c "$cmd" >jash.out )
    if cmp -s "$T/bash.out" "$T/jash.out"; then
        ok "$rot"
    else
        bad "$rot" "bash: $(dump "$T/bash.out")   jash: $(dump "$T/jash.out")"
    fi
}

# par_env — igual, mas com uma variável extra, para forçar uma porta específica.
par_env() {
    local rot="$1" var="$2" cmd="$3"
    ( cd "$T" && /bin/bash -c "$cmd" >bash.out )
    ( cd "$T" && env "$var" "$JASH" -c "$cmd" >jash.out )
    if cmp -s "$T/bash.out" "$T/jash.out"; then
        ok "$rot"
    else
        bad "$rot" "bash: $(dump "$T/bash.out")   jash: $(dump "$T/jash.out")"
    fi
}

# ── 1. O SEPARADOR SOME ────────────────────────────────────────────────────
par "echo x (sem espaço) — controle"          'echo x'
par "echo x␣ — um separador some"             'echo x '
par "echo x␣␣ — dois separadores somem"       'echo x  '
par "echo x<TAB> — tab também é separador"    'echo x	'
par "echo x<TAB><TAB>"                        'echo x		'
par "echo a b␣ — o último argumento é limpo"  'echo a b '
par "echo -n x␣ — sem newline, sem espaço"    'echo -n x '
par "comparação: [ \$(echo x ) = x ]"         '[ "$(echo x )" = "x" ] && echo EQ || echo NE'

# ── 2. O DADO FICA (a metade que um rtrim cego quebraria) ──────────────────
par "echo \"x \" — aspas duplas preservam"          'echo "x "'
par "echo \"x \"␣ — dado E separador na mesma linha" 'echo "x " '
par "echo \"x \"<TAB>"                              'echo "x "	'
par "echo 'x ' — aspas simples preservam"           "echo 'x '"
par "echo 'x '␣"                                    "echo 'x ' "
par "echo \"a  b\"␣␣ — espaços internos intactos"   'echo "a  b"  '
par "echo -e \"x \"␣ — o -e não muda a regra"       'echo -e "x " '
par "echo \"x\"␣ — aspas sem espaço dentro"         'echo "x" '

# ── 3. BARRA INVERTIDA: espaço escapado é DADO (medido contra o bash) ──────
par "echo x\\␣ — espaço escapado no fim da linha" 'echo x\ '
par "echo x\\␣␣ — o escapado fica, o solto sai"   'echo x\  '
par "echo x\\␣␣␣ — um dado, dois separadores"     'echo x\   '
par "echo a\\␣b — escapado no meio (controle)"    'echo a\ b'

# ── 4. NÃO-REGRESSÃO: o tokenizador não pode ganhar argumento vazio ────────
# O achado original já provava que `printf` estava CERTO. Se o corte fosse feito
# no lugar errado, é aqui que apareceria um `[]` a mais.
par "printf '[%s]' a␣"                'printf "[%s]" a '
par "printf '[%s]' a b␣"              'printf "[%s]" a b '
par "printf '[%s]' \"a \"␣"           'printf "[%s]" "a " '
par "set -- \"a b\" — fronteira de palavra" 'set -- "a b" ; echo "[$1][$#]"'
par "set -- a b␣"                     'set -- a b ; echo "[$1][$2][$#]"'
par "função: \$# não ganha posicional" 'f() { echo "[$1][$#]" ; } ; f a '
par "função: posicional citado com espaço" 'f() { echo "[$1][$#]" ; } ; f "a " '

# ── 5. VAZIO e SÓ-ESPAÇO ───────────────────────────────────────────────────
par "echo sozinho"        'echo'
par "echo␣ (só separador)" 'echo '
par 'echo ""'             'echo ""'
par 'echo ""␣'            'echo "" '
par "linha só com espaços não vira comando" '   '

# ── 6. EXPANSÃO: o resultado não-citado é PALAVRA, o citado é DADO ─────────
par 'x="a "; echo "$x" — citado preserva'   'x="a "; echo "$x"'
par 'x="a "; echo $x — não-citado é palavra' 'x="a "; echo $x '
par 'echo $(printf "a ") — cmdsub sem aspas' 'echo $(printf "a ") '
par 'echo "$(printf "a ")" — cmdsub citado'  'echo "$(printf "a ")" '
par 'echo `printf "a "`␣ — crase'            'echo `printf "a "` '
par 'echo ${x:-d}␣'                          'echo ${x:-d} '
par 'echo $((1+2))␣'                         'echo $((1+2)) '

# ── 7. AS PORTAS INDEPENDENTES ─────────────────────────────────────────────
# Uma linha simples de `-c` pode ser servida por três caminhos distintos, e no
# achado original cada um levava o espaço adiante por conta própria. Aqui cada
# porta é forçada explicitamente — sem isto o gate ficaria refém do ambiente
# (qual porta roda depende de JASH_AGENT/JASH_SIGIL estarem setados ou não).
par_env "porta ultra-rápida (-c trivial)"  "JASH_AGENT=0" 'echo x '
par_env "porta rápida de -c"               "JASH_AGENT=1" 'echo x '
par_env "porta ultra: dado escapado"       "JASH_AGENT=0" 'echo x\  '
par_env "porta rápida: dado citado"        "JASH_AGENT=1" 'echo "x " '

# script em ARQUIVO — não passa por nenhuma das vias rápidas de `-c`
printf 'echo x \necho "x " \necho x\\  \n' > "$T/s.sh"
( cd "$T" && /bin/bash    s.sh >bash.out )
( cd "$T" && "$JASH"      s.sh >jash.out )
if cmp -s "$T/bash.out" "$T/jash.out"; then
    ok "modo SCRIPT (arquivo): as três formas de uma vez"
else
    bad "modo SCRIPT (arquivo)" "bash: $(dump "$T/bash.out")   jash: $(dump "$T/jash.out")"
fi

# stdin — o caminho do REPL não-interativo
printf 'echo x \necho "x " \n' > "$T/in.txt"
( cd "$T" && /bin/bash < in.txt >bash.out )
( cd "$T" && "$JASH"   < in.txt >jash.out )
if cmp -s "$T/bash.out" "$T/jash.out"; then
    ok "modo STDIN (pipe): separador some, dado fica"
else
    bad "modo STDIN (pipe)" "bash: $(dump "$T/bash.out")   jash: $(dump "$T/jash.out")"
fi

# ── 8. SEGMENTOS: cada separador de estrutura corta pelo mesmo contrato ────
par "pipe: echo x␣ | cat"                'echo x  | cat'
par "pipe: echo \"x \"␣ | cat"           'echo "x "  | cat'
par "pipe: dado escapado atravessa"      'echo x\  | cat'
par "; : echo x␣ ; echo y␣"              'echo x ; echo y '
par "; : dado escapado atravessa"        'echo x\  ; echo y'
par "&&: echo x␣ && echo y␣"             'echo x  && echo y '
par "redirect: echo x␣ > f"              'echo x  > R1 ; cat R1'
par "redirect: echo \"x \"␣ > f"         'echo "x "  > R2 ; cat R2'
par "for: corpo com separador no fim"    'for i in a b ; do echo "$i" ; done '
par "for: corpo com dado escapado"       'for i in 1 ; do echo x\  ; done'
par "case: corpo com separador no fim"   'case y in y) echo x ;; esac '
par "case: corpo com dado escapado"      'case y in y) echo x\  ;; esac'
par "eval: uma camada por vez"           'eval "echo x " '
par "eval: dado citado sobrevive ao eval" 'eval "echo \"x \" " '

# ── 9. COMENTÁRIO: mesma família, mesmo corte ─────────────────────────────
par "echo a␣#nota"                    'echo a #note'
par "echo a␣␣#nota"                   'echo a  #note'
par "echo \"a \"␣#nota — dado fica"   'echo "a " #note'
par "echo x\\␣␣#nota — escapado fica" 'echo x\  #note'

# ── 10. ESTRUTURAL: uma lógica, um lugar ──────────────────────────────────
# A auditoria (audits/ACHADO-espaco-final-vaza-no-echo.md) nomeou a duplicação
# como o PIOR achado: cinco cópias do mesmo corte, cada uma com uma regra
# ligeiramente diferente, e a divergência entre elas era o defeito. Um gate que
# só olha comportamento deixa a sexta cópia nascer amanhã.
Q="c/parser/quotes.c"
if grep -q "^size_t jash_rtrim_unquoted" "$Q"; then
    ok "a função de corte existe, e existe em quotes.c"
else
    bad "fonte única" "jash_rtrim_unquoted sumiu de $Q"
fi
if grep -q "jash_rtrim_unquoted" c/parser/quotes.h; then
    ok "o contrato está declarado em quotes.h"
else
    bad "contrato declarado" "quotes.h não declara jash_rtrim_unquoted"
fi
usos=$(grep -rl "jash_rtrim_unquoted" c/ --include=*.c | grep -cv "quotes.c")
if [ "$usos" -ge 3 ]; then
    ok "as portas independentes usam a MESMA função ($usos arquivos)"
else
    bad "portas compartilhando o corte" \
        "só $usos arquivo(s) chamam jash_rtrim_unquoted — alguma porta voltou a ter cópia própria"
fi
# CATRACA de rtrim cego. O padrão procurado é exatamente o que foi removido das
# portas: um laço que decrementa um comprimento enquanto o último byte for
# espaço/tab, sem olhar aspas. Sete deles viraram chamada à função única; a
# camada de parsing ainda tem os de baixo, e cada um é DÍVIDA DECLARADA, não
# descuido — vários cortam também `;`/`\n` (fronteira de bloco, semântica
# diferente) ou operam sobre texto que não é comando (valor de variável, token).
#
# O número só pode DESCER. Ele não afirma que os restantes estão certos; afirma
# que ninguém acrescentou o oitavo sem passar por aqui. Foi assim que os cinco
# originais nasceram: cada um parecia inofensivo sozinho.
CEGO_BASE=17
cegos=$(grep -rnE "while *\([a-z_]+ *[>-].*(\[[a-z_]+ *- *1\]|\[[a-z_]+-1\]|\[-1\]) *== *' '" \
        c/parser c/main.c | wc -l | tr -d ' ')
if [ "$cegos" -le "$CEGO_BASE" ]; then
    ok "catraca de rtrim cego: $cegos (teto $CEGO_BASE)"
else
    bad "catraca de rtrim cego" \
        "subiu para $cegos (teto $CEGO_BASE) — uma cópia nova do corte nasceu; use jash_rtrim_unquoted"
fi
# E o marcador tem que estar no código — um invariante sem marca vira folclore.
if grep -rq "JINV-063" c/parser/quotes.c c/parser/quotes.h c/parser/eval.c c/main.c; then
    ok "marcador /* JINV-063 */ presente no código"
else
    bad "marcador no código" "JINV-063 não aparece nos arquivos do conserto"
fi

# ── 11. DIVERGÊNCIAS CONHECIDAS (fora do escopo deste invariante) ─────────
# Registradas aqui, e não escondidas: quem herdar isto merece saber o que ainda
# não bate com o bash, em vez de redescobrir.
#
# (a) O ESPELHO À ESQUERDA. `echo " "` imprime '\n' no jash e ' \n' no bash;
#     `echo "  x  "` perde os dois espaços da frente. Causa: c/builtins/echo.c
#     corta espaço à ESQUERDA de `args` sem olhar aspas — e quando `args` chega
#     lá a informação de aspas já se perdeu, exatamente como no defeito que este
#     invariante mata. É a MESMA doença virada ao contrário, e o conserto é
#     outro (echo tem que ir para argv, ou o corte da esquerda tem que subir
#     para o ponto de split). Não foi feito aqui para não misturar duas
#     cirurgias no mesmo caminho quente.
# (b) Espaço ESCAPADO como último byte do corpo de um `if ... then` de uma linha
#     ou do corpo de uma função (`f() { echo x\  ; }`): ainda vira `x\`. Esses
#     dois extratores cortam também `;` e `\n` no mesmo laço, então não são um
#     troca-a-função; pedem análise própria.
for c in 'echo " "' 'echo "  x  "' 'if true ; then echo x\  ; fi' 'f() { echo x\  ; } ; f' ; do
    ( cd "$T" && /bin/bash -c "$c" >bash.out 2>/dev/null )
    ( cd "$T" && "$JASH"   -c "$c" >jash.out 2>/dev/null )
    cmp -s "$T/bash.out" "$T/jash.out" \
        && nota "JÁ BATE com o bash (pode virar asserção): $c" \
        || nota "diverge do bash, fora do escopo do JINV-063: $c"
done

echo ""
echo "=== JINV-063: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
