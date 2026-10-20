#!/usr/bin/env bash
# JINV-084 — `{ … }` é um COMANDO COMPOSTO, não dois comandos chamados `{` e `}`.
#
# ═══ A propriedade ═══════════════════════════════════════════════════════════
#
# Um grupo de chaves é UM enunciado. Isso tem que valer em toda porta por onde
# um enunciado passa — o leitor de script (que decide quando a linha acabou), o
# avaliador (que decide onde o grupo fecha) e os divisores de corpo de bloco
# (que decidem onde cortar em `;`/nova-linha). Bastava UMA dessas portas não
# saber o que é `{` para o grupo virar dois comandos inexistentes:
#
#     {                    bash -> hi     jash -> `{: command not found`
#     echo hi                                     `hi`
#     }                                           `}: command not found`
#
# E o inverso é a metade que ninguém lembra: `{` e `}` só são estrutura quando
# são palavra reservada. `echo }`, `echo "{"`, `case x in }) …`, `{a,b}` e
# `${VAR}` são DADO, e continuar sendo dado é parte da propriedade — é a
# família dos JINV-016/017/018/019/072/083, dado virando estrutura por leitura
# cega de texto.
#
# ═══ O que este gate cobre ═══════════════════════════════════════════════════
#
#  1. o grupo roda: uma linha e multi-linha
#  2. a CAUDA depois do `}` roda: `;`, `&&`, `||`, pipe, e a cadeia `&& … || …`
#  3. redirecionamento de bloco (`{ …; } > f`, `{ …; } < f`)
#  4. aninhado em if / for / while / função / outro grupo
#  5. `{` e `}` como DADO continuam dado
#  6. o que NÃO é grupo continua não sendo: `{echo`, `{a,b}`, `${VAR}`
#  7. não TRAVA — `{ … }` numa linha só dentro de laço multi-linha girava para
#     sempre num laço que não avançava o ponteiro (o mais banal dos casos)
#
# ═══ O oráculo ═══════════════════════════════════════════════════════════════
#
# TODA asserção compara com `/bin/bash` rodando o MESMO arquivo. Nada aqui
# afirma "o certo é X" por conta própria — o certo é o que o bash faz. As
# comparações são de STDOUT + rc; o stderr fica de fora de propósito, porque o
# jash acrescenta sugestões ("Did you mean …?") que são melhoria deliberada e
# não podem prender a evolução da prosa de erro.
#
# ═══ Divergências REGISTRADAS (não consertadas nesta onda) ═══════════════════
#
#   `{ ; }`     bash: erro de sintaxe (rc=2)   jash: aceita, rc=0
#               O jash não tem validador de sintaxe de grupo vazio. Aceitar
#               demais é menos grave que executar outra coisa, então fica
#               registrado — e a asserção 7.2 garante ao menos que não trava.
#   `}` órfã    bash: erro de sintaxe (rc=2)   jash: `}: command not found` (127)
#               Os dois RECUSAM e os dois com rc≠0; só o diagnóstico difere.
#               A asserção 7.1 trava essa propriedade mais fraca (rc≠0 nos
#               dois), que é a que importa: `}` sozinha nunca pode ser sucesso.
#
# ═══ Cão de guarda ═══════════════════════════════════════════════════════════
#
# `tools/deadline.sh`, NUNCA `timeout(1)` — ele não existe nesta máquina e
# devolve 127 sob bash, o que faz um teste de travamento concluir "não travou"
# justamente quando travou (`command -v timeout` também mente: no jash é
# builtin). Dois dos defeitos que este gate protege eram TRAVAMENTOS.

set -u
cd "$(dirname "$0")/.." || exit 2

# JASH_UNDER_TEST, NUNCA JASH_BIN: o próprio jash exporta `JASH_BIN` para os
# processos filhos, então um gate que lesse JASH_BIN passaria verde contra
# QUALQUER binário mutante — a prova negativa ficaria inerte sem avisar.
JASH="${JASH_UNDER_TEST:-$PWD/build/jash}"
BASH_ORACLE=/bin/bash
DL="$PWD/tools/deadline.sh"
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0
# A memoização do jash devolve resultado ANTIGO para comando repetido — já
# produziu um "IDÊNTICOS" falso comparando 133 bytes com 0. Toda medição desliga.
export JASH_MEMO=0

