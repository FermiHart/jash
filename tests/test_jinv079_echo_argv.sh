#!/usr/bin/env bash
# JINV-079 — a saida do `echo` se monta do ARGV, nunca da linha crua.
#
# ═══ A PROPRIEDADE PROTEGIDA ═══
#
#   A saida do `echo` e a JUNCAO de argv[1..n] com UM espaco.
#   O branco ENTRE palavras nunca chega ao builtin: ele e estrutura, comida
#   pelo tokenizador. O branco DENTRO de uma palavra (citado ou escapado) e
#   dado, e sai inteiro — inclusive quando esta no comeco dela.
#
# Isto e uma propriedade sobre a FONTE do dado, nao uma lista de casos. Quem
# monta a saida da linha crua nao consegue satisfaze-la com nenhum cuidado
# local, porque na linha crua a informacao ja nao existe: depois da remocao de
# aspas, `echo "  x"` e `echo   x` sao literalmente a mesma sequencia de bytes.
# E por isso que o conserto e trocar a PORTA (string -> argv), e nao ajustar o
# corte de espacos.
#
# ═══ POR QUE ISTO IMPORTA ═══
#
# Era a maior causa isolada de divergencia do corpus de conformidade: 16 casos,
# mais que qualquer outra das 28 causas medidas (audits/PLANO-subir-na-matriz.md,
# causa C1). E `echo` aparece em 1034 das 1122 linhas do corpus — e o caminho
# mais quente do shell inteiro.
#
# Duas metades, uma espelho da outra:
#
#     echo " "        jash: `\n`        bash: ` \n`     <- o citado da ESQUERDA sumia
#     echo "  x  "    jash: `x  \n`     bash: `  x  \n`
#     echo A     B    jash: `A     B\n` bash: `A B\n`   <- o branco ENTRE nao colapsava
#
# ═══ PAR SIMETRICO DO JINV-063 ═══
#
# O JINV-063 matou o lado DIREITO (o separador do fim da linha) e deixou este
# de proposito, para nao misturar duas cirurgias no mesmo caminho quente. Por
# isso as duas familias sao afirmadas JUNTAS aqui, na secao (b): consertar o
# lado esquerdo reintroduzindo o direito nao seria conserto, seria troca de
# bug. Um gate que so cobrisse a metade nova aprovaria essa troca.
#
# ═══ AS DUAS PORTAS ═══
#
# `echo` tinha DUAS implementacoes que liam a linha crua, e as duas foram
# consertadas: o builtin (c/builtins/echo.c) e o atalho de `-c` sem estado
# (`ultra_fast_c_path` em c/main.c, que escrevia a cauda verbatim). O atalho
# agora DESISTE quando verbatim deixaria de ser igual a juncao de argv — nao
# colapsa por conta propria, que seria a quinta copia da semantica. As duas
# portas sao exercitadas aqui: as linhas de comando unico caem no atalho, as
# que tem `;` ou `|` caem no shell completo (secao (g)).
#
# Oraculo: /bin/bash, em TODA assercao. Nenhuma saida escrita a mao.
# Prova negativa: JASH_UNDER_TEST=<binario anterior ao conserto> -> VERMELHO
#                 (secoes (a) e (g); a secao (b) fica verde, que e o ponto).
#
# Origem: audits/PLANO-subir-na-matriz.md causa C1
# Codigo:  c/builtins/echo.c (jash_b_echo_argv), c/builtins/registry.c,
#          c/main.c (ultra_fast_c_path)
# Catraca: tests/argv_migrated.txt + tests/test_argv_ratchet.sh

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="${JASH_UNDER_TEST:-$PWD/build/jash}"
DL="$PWD/tools/deadline.sh"
BASH_ORACLE="/bin/bash"
PERL="/usr/bin/perl"
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0

