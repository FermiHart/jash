#!/usr/bin/env bash
# JINV-087 — a divisão por `$IFS` só corta o que veio de expansão NÃO CITADA.
#
# ═══ A PROPRIEDADE ══════════════════════════════════════════════════════
#
# POSIX XCU 2.6.5 é uma frase só: a separação de campos se aplica ao RESULTADO
# DE EXPANSÃO NÃO CITADA. Nunca a texto literal. Nunca ao que saiu de dentro de
# aspas. A lista de palavras do `for` aplicava o mapa de `$IFS` à cadeia
# INTEIRA, DEPOIS de expandir tudo — e nesse ponto não existe mais nenhuma
# diferença entre um `:` que veio de `$P` e um `:` que o autor escreveu:
#
#   IFS=:; for d in /bin:/usr/bin      bash: 1 item · jash: 2   ← literal
#   IFS=:; for d in "$P"               bash: 1 item · jash: 2   ← citado
#   IFS=:; for d in $P                 bash: 2 itens · jash: 2  ← já estava certo
#
# É DADO VIRANDO ESTRUTURA — a mesma família de JINV-016 (argv[0] injetado),
# JINV-020 (separador em aspas aninhadas), JINV-028 (`>` perdido), JINV-032
# (`;;` em aspas) e JINV-083 (palavra-chave em aspas). Em todos, a informação
# de citação EXISTIA e foi descartada antes do ponto que precisava dela; em
# todos, o conserto foi preservar a marca até lá, nunca filtrar depois.
#
# ═══ POR QUE ISTO NÃO É TEORIA ══════════════════════════════════════════
#
# É o idioma `as_dir` de varredura de PATH do autoconf, que está nos 19
# `configure` desta máquina:
#
#     as_save_IFS=$IFS; IFS=:
#     for as_dir in $PATH
#     do
#       IFS=$as_save_IFS
#       test -z "$as_dir" && as_dir=.
#       ...
#     done
#
# O campo VAZIO (asserções `vazio-*`) não é preciosismo: é ele que faz o
# `PATH=/bin::/usr/bin` significar "o diretório corrente está no meio", e é a
# razão de o autoconf escrever aquele `test -z` na linha seguinte.
#
# ═══ A RÉGUA DO ORÁCULO: a palavra, não o byte ══════════════════════════
#
# Medido contra /bin/bash, não deduzido: o bash divide por PALAVRA. Se a
# palavra contém uma expansão não citada, os caracteres de IFS LITERAIS dela
# TAMBÉM cortam — mas os que estão em aspas ou escapados, não:
#
#   IFS=:; for d in a:b        → [a:b]       nenhuma expansão: nada corta
#   IFS=:; for d in $P:x       → [0] [x]     o `:` literal corta
#   IFS=:; for d in "$P":x     → [0:x]       a expansão era citada
#   IFS=:; for d in $P'a:b'    → [0a:b]      o `:` está em aspas
#   IFS=:; for d in ${P}a\:b   → [0a:b]      o `:` está escapado
#
# Um conserto que só olhasse o byte acertaria a 1ª e a 3ª e erraria a 2ª; um
# que marcasse a palavra inteira sem olhar aspas erraria a 4ª e a 5ª. As cinco
# juntas são a propriedade — por isso estão todas aqui.
#
# ═══ O QUE ESTE GATE NÃO ACUSA ══════════════════════════════════════════
#
# Duas divergências PRÉ-EXISTENTES foram encontradas ao escrever este gate e
# NÃO são asseridas aqui — em ambas o binário anterior responde igual ao novo,
# logo não são desta propriedade e mexer nelas seria outro conserto com outra
# medida. Ficam nomeadas para quem for atacá-las:
#
#   1. `for d in ~/x` não expande o til (o `for` nunca teve o estágio de til).
#   2. `for f in ./*.log` devolve `cc.log`, não `./cc.log` — o glob do jash
#      normaliza o prefixo `./`. Por isso a asserção de "glob depois de
#      expansão" usa um subdiretório de verdade em vez de `.`: senão mediria a
#      normalização de caminho, não a divisão de campos.
#
# ═══ O ORÁCULO E O CÃO DE GUARDA ════════════════════════════════════════
#
# TODA asserção compara contra `/bin/bash` rodando o MESMO arquivo no MESMO
# diretório. Nenhuma afirma nada por conta própria.
#
# O cão de guarda é `tools/deadline.sh`, NUNCA `timeout`: `timeout(1)` não
# existe nesta máquina e devolve 127 sob bash — um teste que veja rc≠124
# concluiria "não travou" de um travamento real. E `command -v timeout` MENTE
# quando quem pergunta é o jash (lá é builtin). A asserção 0 prova que o cão
# está VIVO antes que qualquer coisa confie nele.
#
# O binário sob teste vem de `JASH_UNDER_TEST` — **nunca de `JASH_BIN`**. O
# próprio jash exporta `JASH_BIN` no ambiente dos agentes, então um gate que a
# leia mede o binário INSTALADO e fica verde contra qualquer mutante. Já mordeu
# três agentes; a prova negativa deste conserto depende disso.