[ -x "$JASH" ]         || { echo "SKIP: $JASH ausente"; exit 0; }
[ -x "$BASH_ORACLE" ]  || { echo "SKIP: $BASH_ORACLE ausente (sem oráculo)"; exit 0; }
[ -x "$DL" ]           || { echo "SKIP: $DL ausente (sem cão de guarda)"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM

# Roda um programa num arquivo e devolve "STDOUT<US>rc". stderr descartado
# (ver "O oráculo", acima). O prazo protege TODA invocação, inclusive as do
# oráculo — o preparo é que costuma carregar a entrada patológica.
run_with() {
    local sh="$1" prog="$2" out rc
    printf '%s' "$prog" > "$T/p.sh"
    out=$( (cd "$T" && "$DL" 10 "$sh" "$T/p.sh" 2>/dev/null) ); rc=$?
    printf '%s\037%s' "$out" "$rc"
}

# Asserção principal: jash e bash produzem o MESMO stdout e o MESMO rc.
parity() {
    local label="$1" prog="$2" b j
    b=$(run_with "$BASH_ORACLE" "$prog")
    j=$(run_with "$JASH"        "$prog")
    case "$j" in
        *$'\037'124) bad "$label" "jash TRAVOU (deadline 124)"; return ;;
    esac
    if [ "$b" = "$j" ]; then
        ok "$label"
    else
        bad "$label" "bash=[$(printf '%s' "$b" | tr '\n\037' '~|')]  jash=[$(printf '%s' "$j" | tr '\n\037' '~|')]"
    fi
}

# Asserção fraca: stdout igual ao do bash e AMBOS recusam (rc≠0). Para as
# formas em que os dois rejeitam com diagnósticos diferentes.
both_reject() {
    local label="$1" prog="$2" b j bo br jo jr
    b=$(run_with "$BASH_ORACLE" "$prog"); bo=${b%$'\037'*}; br=${b##*$'\037'}
    j=$(run_with "$JASH"        "$prog"); jo=${j%$'\037'*}; jr=${j##*$'\037'}
    if [ "$jr" = "124" ]; then bad "$label" "jash TRAVOU (deadline 124)"; return; fi
    if [ "$bo" = "$jo" ] && [ "$br" != "0" ] && [ "$jr" != "0" ]; then
        ok "$label (ambos recusam: bash rc=$br, jash rc=$jr)"
    else
        bad "$label" "bash=[$bo|$br] jash=[$jo|$jr] — o esperado é mesmo stdout e rc≠0 nos dois"
    fi
}

# ── 1. o grupo roda ─────────────────────────────────────────────────────────
parity "1.1 grupo numa linha"            '{ echo hi; }
'
parity "1.2 grupo multi-linha"           '{
echo hi
}
echo tail
'
parity "1.3 multi-linha, 3 enunciados"   '{
echo one
echo two
echo three
}
'
parity "1.4 escopo compartilhado (1 linha)" '{ X=2; }
echo $X
'
parity "1.5 escopo compartilhado (multi)"   '{
X=3
}
echo $X
'

# ── 2. a CAUDA depois do `}` ────────────────────────────────────────────────
# Era descartada em silêncio para tudo que não fosse `;`/nova-linha.
parity "2.1 cauda com ;"                 '{ echo a; }; echo b
'
parity "2.2 cauda com &&"                '{ echo a; } && echo b
'
parity "2.3 cauda com || (grupo falha)"  '{ false; } || echo b
'
parity "2.4 cauda com && (grupo falha)"  '{ false; } && echo nao
echo fim
'
# A cadeia AND-OR é associativa à esquerda — um par binário não a resolve.
parity "2.5 cadeia && … ||"              '{ true; } && echo y || echo n
'
parity "2.6 cadeia && … || (falso)"      '{ false; } && echo y || echo n
'
parity "2.7 cauda com pipe"              '{ echo a; } | tr a-z A-Z
'
parity "2.8 cauda multi-linha com &&"    '{
echo a
} && echo b
'
parity "2.9 dois grupos seguidos"        '{ echo a; }
{ echo b; }
'

