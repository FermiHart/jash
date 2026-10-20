#!/usr/bin/env bash
# JINV-049 — `checkpoint --json` / `checkpoint list --json`.
#
# O texto já carregava tudo o que um agente precisa: nome, profundidade,
# mutações desde, cwd, hora. Lendo TEXTO, porém, ele volta a parsear com regex
# — a mesma raiz que o JINV-047 atacou no campo `effect`.
#
# Este verbo tem uma história a mais: até o JINV-046, `checkpoint --json`
# **criava um checkpoint chamado "--json"**. A flag virava nome e a sondagem
# gerava lixo de estado. Agora ela é tratada, e o gate exige as duas coisas —
# que a flag produza JSON, e que nenhum checkpoint com esse nome exista.
#
# Como no JINV-048, a saída de máquina vem dos MESMOS dados que o texto
# (`cp_load` + `jash_timeline_depth`). Uma fonte de verdade só: texto e JSON
# não podem divergir.

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

# ── 1. lista vazia é JSON válido, não texto ──────────────────────────────
out=$(J 'checkpoint --json' | tail -1)
if printf '%s' "$out" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null; then
    ok "lista vazia devolve JSON válido"
else
    bad "JSON com lista vazia" "veio: $(printf '%s' "$out" | head -c 70)"
fi
n=$(printf '%s' "$out" | python3 -c "import json,sys; print(len(json.load(sys.stdin)['checkpoints']))" 2>/dev/null)
[ "$n" = "0" ] && ok "lista vazia tem 0 checkpoints" || bad "lista vazia" "veio $n"

# ── 2. com conteúdo: campos e ordem ──────────────────────────────────────
out=$(J 'checkpoint m1; checkpoint m2; checkpoint list --json' | tail -1)
r=$(printf '%s' "$out" | python3 -c "
import json,sys
d=json.load(sys.stdin)
c=d['checkpoints']
campos = all(k in x for x in c for k in ('name','depth','mutations_since','ts','cwd'))
print(len(c), campos, c[0]['name'] if c else '-')" 2>/dev/null)
case "$r" in
    "2 True m2") ok "2 checkpoints, campos completos, mais recente primeiro" ;;
    *) bad "conteúdo da lista" "veio '$r' (esperado '2 True m2')" ;;
esac

# ── 3. a flag NÃO cria um checkpoint chamado '--json' (JINV-046) ────────
out=$(J 'checkpoint --json; checkpoint list --json' | tail -1)
if printf '%s' "$out" | grep -q -- '"--json"'; then
    bad "flag não vira nome" \
        "existe um checkpoint chamado '--json' — a sondagem criou lixo de estado"
else
    ok "nenhum checkpoint chamado '--json' foi criado"
fi

# ── 4. texto e JSON contam o MESMO (uma fonte de verdade) ───────────────
jn=$(J 'checkpoint a; checkpoint b; checkpoint c; checkpoint list --json' | tail -1 |
     python3 -c "import json,sys; print(len(json.load(sys.stdin)['checkpoints']))" 2>/dev/null)
tn=$(J 'checkpoint a; checkpoint b; checkpoint c; checkpoint --list' |
     grep -cE "^  [0-9-]+:[0-9-]+ ")
if [ "$jn" = "$tn" ]; then
    ok "texto e JSON contam o mesmo ($jn checkpoints)"
else
    bad "texto vs JSON" "json=$jn texto=$tn — duas fontes de verdade divergiram"
fi

# ── 5. o texto não regrediu ─────────────────────────────────────────────
J 'checkpoint marca; checkpoint --list' | grep -q "marca" \
    && ok "modo texto intacto" || bad "modo texto" "a listagem humana quebrou"
# E criar checkpoint com nome normal continua funcionando.
J 'checkpoint nome-real; checkpoint list --json' | tail -1 | grep -q "nome-real" \
    && ok "checkpoint com nome normal continua sendo criado" \
    || bad "criação normal" "um nome legítimo parou de ser aceito"

# ── 6. flag desconhecida continua recusada ──────────────────────────────
HOME="$H" JASH_SIGIL=0 JASH_NUDGE=0 "$JASH" -c 'checkpoint --nao-existe' >/dev/null 2>&1
[ $? -ne 0 ] && ok "flag desconhecida no checkpoint continua recusada" \
             || bad "guarda de flags" "o checkpoint voltou a engolir flags"

echo ""
echo "=== JINV-049: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
