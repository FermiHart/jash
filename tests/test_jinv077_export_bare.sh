#!/usr/bin/env bash
# JINV-077 — `export NOME` (sem `=valor`) e a forma POSIX, nao um erro.
#
# ═══ A PROPRIEDADE PROTEGIDA ═══
#
#   `export nome...` marca uma variavel JA EXISTENTE para exportacao. Nao
#   exige `=valor`. Um shell que recusa essa forma nao roda script real:
#   e o idioma de duas linhas que todo mundo escreve,
#
#       CC=gcc
#       export CC
#
#   e que autoconf emite dezenas de vezes por `configure`.
#
# ═══ O DEFEITO ═══
#
#     $ jash -c 'FOO=bar; export FOO; echo rc=$?'
#     export: usage: export KEY=VALUE          <- stderr
#     rc=2
#
#     $ bash -c 'FOO=bar; export FOO; echo rc=$?'
#     rc=0
#
#   Duas coisas erradas de uma vez: barulho no stderr e rc 2 num comando que
#   nao falhou. Numa cadeia `export X && ...` ou sob `set -e`, o rc 2 mata o
#   script — e o barulho polui o stderr de tudo que corre em cima.
#
#   `audits/BENCHMARK-scripts-reais.md` mediu 19 `configure` de autoconf,
#   38 invocacoes (`--help`/`--version`). **22 delas tinham este texto como
#   PRIMEIRO erro** — o bloqueador dominante, sozinho.
#
# ═══ POR QUE O NO-OP E A SEMANTICA CERTA (e nao gambiarra) ═══
#
#   A tabela de env do jash e plana: toda variavel ja e a environment do
#   filho (`V=1; sh -c 'echo $V'` imprime `1`). Logo `export NOME` de uma
#   variavel existente nao tem trabalho a fazer. E de uma que NAO existe
#   tambem nao: o bash tampouco a cria (asercao 4 abaixo prova isso contra o
#   oraculo). O no-op coincide com o bash nos dois casos; o que muda e o
#   contrato observavel — rc 0 e stderr limpo.
#
#   O nome continua VALIDADO (`export 1bad` -> `not a valid identifier`,
#   rc 1). Trocar um erro barulhento por uma mentira silenciosa seria o
#   defeito da familia JINV-045/069, nao o conserto dele.
#
# ═══ DIVERGENCIA CONHECIDA, DECLARADA AQUI PARA NAO SER DESCOBERTA DEPOIS ═══
#
#   `export -n A` no bash MANTEM o valor da variavel (so tira do ambiente do
#   filho). O jash, sem bit de exportacao por entrada, remove. Este gate
#   afirma so o que o jash sustenta — rc, stderr e ausencia no ambiente do
#   filho — e NAO afirma retencao de valor. Um gate que afirmasse isso
#   ficaria vermelho de proposito, e um gate vermelho de proposito e um gate
#   que ninguem le.
#
# PROVA NEGATIVA: JASH_UNDER_TEST=<binario sem o conserto> deixa este gate
# VERMELHO (JINV-061). Use JASH_UNDER_TEST, NUNCA JASH_BIN — o proprio jash
# exporta JASH_BIN apontando para o binario real, e um gate que lesse
# JASH_BIN passaria verde contra qualquer mutante.
#
# ORACULO: /bin/bash em toda asercao.
# Gate: bash tests/test_jinv077_export_bare.sh

set -u
cd "$(dirname "$0")/.." || exit 2