set -u
cd "$(dirname "$0")/.." || exit 2
ROOT="$PWD"
JASH="${JASH_UNDER_TEST:-$ROOT/build/jash}"
DEADLINE="$ROOT/tools/deadline.sh"
ORACLE=/bin/bash
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0 JASH_MEMO=0
[ -x "$JASH" ]     || { echo "SKIP: $JASH ausente"; exit 0; }
[ -x "$DEADLINE" ] || { echo "SKIP: $DEADLINE ausente"; exit 0; }
[ -x "$ORACLE" ]   || { echo "SKIP: $ORACLE ausente — sem oráculo não há gate"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

W=$(mktemp -d) || exit 2
trap 'rm -rf "$W"' EXIT INT TERM
: > "$W/aa.txt"; : > "$W/bb.txt"; : > "$W/cc.log"
mkdir -p "$W/sub"; : > "$W/sub/inner.txt"

# ── o juiz ─────────────────────────────────────────────────────────────
# Escreve o programa num ARQUIVO e roda os dois shells sobre ele, no mesmo
# diretório e com o mesmo locale. Arquivo, e não `-c`, porque é assim que um
# `configure` chega — e porque um `-c` passa por mais uma camada de citação do
# shell externo, que é justamente o que está sob teste.
oracle() { # rotulo  programa
    local label="$1" prog="$2" jo bo jr br
    printf '%s\n' "$prog" > "$W/p.sh"
    jo=$( (cd "$W" && LC_ALL=C LANG=C "$DEADLINE" 10 "$JASH"   "$W/p.sh" 2>/dev/null) ); jr=$?
    bo=$( (cd "$W" && LC_ALL=C LANG=C "$DEADLINE" 10 "$ORACLE" "$W/p.sh" 2>/dev/null) ); br=$?
    if [ "$jr" = 124 ]; then
        bad "$label" "o jash TRAVOU (10 s) — programa: $(printf '%s' "$prog" | tr '\n' '|')"
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

# ── 1. AS TRÊS DO DEFEITO ──────────────────────────────────────────────
oracle "literal NÃO divide" \
  'IFS=:
for d in /bin:/usr/bin; do echo "[$d]"; done'
oracle "citado NÃO divide" \
  'P=/bin:/usr/bin
IFS=:
for d in "$P"; do echo "[$d]"; done'
oracle "não-citado DIVIDE (não regride)" \
  'P=/bin:/usr/bin
IFS=:
for d in $P; do echo "[$d]"; done'

# ── 2. O IDIOMA as_dir DO AUTOCONF, INTEIRO ────────────────────────────
oracle "as_dir: lista literal" \
  'as_save_IFS=$IFS; IFS=:
for as_dir in /bin:/usr/bin
do
  IFS=$as_save_IFS
  test -z "$as_dir" && as_dir=.
  echo "[$as_dir]"
done
echo fim'
oracle "as_dir: PATH em variável (com campo vazio)" \
  'P=/bin::/usr/bin
as_save_IFS=$IFS; IFS=:
for as_dir in $P
do
  IFS=$as_save_IFS
  test -z "$as_dir" && as_dir=.
  echo "[$as_dir]"
done
echo fim'

# ── 3. ASPAS EM VOLTA E NO MEIO ────────────────────────────────────────
oracle "aspas com texto em volta: \"a\${P}b\"" \
  'P=x:y
IFS=:
for d in "a${P}b"; do echo "[$d]"; done'
oracle "sem aspas com texto em volta: a\${P}b" \
  'P=x:y
IFS=:
for d in a${P}b; do echo "[$d]"; done'
oracle "aspas simples são literais" \
  'IFS=:
for d in '"'"'a:b'"'"'; do echo "[$d]"; done'
oracle "duas palavras literais com separador" \
  'IFS=:
for d in "a:b" c:d; do echo "[$d]"; done'
oracle "escape protege o separador" \
  'IFS=:
for d in a\:b; do echo "[$d]"; done'

# ── 4. A RÉGUA DA PALAVRA (medida no oráculo) ──────────────────────────
oracle "palavra com expansão: o separador literal corta" \
  'P=0
IFS=:
for d in $P:x; do echo "[$d]"; done'
oracle "palavra com expansão: literal ANTES também corta" \
  'P=0
IFS=:
for d in x:$P; do echo "[$d]"; done'
oracle "expansão CITADA não autoriza o corte do literal" \
  'P=0
IFS=:
for d in "$P":x; do echo "[$d]"; done'
oracle "aspas simples no meio da palavra expandida" \
  'P=0
IFS=:
for d in $P'"'"'a:b'"'"'; do echo "[$d]"; done'
oracle "escape no meio da palavra expandida" \
  'P=0
IFS=:
for d in ${P}a\:b; do echo "[$d]"; done'
oracle "expansão VAZIA ainda marca a palavra" \
  'E=
IFS=:
for d in $E:x; do echo "[$d]"; done'
oracle "palavra sem expansão ao lado de outra com" \
  'P=0
IFS=:
for d in a:b $P:c; do echo "[$d]"; done'

# ── 5. CAMPO VAZIO — delimitador é EXATO ───────────────────────────────
oracle "vazio-meio: a::b são três campos" \
  'IFS=:
v=a::b
for i in $v; do echo "[$i]"; done'
oracle "vazio-inicio: :a" \
  'IFS=:
v=:a
for i in $v; do echo "[$i]"; done'
oracle "vazio-fim: a: não gera campo final" \
  'IFS=:
v=a:
for i in $v; do echo "[$i]"; done
echo fim'
oracle "vazio-fim-duplo: a::" \
  'IFS=:
v=a::
for i in $v; do echo "[$i]"; done
echo fim'
oracle "vazio-só-separadores: :::" \
  'IFS=:
v=:::
for i in $v; do echo "[$i]"; done
echo fim'
oracle "vazio-um-separador: :" \
  'IFS=:
v=:
for i in $v; do echo "[$i]"; done
echo fim'

# ── 6. IFS COM VÁRIOS CARACTERES ───────────────────────────────────────
oracle "IFS=:, — dois separadores" \
  'v=a:b,c
IFS=:,
for d in $v; do echo "[$d]"; done'
oracle "IFS=abc — letras como separador" \
  'IFS=abc
v=xaybzc
for i in $v; do echo "[$i]"; done'
oracle "IFS com branco E não-branco: \": \"" \
  'IFS=": "
v="a : b"
for i in $v; do echo "[$i]"; done'
oracle "branco em volta do separador é ABSORVIDO" \
  'IFS=": "
v="a :: b"
for i in $v; do echo "[$i]"; done'

# ── 7. IFS VAZIO — nenhuma divisão ─────────────────────────────────────
oracle "IFS= (vazio): expansão não divide" \
  'IFS=
v="a b"
for i in $v; do echo "[$i]"; done'
oracle "IFS= (vazio): literal continua sendo duas palavras" \
  'IFS=
for i in a b; do echo "[$i]"; done'
oracle "IFS= (vazio) com separador antigo no valor" \
  'IFS=
v="a:b c"
for i in $v; do echo "[$i]"; done'

# ── 8. BRANCO: com espaço (padrão) e SEM espaço ────────────────────────
oracle "IFS padrão (ausente): corrida de branco colapsa" \
  'v="a  b   c"
for i in $v; do echo "[$i]"; done'
oracle "unset IFS explícito" \
  'unset IFS
v="a b"
for i in $v; do echo "[$i]"; done'
oracle "IFS=: — o espaço do VALOR é dado, não separador" \
  'IFS=:
v="a b:c"
for i in $v; do echo "[$i]"; done'
oracle "IFS=: — a TABULAÇÃO do valor é dado" \
  'IFS=:
v=$(printf "a\tb:c")
for i in $v; do echo "[$i]"; done'
oracle "IFS=: — a NOVA-LINHA do valor é dado" \
  'IFS=:
v=$(printf "a\nb:c")
for i in $v; do echo "[$i]"; done'
oracle "IFS=<TAB> — só a tabulação separa; o espaço não" \
  'v=$(printf "a\tb c")
IFS=$(printf "\t")
for i in $v; do echo "[$i]"; done'
oracle "IFS=<NL> — só a nova-linha separa" \
  'IFS=$(printf "\n")
v=$(printf "a b\nc d")
for i in $v; do echo "[$i]"; done'
oracle "separador seguido de espaço com IFS sem espaço" \
  'IFS=:
v="a: b"
for i in $v; do echo "[$i]"; done'

# ── 9. POSICIONAIS: $@ $* citados e não ────────────────────────────────
oracle 'ǀ"$@"ǀ preserva elementos e não divide' \
  'set -- "a b" c:d
IFS=:
for d in "$@"; do echo "[$d]"; done'
oracle 'ǀ$@ǀ sem aspas divide cada elemento' \
  'set -- "a:b" c
IFS=:
for d in $@; do echo "[$d]"; done'
oracle 'ǀ"$*"ǀ é UMA palavra unida pelo IFS' \
  'set -- a b
IFS=:
for d in "$*"; do echo "[$d]"; done'
oracle 'ǀ$*ǀ sem aspas divide' \
  'set -- a b
IFS=:
for d in $*; do echo "[$d]"; done'
oracle 'ǀ$1ǀ com literal colado' \
  'set -- 1
IFS=:
for d in $1:x; do echo "[$d]"; done'

# ── 10. `for` SEM `in` (JINV-078) — não pode regredir ──────────────────
oracle "for VAR; sem in percorre os posicionais" \
  'set -- "a:b" c
IFS=:
for d; do echo "[$d]"; done'
oracle "for VAR; sem in, lista vazia" \
  'set --
IFS=:
for d; do echo "[$d]"; done
echo fim'
oracle "for VAR; sem in, elemento com espaço" \
  'set -- "a b" c
IFS=:
for d; do echo "[$d]"; done'

# ── 11. SUBSTITUIÇÃO DE COMANDO, citada e não ──────────────────────────
oracle "\$(…) sem aspas divide no IFS" \
  'IFS=:
for d in $(echo a:b); do echo "[$d]"; done'
oracle "\$(…) CITADA não divide" \
  'IFS=:
for d in "$(echo a:b)"; do echo "[$d]"; done'
oracle "\$(…) sem aspas com literal colado" \
  'IFS=:
for d in $(echo 1):x; do echo "[$d]"; done'
oracle "\$(…) com branco na saída e IFS sem espaço" \
  'IFS=:
for d in $(printf "a b:c"); do echo "[$d]"; done'
oracle "crase sem aspas divide no IFS" \
  'IFS=:
for d in `echo a:b`; do echo "[$d]"; done'
oracle "RHS de atribuição NÃO é dividido (POSIX)" \
  'IFS=:
v=$(printf "a b\nc d")
echo "[$v]"'

# ── 12. GLOB NO MEIO DA LISTA ──────────────────────────────────────────
oracle "glob no meio, IFS=:" \
  'IFS=:
for f in x *.txt y; do echo "[$f]"; done'
oracle "glob vindo de variável, IFS=:" \
  'IFS=:
P="*.txt"
for f in $P; do echo "[$f]"; done'
oracle "glob que não casa nada continua literal" \
  'IFS=:
for f in nada*.zz; do echo "[$f]"; done'
oracle "glob depois de expansão na mesma palavra" \
  'D=sub
IFS=:
for f in $D/*.txt; do echo "[$f]"; done'

# ── 13. ARITMÉTICA E CHAVES ────────────────────────────────────────────
oracle "\$((…)) com literal colado" \
  'IFS=:
for d in $((1+1)):x; do echo "[$d]"; done'
oracle "expansão de chaves não é expansão de parâmetro" \
  'IFS=:
for d in {a:b,c}; do echo "[$d]"; done'
oracle "chaves numéricas continuam funcionando" \
  'IFS=:
for d in {1..3}; do echo "[$d]"; done'

# ── 14. ARRAYS — a mesma regra ─────────────────────────────────────────
oracle 'ǀ"${a[@]}"ǀ preserva o separador dentro do elemento' \
  'IFS=:
a=(x:y z)
for i in "${a[@]}"; do echo "[$i]"; done'
oracle 'ǀ${a[@]}ǀ sem aspas divide' \
  'IFS=:
a=(x:y z)
for i in ${a[@]}; do echo "[$i]"; done'
oracle 'ǀ${a[@]}ǀ sem aspas com literal colado' \
  'IFS=:
a=(1 2)
for i in ${a[@]}:x; do echo "[$i]"; done'
oracle 'ǀ"${a[@]}"ǀ com elemento vazio' \
  'a=(x "" y)
for i in "${a[@]}"; do echo "[$i]"; done'

# ── 15. A FAMÍLIA NOS VIZINHOS: case, atribuição, read ─────────────────
# Não compartilham o caminho da divisão do `for` (cada um tem o seu), e é por
# isso que estão aqui: se um dia forem unificados, estas asserções são a
# garantia de que a unificação não importa o defeito.
oracle "case: o padrão literal não é dividido" \
  'IFS=:
case a:b in a:b) echo SIM;; *) echo NAO;; esac'
oracle "case: o assunto vindo de variável não é dividido" \
  'IFS=:
v=a:b
case $v in a:b) echo SIM;; *) echo NAO;; esac'
oracle "atribuição: o RHS conserva o separador" \
  'IFS=:
P=a:b
Q=$P
echo "[$Q]"'
oracle "read: separa pelo IFS, campos exatos" \
  'IFS=:
echo "a:b:c" | { read x y z; echo "[$x][$y][$z]"; }'
oracle "read: campo vazio no meio" \
  'IFS=:
echo "a::c" | { read x y z; echo "[$x][$y][$z]"; }'

# ── 16. NÃO REGREDIR O CAMINHO COMUM (IFS padrão) ──────────────────────
# Com IFS branco, NADA deste conserto pode aparecer: nenhuma sentinela, nenhum
# campo vazio novo, nenhuma palavra a mais.
oracle "padrão: lista simples" \
  'for i in um dois tres; do echo "[$i]"; done'
oracle "padrão: variável com espaços" \
  'v="a b c"
for i in $v; do echo "[$i]"; done'
oracle "padrão: aspas preservam a palavra composta" \
  'for i in "a b" c; do echo "[$i]"; done'
oracle "padrão: espaços na borda do valor" \
  'v=" a b "
for i in $v; do echo "[$i]"; done'
oracle "padrão: valor vazio não produz iteração" \
  'v=
for i in $v; do echo "[$i]"; done
echo fim'
oracle "padrão: nome de arquivo com espaço vindo de glob (JINV-009)" \
  'touch "meu arquivo.txt"
n=0
for f in *.txt; do n=$((n+1)); done
echo "n=$n"
rm -f "meu arquivo.txt"'
oracle "padrão: corpo do laço com break/continue (JINV-073)" \
  'for i in 1 2 3 4; do
  if [ "$i" = 2 ]; then continue; fi
  if [ "$i" = 4 ]; then break; fi
  echo "[$i]"
done
echo fim'

# ── 17. O SENTINELA NÃO VAZA ───────────────────────────────────────────
# Os bytes de marca (0x1F campo, 0x0E/0x0F branco protegido, 0x11 palavra) são
# transitórios. Se algum sobreviver ao token, ele chega ao corpo do laço — e
# um `wc -c` acusa. Esta é a asserção que pega vazamento de sentinela.
oracle "nenhum byte de controle sobrevive ao token" \
  'IFS=:
v="a b:c"
for i in $v; do printf "%s" "$i" | /usr/bin/wc -c; done'
oracle "nenhum byte de controle no token com tab/nova-linha" \
  'IFS=:
v=$(printf "a\tb:c")
for i in $v; do printf "%s" "$i" | /usr/bin/od -An -c | /usr/bin/head -1; done'

echo ""
echo "=== JINV-087: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