# ── 3. redirecionamento de bloco ────────────────────────────────────────────
parity "3.1 { …; } > arquivo"            '{ echo a; echo b; } > out.txt
cat out.txt
'
parity "3.2 { …; } < arquivo"            'printf "L1\nL2\n" > in.txt
{ read -r a; echo "got:$a"; } < in.txt
'
parity "3.3 { …; } >> arquivo"           'echo base > out.txt
{ echo mais; } >> out.txt
cat out.txt
'

# ── 4. aninhamento ──────────────────────────────────────────────────────────
parity "4.1 grupo dentro de if"          'if true; then
{ echo in; }
fi
'
parity "4.2 grupo dentro de if/else"     'if false; then
{ echo nao; }
else
{ echo sim; }
fi
'
# 4.3 e 4.4 TRAVAVAM o shell para sempre (laço de redirecionamento que não
# avançava o ponteiro na nova-linha logo depois do `}`).
parity "4.3 grupo dentro de for"         'for i in 1 2; do
{ echo v$i; }
done
'
parity "4.4 grupo dentro de while"       'i=0
while [ $i -lt 2 ]; do
{ echo w$i; }
i=$((i+1))
done
'
parity "4.5 grupo dentro de until"       'i=0
until [ $i -ge 2 ]; do
{ echo u$i; }
i=$((i+1))
done
'
parity "4.6 grupo dentro de função"      'f() {
{ echo deep; }
}
f
'
parity "4.7 grupo dentro de grupo"       '{ { echo inner; }; echo outer; }
'
parity "4.8 grupo aninhado multi-linha"  '{
{
echo dentro
}
echo fora
}
'
parity "4.9 grupo com return em função"  'g() { { echo a; return 0; }; echo never; }
g
echo rc=$?
'

# ── 5. `{` e `}` como DADO ──────────────────────────────────────────────────
# A metade que a leitura cega sempre atropela.
parity "5.1 echo \"{\""                  'echo "{"
'
parity "5.2 echo }"                      'echo }
'
parity "5.3 echo \"}\""                  'echo "}"
'
parity "5.4 chaves citadas DENTRO do grupo" '{ echo "{"; echo "}"; }
'
# `}` em posição de ARGUMENTO não pode fechar o grupo que o contém.
parity "5.5 } como argumento no grupo"   '{ echo fim }; }
'
# O padrão `})` do `case` é o idioma que autoconf usa; `}` ali é DADO.
parity "5.6 } como padrão de case"       'case x in }) echo br;; x) echo ex;; esac
'
parity "5.7 case com } dentro de grupo"  '{ case x in }) echo br;; x) echo ex;; esac; }
'

# ── 6. o que NÃO é grupo continua não sendo ─────────────────────────────────
# POSIX 2.9: `{` só é palavra reservada seguida de BRANCO.
parity "6.1 {echo é um comando, não grupo" '{echo hi
'
parity "6.2 {a,b} é expansão de chaves"  'echo {a,b}
'
parity "6.3 \${VAR} sobrevive no grupo"  'V=7
{ echo "${V}"; }
'
parity "6.4 \${VAR} sem aspas no grupo"  'V=8
{ echo ${V}; }
'
parity "6.5 expansão de chaves no grupo" '{ echo x{1,2}y; }
'
parity "6.6 grupo dentro de \$( )"       'echo "$( { echo q; } )"
'