[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
[ -x "$BASH_ORACLE" ] || { echo "SKIP: oraculo $BASH_ORACLE ausente"; exit 0; }
[ -x "$PERL" ] || { echo "SKIP: $PERL ausente"; exit 0; }
# `timeout(1)` NAO existe nesta maquina e `command -v timeout` MENTE quando
# quem pergunta e o jash (onde `timeout` e builtin). O cao de guarda canonico
# do repo e este, e ele distingue 124 (prazo) de 127 (ausente).
[ -x "$DL" ] || { echo "SKIP: $DL ausente"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM

# Areia identica para os dois shells: `echo *` tem que ter uma resposta so.
SBX="$T/sbx"
mkdir -p "$SBX"
: > "$SBX/a.txt"
: > "$SBX/two words.txt"

# Renderiza bytes crus de forma legivel (nova-linha, tab e nao-ASCII visiveis),
# para que a linha de FAIL mostre EXATAMENTE onde os espacos divergiram.
render() {
    "$PERL" -e '
        local $/; my $s = <STDIN>; $s = "" unless defined $s;
        $s =~ s/\\/\\\\/g; $s =~ s/\n/\\n/g; $s =~ s/\t/\\t/g;
        $s =~ s/([^\x20-\x7e])/sprintf("\\x%02x", ord($1))/ge;
        print $s;
    ' < "$1"
}

B=""; J=""; BRC=""; JRC=""

# stderr NUNCA entra na captura comparada: o jash emite avisos de UX por ali
# (nudge/adoption/brief) e fundi-los faria o veredito depender de um contador
# global — verde por sorte, nunca por merito (JINV-061, classe MERGED-EXACT).
# O rc vem da invocacao DIRETA, nunca do fim de um pipe (classe PIPE-RC).
run_both() {
    ( cd "$SBX" && "$BASH_ORACLE" -c "$1" ) > "$T/b.out" 2> "$T/b.err" < /dev/null
    BRC=$?
    ( cd "$SBX" && "$DL" 10 "$JASH" -c "$1" ) > "$T/j.out" 2> "$T/j.err" < /dev/null
    JRC=$?
    B=$(render "$T/b.out")
    J=$(render "$T/j.out")
}

# A assercao unica desta suite: byte a byte contra o bash, saida E status.
same() {
    local desc="$1" code="$2"
    run_both "$code"
    if [ "$J" = "$B" ] && [ "$JRC" = "$BRC" ]; then
        ok "$desc  ->  [$B]"
    else
        bad "$desc" "codigo: $code
        bash: [$B] rc=$BRC
        jash: [$J] rc=$JRC"
    fi
}

echo "── (a) o DEFEITO: branco citado a esquerda, e branco entre palavras ──"
same 'echo " " imprime o espaco citado'          'echo " "'
same 'echo "  x  " preserva os dois lados'       'echo "  x  "'
same 'echo "  leading" preserva o comeco'        'echo "  leading"'
same 'corrida entre palavras colapsa (5 -> 1)'   'echo A     B'
same 'corrida entre palavras colapsa (2 -> 1)'   'echo a  b'
same 'corrida no meio de tres palavras'          'echo a b   c'
same 'tab ENTRE palavras e separador'            'echo a	b'
same 'tab citado e DADO'                         'echo "	"'
same 'aspas vazias viram argumento vazio'        'echo "" ""'
same 'aspas simples vazias idem'                 "echo '' ''"
same 'duas palavras citadas, separador colapsa'  'echo "a b"  "c d"'
same 'palavras citadas de um char'               'echo "a"  "b"'
same 'branco interno citado sobrevive inteiro'   'echo "  a  b  "'
same 'branco dos dois lados, citado'             'echo "  both  "'
same '-n nao muda o branco citado da esquerda'   'echo -n "  x"; echo'

echo "── (b) o LADO DIREITO (JINV-063) — nao pode regredir ──"
same 'separador final da linha morre'            'echo x '
same 'espaco citado no fim e DADO'               'echo "x "'
same 'espaco ESCAPADO no fim e DADO'             'echo x\  '
same 'comentario no fim nao vira argumento'      'echo a #nota'
same 'espaco escapado no meio e DADO'            'echo a\  b'

echo "── (c) expansao: citada preserva, nao citada divide ──"
same 'nao citada: o branco vira separador'       'v="a  b"; echo $v'
same 'citada: o branco e dado'                   'v="a  b"; echo "$v"'
same 'nao citada dentro de colchetes'            'v="  x  "; echo [$v]'
same 'citada dentro de aspas'                    'v="  x  "; echo "$v"'
same 'IFS alterado divide por ele'               'IFS=:; v="a:b"; echo $v'
same 'IFS alterado nao afeta palavra literal'    'IFS=:; echo a  b'

echo "── (d) flags: -n, -e, -E, combinadas, e a desconhecida ──"
same 'echo sozinho'                              'echo'
same 'echo "" (um argumento vazio)'              'echo ""'
same '-n sozinho'                                'echo -n'
same '-n com argumento'                          'echo -n hi'
same '-n com argumento vazio'                    'echo -n ""'
same '-e interpreta \t'                          'echo -e "a\tb"'
same '-e interpreta \n'                          'echo -e "a\nb"'
same '-E deixa a barra literal'                  'echo -E "a\tb"'
same 'sem -e a barra e literal'                  'echo "a\tb"'
same '-ne combinadas'                            'echo -ne "a\tb"'
same '-en combinadas'                            'echo -en "a\nb"'
same '-eE: o E desliga o e (ordem importa)'      'echo -eE "a\tb"'
same '-Ee: o e desliga o E'                      'echo -Ee "a\tb"'
same 'duas flags separadas'                      'echo -n -e "a\tb"'
same 'flag DESCONHECIDA e operando'              'echo -x hi'
same 'letra ruim invalida a palavra INTEIRA'     'echo -nx hi'
same '"-" sozinho e operando'                    'echo -'
same '"--" nao e fim-de-flags'                   'echo -- a'
same 'flag citada continua sendo flag'           'echo "-n" hi'
same 'flag depois de operando e operando'        'echo - -n'

echo "── (e) dado que o -e nao pode transformar em travamento ──"
# Classe JINV-062 alcancavel por linha de usuario: uma barra invertida no FIM
# nao tem byte para escapar, e o laco de varredura parava nela sem avancar.
# `jash -c 'echo -e "\\"'` girava para sempre — sem mensagem, sem rc, sem
# rastro. Se voltar, `same` reprova por rc=124 (prazo estourado), nao por hang.
same 'barra invertida solta com -e nao trava'    'echo -e "\\"'
same 'barra invertida dupla com -e'              'echo -e "a\\\\b"'

echo "── (f) UTF-8: o byte alto nao e branco ──"
same 'acentos em duas palavras'                  'echo "aé" "bü"'
same 'acentos com corrida entre palavras'        'echo é     ü'
same 'acentos citados com branco interno'        'echo "é  ü"'

echo "── (g) as duas portas: atalho de -c e shell completo ──"
# A MESMA linha tem que dar a mesma coisa nas duas rotas. `echo a  b` sozinho
# cai no atalho sem estado de c/main.c; com `;`, `|` ou uma atribuicao antes,
# cai no shell completo e no builtin de argv. Divergir entre as duas rotas foi
# como o defeito sobreviveu ao primeiro conserto.
same 'atalho -c: comando unico'                  'echo a  b'
same 'shell completo: depois de ;'               'true; echo a  b'
same 'shell completo: com ; no fim'              'echo a  b;'
same 'shell completo: num pipe'                  'echo a  b | /bin/cat'
same 'shell completo: depois de atribuicao'      'x=1; echo a  b'
same 'shell completo: dentro de if'              'if true; then echo a  b; fi'
same 'shell completo: dentro de for'             'for i in 1; do echo a  b; done'
same 'substituicao de comando'                   'echo "$(echo a  b)"'

echo "── (h) redirecionamento canonico conserva o atalho -c ──"
# Com Sigil explicitamente desligado, sem envelope de agente e sem sintaxe que
# exija estado, `echo literal > arquivo` pode abrir/escrever diretamente. O
# JASH_BENCH só existe no caminho de inicializacao completa: se state_init
# aparecer, o redirect voltou a pagar ~350 KB de estado e perdeu o fast path.
fast_file="$SBX/fast-redir.txt"
fast_err="$T/fast-redir.err"
mkdir -p "$T/fast-home"
fast_jash() {
    env -i HOME="$T/fast-home" PATH="$PATH" TERM=dumb JASH_SIGIL=0 \
        JASH_BENCH=1 JASH_CHANGES=0 "$JASH" "$@"
}
( cd "$SBX" && fast_jash -c 'echo fast-redir > fast-redir.txt' \
    > /dev/null 2> "$fast_err" )
fast_rc=$?
fast_data=$(render "$fast_file")
if [ "$fast_rc" = 0 ] && [ "$fast_data" = 'fast-redir\n' ] &&
   ! grep -q 'state_init=' "$fast_err"; then
    ok 'echo canonico > arquivo usa fast path sem perder bytes'
else
    bad 'fast path de echo > arquivo' \
        "rc=$fast_rc data=[$fast_data] stderr=[$(render "$fast_err")]"
fi

fast_jash -c "echo append >> $fast_file" \
    > /dev/null 2> "$fast_err"
append_rc=$?
append_data=$(render "$fast_file")
if [ "$append_rc" = 0 ] &&
   [ "$append_data" = 'fast-redir\nappend\n' ] &&
   ! grep -q 'state_init=' "$fast_err"; then
    ok 'echo canonico >> arquivo preserva append no fast path'
else
    bad 'fast path de echo >> arquivo' \
        "rc=$append_rc data=[$append_data] stderr=[$(render "$fast_err")]"
fi

noncanonical="$SBX/noncanonical-redir.txt"
fast_jash -c \
    "echo A  B > $noncanonical" > /dev/null 2> "$fast_err"
noncanonical_rc=$?
noncanonical_data=$(render "$noncanonical")
if [ "$noncanonical_rc" = 0 ] && [ "$noncanonical_data" = 'A B\n' ] &&
   grep -q 'state_init=' "$fast_err"; then
    ok 'whitespace nao canonico cai no parser completo'
else
    bad 'fallback de echo nao canonico com redirect' \
        "rc=$noncanonical_rc data=[$noncanonical_data] stderr=[$(render "$fast_err")]"
fi

agent_file="$SBX/agent-redir.txt"
( cd "$SBX" && env -i HOME="$T/fast-home" PATH="$PATH" TERM=dumb \
    CLAUDECODE=1 JASH_SIGIL=0 JASH_BENCH=1 \
    "$JASH" -c 'echo observed > agent-redir.txt' \
    > /dev/null 2> "$fast_err" )
agent_rc=$?
agent_data=$(render "$agent_file")
if [ "$agent_rc" = 0 ] && [ "$agent_data" = 'observed\n' ] &&
   grep -q 'state_init=' "$fast_err" && grep -q 'files changed' "$fast_err"; then
    ok 'observabilidade de agente obriga caminho completo e reporta mudanca'
else
    bad 'fallback observado de echo com redirect' \
        "rc=$agent_rc data=[$agent_data] stderr=[$(render "$fast_err")]"
fi

agent_off_file="$SBX/agent-changes-off.txt"
( cd "$SBX" && env -i HOME="$T/fast-home" PATH="$PATH" TERM=dumb \
    CLAUDECODE=1 JASH_SIGIL=0 JASH_CHANGES=0 JASH_BENCH=1 \
    "$JASH" -c 'echo observed-off > agent-changes-off.txt' \
    > /dev/null 2> "$fast_err" )
agent_off_rc=$?
agent_off_data=$(render "$agent_off_file")
if [ "$agent_off_rc" = 0 ] && [ "$agent_off_data" = 'observed-off\n' ] &&
   grep -q 'state_init=' "$fast_err" && ! grep -q 'files changed' "$fast_err"; then
    ok 'JASH_CHANGES=0 desliga scan, nao a inicializacao do agente'
else
    bad 'fallback de agente com changes desligado' \
        "rc=$agent_off_rc data=[$agent_off_data] stderr=[$(render "$fast_err")]"
fi

session_home="$T/session-home"
session_dir="$SBX/session-cwd"
session_root_file="$SBX/session-relative.txt"
session_cwd_file="$session_dir/session-relative.txt"
mkdir -p "$session_home" "$session_dir"
( cd "$SBX" && env -i HOME="$session_home" PATH="$PATH" TERM=dumb \
    JASH_SIGIL=0 JASH_CHANGES=0 JASH_SESSION=redir-session \
    "$JASH" -c "cd $session_dir" > /dev/null 2> "$fast_err" )
( cd "$SBX" && env -i HOME="$session_home" PATH="$PATH" TERM=dumb \
    JASH_SIGIL=0 JASH_CHANGES=0 JASH_SESSION=redir-session JASH_BENCH=1 \
    "$JASH" -c 'echo session > session-relative.txt' \
    > /dev/null 2> "$fast_err" )
session_rc=$?
session_data=$(render "$session_cwd_file")
if [ "$session_rc" = 0 ] && [ "$session_data" = 'session\n' ] &&
   [ ! -e "$session_root_file" ] && grep -q 'state_init=' "$fast_err"; then
    ok 'sessao restaura cwd antes de abrir redirect relativo'
else
    bad 'redirect relativo com cwd persistido' \
        "rc=$session_rc data=[$session_data] root=$([ -e "$session_root_file" ] && printf yes || printf no) stderr=[$(render "$fast_err")]"
fi

fast_jash -c \
    "echo denied > $SBX/missing/child" > /dev/null 2> "$fast_err"
missing_rc=$?
if [ "$missing_rc" = 1 ] && grep -q "$SBX/missing/child" "$fast_err" &&
   ! grep -q 'state_init=' "$fast_err" && [ ! -e "$SBX/missing/child" ]; then
    ok 'falha de open no fast path retorna 1 sem criar destino'
else
    bad 'falha de open no fast path' \
        "rc=$missing_rc stderr=[$(render "$fast_err")]"
fi

noclobber="$SBX/noclobber-redir.txt"
printf 'original\n' > "$noclobber"
fast_jash -C -c \
    "echo replacement > $noclobber" > /dev/null 2> "$fast_err"
noclobber_rc=$?
noclobber_data=$(render "$noclobber")
if [ "$noclobber_rc" = 1 ] && [ "$noclobber_data" = 'original\n' ] &&
   grep -q 'state_init=' "$fast_err"; then
    ok 'noclobber recusa overwrite pelo parser completo'
else
    bad 'fallback noclobber de echo com redirect' \
        "rc=$noclobber_rc data=[$noclobber_data] stderr=[$(render "$fast_err")]"
fi

echo "── (i) glob: o espaco do NOME do arquivo e dado ──"
same 'nome com espaco nao vira duas palavras'    'echo *.txt'

echo "── (j) continuacao de linha indentada (herdado do JINV-071) ──"
# O cabecalho do tests/test_jinv071_block_continuation.sh registrou esta
# divergencia como "outra raiz, em outro arquivo": a juncao ja funcionava, o
# que faltava era o `echo` colapsar o branco da linha seguinte. E esta raiz.
same 'continuacao indentada no topo'             'echo A \
   B'
same 'continuacao indentada dentro de bloco'     'if true; then
echo A \
   B
fi'

echo
echo "=== JINV-079: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