JASH="${JASH_UNDER_TEST:-$PWD/build/jash}"
DL="$PWD/tools/deadline.sh"
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0 JASH_MEMO=0
# O ambiente do agente pode ter ativado o agent-guard strict; este gate compara
# jash com /bin/bash e precisa que chamadas legitimas a /bin/sh -c nao sejam
# bloqueadas.
export JASH_GUARD_BYPASS=0 JASH_GUARD_BYPASS_STRICT=0
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
[ -x "$DL" ]   || { echo "FAIL: $DL ausente (cao de guarda obrigatorio)"; exit 1; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

SB=$(mktemp -d) || exit 2
trap 'rm -rf "$SB"' EXIT INT TERM

# `deadline.sh` e nao `timeout`: timeout(1) NAO EXISTE nesta maquina e sob
# bash devolve 127, fazendo o teste concluir "nao travou" quando travou.
jout() { (cd "$SB" && "$DL" 20 "$JASH"   -c "$1" 2>/dev/null); }
bout() { (cd "$SB" && "$DL" 20 /bin/bash -c "$1" 2>/dev/null); }
jerr() { (cd "$SB" && "$DL" 20 "$JASH"   -c "$1" 2>&1 >/dev/null); }
berr() { (cd "$SB" && "$DL" 20 /bin/bash -c "$1" 2>&1 >/dev/null); }
jrc()  { (cd "$SB" && "$DL" 20 "$JASH"   -c "$1" >/dev/null 2>&1); printf '%s' "$?"; }
brc()  { (cd "$SB" && "$DL" 20 /bin/bash -c "$1" >/dev/null 2>&1); printf '%s' "$?"; }

# stdout + rc contra o oraculo. stderr entra so onde a propriedade E o stderr
# (asercao 2), porque o TEXTO de um diagnostico nunca foi contrato.
oracle() {
    local desc="$1" code="$2" jo bo jr br
    jo=$(jout "$code"); jr=$?
    bo=$(bout "$code"); br=$?
    if [ "$jr" = "124" ]; then
        bad "$desc" "TRAVOU (deadline 20s): $code"
    elif [ "$jo" = "$bo" ] && [ "$jr" = "$br" ]; then
        ok "$desc"
    else
        bad "$desc" "jash rc=$jr out=[$jo] | bash rc=$br out=[$bo] | $code"
    fi
}

# So o rc. Existe para os casos em que o STDOUT do bash e a listagem do
# ambiente (que difere por construcao entre os dois processos) e a
# propriedade em disputa e o codigo de saida.
oracle_rc() {
    local desc="$1" code="$2" jr br
    jr=$(jrc "$code"); br=$(brc "$code")
    if [ "$jr" = "124" ]; then
        bad "$desc" "TRAVOU (deadline 20s): $code"
    elif [ "$jr" = "$br" ]; then
        ok "$desc (rc=$jr nos dois)"
    else
        bad "$desc" "rc jash=$jr | bash=$br | $code"
    fi
}

# "os dois calam" / "os dois falam" — compara a PRESENCA de diagnostico, nao
# o texto dele.
oracle_stderr_shape() {
    local desc="$1" code="$2" je be jn bn
    je=$(jerr "$code"); be=$(berr "$code")
    [ -n "$je" ] && jn=fala || jn=cala
    [ -n "$be" ] && bn=fala || bn=cala
    if [ "$jn" = "$bn" ]; then
        ok "$desc (ambos $jn)"
    else
        bad "$desc" "jash $jn [$(printf '%s' "$je" | head -c 90)] | bash $bn [$(printf '%s' "$be" | head -c 90)] | $code"
    fi
}

echo "── (1) o caso que matava 22 das 38 invocacoes de configure ──────────"
oracle "export NOME de variavel existente"   'FOO=bar; export FOO; echo rc=$?'
oracle "o valor sobrevive ao export nu"      'FOO=bar; export FOO; echo "[$FOO]"'
oracle "export nu numa cadeia &&"            'FOO=bar; export FOO && echo chegou'
oracle "o idioma de duas linhas, em script"  'CC=gcc
export CC
echo "[$CC]"'

echo
echo "── (2) o stderr fica LIMPO — a propriedade e o silencio ─────────────"
oracle_stderr_shape "export NOME nao escreve no stderr" 'FOO=bar; export FOO'
oracle_stderr_shape "export A B C nao escreve no stderr" 'A=1;B=2;C=3; export A B C'
# e o texto especifico da recusa antiga nao pode reaparecer em canal nenhum
out=$( (cd "$SB" && "$DL" 20 "$JASH" -c 'FOO=bar; export FOO' 2>&1) )
if printf '%s' "$out" | grep -q 'usage: export'; then
    bad "a recusa antiga sumiu" "ainda aparece: [$(printf '%s' "$out" | head -c 80)]"
else
    ok "a recusa antiga ('usage: export KEY=VALUE') nao aparece mais"
fi

echo
echo "── (3) varios nomes, e a mistura com atribuicao ─────────────────────"
oracle "export A B C (varios nomes nus)"     'A=1;B=2;C=3; export A B C; echo "$A$B$C rc=$?"'
oracle "export VAR=x OUTRA (misturado)"      'OUTRA=z; export VAR=x OUTRA; echo "$VAR/$OUTRA rc=$?"'
oracle "export OUTRA VAR=x (ordem inversa)"  'OUTRA=z; export OUTRA VAR=x; echo "$VAR/$OUTRA rc=$?"'
oracle "export A=1 B C=3 D"                  'B=2;D=4; export A=1 B C=3 D; echo "$A$B$C$D rc=$?"'

echo
echo "── (4) variavel INEXISTENTE: o bash tambem nao cria ─────────────────"
oracle "rc de export de nome inexistente"    'export NOPE_XYZ_1; echo rc=$?'
oracle "nao passa a existir"                 'export NOPE_XYZ_1; echo "[${NOPE_XYZ_1-unset}]"'
oracle "e nao chega ao ambiente do filho"    'export NOPE_XYZ_1; /bin/sh -c '"'"'echo "[${NOPE_XYZ_1-unset}]"'"'"''
oracle_stderr_shape "silencio tambem no inexistente" 'export NOPE_XYZ_1'

echo
echo "── (5) a exportacao e REAL: o filho enxerga ─────────────────────────"
oracle "filho ve a variavel exportada nua"   'MARCA=presente; export MARCA; /bin/sh -c '"'"'echo child=$MARCA'"'"''
oracle "filho ve as tres de export A B C"    'A=1;B=2;C=3; export A B C; /bin/sh -c '"'"'echo child=$A$B$C'"'"''

echo
echo "── (6) nome invalido continua sendo ERRO (nao virou silencio) ───────"
oracle "export 1bad"       'export 1bad; echo rc=$?'
oracle "export a-b"        'export a-b; echo rc=$?'
oracle_stderr_shape "nome invalido fala no stderr" 'export 1bad'
# o rc 0 do bash em `export ok` versus rc 1 no invalido: a distincao existe
jr_ok=$(jrc 'V=1; export V'); jr_bad=$(jrc 'export 1bad')
br_ok=$(brc 'V=1; export V'); br_bad=$(brc 'export 1bad')
if [ "$jr_ok" = "$br_ok" ] && [ "$jr_bad" = "$br_bad" ] && [ "$jr_ok" != "$jr_bad" ]; then
    ok "rc distingue nome valido ($jr_ok) de invalido ($jr_bad), como o bash"
else
    bad "rc valido vs invalido" "jash ok=$jr_ok bad=$jr_bad | bash ok=$br_ok bad=$br_bad"
fi

echo
echo "── (7) formas vizinhas nao regrediram ───────────────────────────────"
oracle "export KEY=VALUE simples"        'export K=v; echo "[$K] rc=$?"'
oracle "export com aspas e espaco"       'export K="a b"; echo "[$K]"'
oracle "export com aspas simples"        "export K='a b'; echo \"[\$K]\""
oracle "export com expansao"             'H=zz; export K=$H/bin; echo "[$K]"'
oracle "export vazio KEY="               'export K=; echo "[${K-unset}]"'
# `export` sozinho e `export -p` listam: o contrato aqui e "rc 0 e produz
# linhas", nao paridade byte-a-byte com o bash (os ambientes diferem).
jr=$(jrc 'export'); br=$(brc 'export')
n=$(jout 'export' | wc -l | tr -d ' ')
if [ "$jr" = "$br" ] && [ "$n" -gt 0 ]; then
    ok "export sozinho lista (rc=$jr, $n linhas)"
else
    bad "export sozinho" "rc jash=$jr bash=$br, linhas=$n"
fi
jr=$(jrc 'export -p'); br=$(brc 'export -p')
n=$(jout 'export -p' | grep -c '^export ')
if [ "$jr" = "$br" ] && [ "$n" -gt 0 ]; then
    ok "export -p emite linhas reusaveis 'export K=...' (rc=$jr, $n linhas)"
else
    bad "export -p" "rc jash=$jr bash=$br, linhas 'export '=$n"
fi

echo
echo "── (8) export -n: rc e silencio como no bash ────────────────────────"
oracle "export -n de nome NUNCA exportado"  'export -n NUNCA_XYZ; echo rc=$?'
oracle_stderr_shape "e sem reclamar"        'export -n NUNCA_XYZ'
oracle "export -n com varios nomes"         'A=1;B=2; export A B; export -n A B; echo rc=$?'
oracle_stderr_shape "varios nomes, silencio" 'A=1;B=2; export A B; export -n A B'
# `export -n` sem operando: o bash 3.2 aproveita e LISTA o ambiente (extensao
# dele; o POSIX manda listar em `export -p`). O stdout, portanto, nao pode ser
# comparado — sao dois ambientes diferentes. O que estava quebrado, e o que
# esta afirmado aqui, e o rc: antes o `-n` nu caia no laco de atribuicoes,
# virava a "chave" `-n` e saia com `usage` + rc 2.
oracle_rc "export -n sem operando"          'export -n'
# a propriedade que o jash SUSTENTA: depois do -n a variavel nao chega ao filho.
child=$(jout 'A=1; export A; export -n A; /bin/sh -c '"'"'echo "[${A-ausente}]"'"'"'')
if [ "$child" = "[ausente]" ] || [ "$child" = "[]" ]; then
    ok "apos export -n a variavel nao chega ao ambiente do filho (veio $child)"
else
    bad "export -n nao desexporta" "o filho ainda ve: $child"
fi

echo
echo "── (9) nome maior que a tabela recusa ALTO — nunca grava chave errada ─"
LONGN=$(printf 'A%.0s' $(seq 1 80))
rc=$(jrc "export ${LONGN}=v")
out=$(jout "export ${LONGN}=v; export -p" | grep -c "^export ${LONGN}=" || true)
if [ "$rc" != "0" ] && [ "$out" = "0" ]; then
    ok "nome de 80 chars recusado alto (rc=$rc), sem gravar chave truncada"
else
    bad "nome longo" "rc=$rc e $out linhas gravadas — truncar em silencio e pior que recusar"
fi

printf '\n=== JINV-077: %d ok / %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
