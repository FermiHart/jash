#!/usr/bin/env bash
# JINV-040 — o til expande dentro de uma atribuição.
#
# POSIX 2.6.1: numa atribuição de variável o til é expandido depois do `=` e
# depois de cada `:` não citado. É como todo mundo escreve caminho:
#
#   export PATH=~/bin:$PATH
#   export TOOL=~/opt/tool
#
# `jash_expand_tilde` só considerava o til elegível em INÍCIO DE PALAVRA
# (depois de espaço/tab), e num `V=~` ele vem depois do `=`. O valor ficava com
# um `~` literal — que só falha quando alguém USA o caminho, longe da causa.
#
# Como argumento (`echo ~`, `cd ~`) sempre funcionou. Foi a divergência entre
# os dois usos que denunciou, numa tabela de 20 casos de ordem de expansão
# POSIX comparada com o bash — os outros 19 já batiam.
#
# Foram DOIS caminhos de código distintos, e por isso dois invariantes:
#
#   JINV-040  `export V=~/bin` — passa pelo expansor de palavra, que enxerga a
#             linha inteira e reconhece o `NAME=` antes do til.
#   JINV-041  `V=~/bin` — a atribuição simples recebe só o VALOR já separado
#             (`~/bin`), sem o `NAME=` para reconhecer. Ali a regra POSIX é
#             mais simples: expande no início e depois de cada `:` não citado —
#             que é o que faz `PATH=~/bin:~/opt/bin` funcionar.
#
# Consertar só o primeiro teria deixado o caso MAIS comum quebrado.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
# JINV-061: `roda` compara jash e bash por IGUALDADE EXATA sobre uma captura
# com `2>&1`. Sem estes interruptores, qualquer aviso de UX no stderr entra na
# captura e decide o veredito — e o scorecard de adoption dispara a cada 10o
# comando com contador PERSISTENTE entre invocacoes, entao o gate passaria ou
# falharia conforme o contador global caisse num multiplo de 10. Foi assim que
# o JINV-019 ficou verde por sorte desde que foi escrito.
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
command -v python3 >/dev/null || { echo "SKIP: sem python3"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM

roda() { # roda <nome> <script>
    printf '%s' "$2" > "$T/$1.sh"
    local gj gb
    gj=$("$JASH" "$T/$1.sh" 2>&1 | tail -1)
    gb=$(bash    "$T/$1.sh" 2>&1 | tail -1)
    if [ "$gj" = "$gb" ]; then
        ok "$1  → $gj"
    else
        bad "$1" "jash=[$gj]  bash=[$gb]"
    fi
}

# ── 1. o caso corrigido: atribuição exportada ────────────────────────────
roda "export_simples" 'export V=~
echo "[$V]"
'
roda "export_sub"     'export V=~/bin
echo "[$V]"
'
roda "export_lista"   'export V=~/a:~/b
echo "[$V]"
'

# ── 1b. atribuição SIMPLES (o caminho mais comum) ───────────────────────
roda "simples"        'V=~
echo "[$V]"
'
roda "simples_sub"    'V=~/bin
echo "[$V]"
'
roda "simples_lista"  'V=~/a:~/b:~/c
echo "[$V]"
'
roda "path_idiomatico" 'P=~/bin:/usr/bin
echo "[$P]"
'
roda "til_no_meio_valor" 'V=/a/~/b
echo "[$V]"
'

# ── 2. citado continua LITERAL (o til entre aspas não expande) ──────────
roda "citado"         'export V="~"
echo "[$V]"
'
roda "citado_simples" 'export V='"'"'~'"'"'
echo "[$V]"
'

# ── 3. os usos que já funcionavam não regridem ─────────────────────────
roda "arg"            'echo ~
'
roda "cd_til"         'cd ~
pwd
'
roda "til_meio"       'echo a~b
'
roda "til_usuario"    'echo ~naoexiste_zz/x
'

# ── 4. a heurística de NOME= não pode disparar em texto qualquer ───────
# Um `=` que não fecha um nome válido não torna o til elegível.
roda "nao_e_nome"     'echo 1=~
'
roda "igual_solto"    'echo "a=~b"
'

echo ""
echo "=== JINV-040: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
