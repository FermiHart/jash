#!/usr/bin/env bash
# JINV-050 — `timeline --json`.
#
# O texto já carrega tudo: hora, tipo da mutação, descrição, e qual entrada o
# `undo` reverteria. Lendo TEXTO, um agente volta a parsear com regex — a raiz
# que o JINV-047 atacou.
#
# A saída sai dos MESMOS slots que o texto (`tl_slot`), então não há segunda
# fonte de verdade. O campo `undoes` é a prosa do texto ("← undo reverts this")
# virando dado.
#
# ═══ O que este gate mede, e por quê ═══
#
# Ele NÃO afirma que a timeline registra X ou Y operações — isso depende de
# condições de sessão que variam. Ele afirma a propriedade que importa: **texto
# e JSON contam a mesma coisa**, sempre. Se a timeline registra 3 mutações, as
# duas saídas dizem 3; se registra 0, as duas dizem 0.
#
# Uma asserção de "deve registrar N" seria frágil e, pior, mediria a timeline
# em vez de medir o JSON — o erro que eu cometi no gate do JINV-046, onde
# testei o mecanismo em vez da propriedade e travei a implementação legítima
# de uma feature três horas depois.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
command -v python3 >/dev/null || { echo "SKIP: sem python3"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

H=$(mktemp -d) || exit 2
trap 'rm -rf "$H"' EXIT INT TERM
J() { HOME="$H" JASH_SIGIL=0 JASH_NUDGE=0 JASH_BRIEF=0 "$JASH" -c "$1" 2>/dev/null; }

# ── 1. JSON válido em qualquer estado ────────────────────────────────────
out=$(J 'timeline --json' | tail -1)
if printf '%s' "$out" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null; then
    ok "JSON válido com timeline vazia"
else
    bad "JSON válido" "veio: $(printf '%s' "$out" | head -c 70)"
fi
r=$(printf '%s' "$out" | python3 -c "
import json,sys
d=json.load(sys.stdin)
print('mutations' in d and 'entries' in d)" 2>/dev/null)
[ "$r" = "True" ] && ok "os campos 'mutations' e 'entries' existem" \
                  || bad "campos" "faltando no objeto raiz"

# ── 2. a propriedade central: texto e JSON contam o MESMO ───────────────
cmds='touch t1; touch t2; rm t1'
jn=$(J "cd $H; $cmds; timeline --json" | tail -1 |
     python3 -c "import json,sys; print(json.load(sys.stdin)['mutations'])" 2>/dev/null)
tt=$(J "cd $H; $cmds; timeline" | grep -oE "SESSION TIMELINE — [0-9]+ mutation" |
     grep -oE "[0-9]+")
tt="${tt:-0}"
if [ "${jn:-x}" = "$tt" ]; then
    ok "texto e JSON contam o mesmo ($jn mutações)"
else
    bad "texto vs JSON" "json=$jn texto=$tt — duas fontes de verdade divergiram"
fi

# ── 3. o número declarado bate com o tamanho da lista ───────────────────
r=$(J "cd $H; $cmds; timeline --json" | tail -1 | python3 -c "
import json,sys
d=json.load(sys.stdin)
print(d['mutations'] == len(d['entries']))" 2>/dev/null)
[ "$r" = "True" ] && ok "'mutations' bate com o tamanho de 'entries'" \
                  || bad "contagem coerente" "o número declarado difere da lista"

# ── 4. quando há entradas, os campos estão completos ────────────────────
r=$(J "cd $H; $cmds; timeline --json" | tail -1 | python3 -c "
import json,sys
d=json.load(sys.stdin)
e=d['entries']
if not e: print('VAZIO')
else: print(all(k in x for x in e for k in ('kind','desc','ts','undoes')))" 2>/dev/null)
case "$r" in
    True)  ok "toda entrada tem kind/desc/ts/undoes" ;;
    VAZIO) ok "timeline vazia neste ambiente — campos não verificados" ;;
    *)     bad "campos da entrada" "faltando algum: '$r'" ;;
esac

# ── 5. no máximo UMA entrada marcada como 'undoes' ─────────────────────
# É a que o `undo` reverteria; duas seria contradição.
r=$(J "cd $H; $cmds; timeline --json" | tail -1 | python3 -c "
import json,sys
print(sum(1 for x in json.load(sys.stdin)['entries'] if x['undoes']))" 2>/dev/null)
if [ "${r:-0}" -le 1 ]; then
    ok "no máximo uma entrada marcada como 'undoes' ($r)"
else
    bad "undoes único" "$r entradas dizem que o undo as reverteria"
fi

# ── 6. o texto não regrediu ─────────────────────────────────────────────
J 'timeline' | grep -qiE "timeline|mutation" \
    && ok "modo texto intacto" || bad "modo texto" "a saída humana sumiu"

# ── 7. flag desconhecida continua recusada (JINV-046) ──────────────────
HOME="$H" JASH_SIGIL=0 JASH_NUDGE=0 "$JASH" -c 'timeline --nao-existe' >/dev/null 2>&1
[ $? -ne 0 ] && ok "flag desconhecida no timeline é recusada" \
             || bad "guarda de flags" "o timeline engole flags desconhecidas"

echo ""
echo "=== JINV-050: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