# ── 7. as duas divergências REGISTRADAS ─────────────────────────────────────
# Não são parity: o bash recusa por sintaxe, o jash recusa por outro caminho
# (ou aceita). O que o gate trava é a propriedade que NÃO pode cair.
both_reject "7.1 } órfã é recusada"      'echo pre
}
'
# `{ ; }`: o bash recusa, o jash aceita (registrado no cabeçalho). O que não
# pode acontecer é TRAVAR — e é só isso que se assere aqui.
{
    r=$(run_with "$JASH" '{ ; }
echo depois
')
    if [ "${r##*$'\037'}" = "124" ]; then
        bad "7.2 { ; } não trava" "jash TRAVOU no grupo vazio (deadline 124)"
    else
        ok "7.2 { ; } não trava (divergência de sintaxe registrada no cabeçalho)"
    fi
}

# ── 8. o idioma do autoconf que motivou a onda ──────────────────────────────
# `as_fn_*` escreve o corpo como grupo multi-linha e chama logo em seguida.
# Era aqui que 5 dos 10 `configure` travados imprimiam `}: command not found`.
parity "8.1 as_fn_ estilo autoconf"      'as_fn_set_status ()
{
  return $1
}
as_fn_set_status 0 && echo zero
as_fn_set_status 1 || echo one
'
parity "8.2 grupo + cauda em função"     'as_fn_error ()
{
  { echo "erro: $1"; } >&2
  return 1
}
as_fn_error boom 2>/dev/null || echo capturado
'

# ── 9. break/continue DENTRO do grupo ───────────────────────────────────────
# O grupo compartilha o escopo do laço, então um `break` lá dentro tem que
# quebrar o laço de fora. A cauda do `}` rodava por cima do pedido em voo e o
# rc 0 dela APAGAVA a sentinela — o laço seguia como se nada tivesse sido
# pedido, e o `break` virava enfeite. É a guarda que o `if` (JINV-075) e o
# `case` já tinham e o grupo não.
parity "9.1 break dentro do grupo"       'for i in 1 2 3; do { break; }; echo x$i; done
echo end
'
parity "9.2 continue dentro do grupo"    'for i in 1 2 3; do { continue; }; echo x$i; done
echo end
'
parity "9.3 break condicional no grupo"  'for i in 1 2 3; do { [ $i = 2 ] && break; }; echo x$i; done
echo end
'
parity "9.4 break no grupo multi-linha"  'for i in 1 2 3; do
{ break; }
echo x$i
done
echo end
'
parity "9.5 break em while, no grupo"    'i=0
while [ $i -lt 5 ]; do
i=$((i+1))
{ [ $i = 2 ] && break; }
echo w$i
done
echo end
'
parity "9.6 laço aninhado com grupo"     'for a in 1 2; do for b in 1 2; do { echo $a$b; }; done; done
echo end
'

# ── 10. o acumulador e o avaliador têm que concordar ────────────────────────
# O leitor de script decide onde o grupo ACABA para montar o enunciado; o
# avaliador decide de novo, depois. Se os dois usarem réguas diferentes, o
# enunciado montado não é o que será lido. 10.1 é o caso que provou isso: um
# `{` que abre no MEIO da linha (`f() {`) não era contado, mas o `}` que o
# fecha começa a linha e era — contagem assimétrica, grupo cortado no meio,
# função ficando indefinida. Por isso os dois lados perguntam ao MESMO
# `jash_find_brace_close`.
parity "10.1 função definida dentro do grupo" '{
f() {
  echo x
}
f
echo y
}
echo z
'
parity "10.2 grupo dentro de grupo (multi)"   '{
{
echo inner
}
echo outer
}
echo z
'
parity "10.3 if dentro do grupo (multi)"      '{
if true; then
echo t
fi
echo after
}
echo z
'
parity "10.4 for dentro do grupo (multi)"     '{
for i in 1 2; do
echo i$i
done
echo after
}
echo z
'
parity "10.5 case dentro do grupo (multi)"    '{
case a in
a) echo ca;;
esac
echo after
}
echo z
'
parity "10.6 } como DADO no grupo (multi)"    '{
echo }
echo ok
}
echo z
'
parity "10.7 três grupos aninhados"           '{
{
{
echo deep
}
}
}
echo z
'
parity "10.8 heredoc dentro do grupo"         '{
cat <<EOF
hd line
EOF
echo after
}
echo z
'

echo ""
echo "=== JINV-084: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
