#!/usr/bin/env bash
# JINV-051 — `sigil chain verify --json`: um veredito que declara o que
# sustenta.
#
# Este verbo tem um requisito que os outros JSON não têm: **ele não pode dizer
# apenas `valid:true`.**
#
# O JINV-024 encontrou o `sigil` afirmando "proof signature valid" quando havia
# apenas integridade, não autenticidade — a chave vinha de dentro da própria
# prova. O JINV-031 encontrou "✓ chain intact" para um log **apagado**. Nos dois
# casos a mensagem afirmava mais do que a verificação sustentava.
#
# Um `valid:true` cru repetiria o mesmo erro em formato de máquina — que é
# **pior**, porque nenhum humano relê.
#
# Por isso a saída declara três coisas:
#   checks        o que foi verificado, item a item
#   not_verified  o que NÃO foi, e por que é impossível
#   limits        o limite honesto do esquema
#
# O `not_verified` não é ressalva defensiva: as tags por entrada são
# forward-secure (`k_n+1 = BLAKE3(k_n ‖ "r")`, sentido único), então tags
# antigas não podem ser recomputadas **nem por este shell**. Quem sabe disso
# decide melhor; quem recebe `valid:true` sem contexto decide pior.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH=${JASH:-"$PWD/build/jash"}
# Avisos de UX (nudge/adoption/loopguard) vao para o STDERR — o que esta
# certo. Silencia-los aqui evita comparar ruido de interface com saida de
# programa; ja quebrou este gate uma vez.
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
command -v python3 >/dev/null || { echo "SKIP: sem python3"; exit 0; }

# Build the module from the source under review. A checked-in or previously
# built .modjash can make this gate validate stale verifier behavior.
MOD_BUILD=$(mktemp -d) || exit 2
trap 'rm -rf "$MOD_BUILD"' EXIT
mkdir -p "$MOD_BUILD/sigil"
ln -s "$PWD/modules/sigil/sigil_module.c" "$MOD_BUILD/sigil/sigil_module.c"
ln -s "$PWD/modules/sigil/manifest.toml" "$MOD_BUILD/sigil/manifest.toml"
if ! HOME="$MOD_BUILD/home" bash "$PWD/modules/build_module.sh" \
        "$MOD_BUILD/sigil" >"$MOD_BUILD/build.log" 2>&1; then
    echo "FAIL: modulo Sigil atual nao compilou" >&2
    exit 1
fi
MOD="$MOD_BUILD/sigil/sigil.modjash"

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

monta() {
    local h; h=$(mktemp -d) || return 1
    mkdir -p "$h/.jash/modules"
    chmod 0700 "$h/.jash" "$h/.jash/modules"
    cp "$MOD" "$h/.jash/modules/"
    chmod 0600 "$h/.jash/modules/sigil.modjash"
    HOME="$h" JASH_SIGIL=2 "$JASH" -c 'sigil 2' >/dev/null 2>&1
    local i; for i in 1 2 3 4 5; do HOME="$h" JASH_SIGIL=2 "$JASH" -c "echo e-$i" >/dev/null 2>&1; done
    printf '%s' "$h"
}
V() { HOME="$1" "$JASH" -c 'sigil chain verify --json' 2>/dev/null | tail -1; }
# ADR-0051 W6: dentro de repo a chain e chain-<key>.log; fora, chain.log.
chainfile() { ls "$1"/.jash/chain*.log 2>/dev/null | head -1; }
# JINV-053: a forma CURTA e a que o consumidor escreve.
Vs() { HOME="$1" "$JASH" -c 'sigil verify --json' 2>/dev/null | tail -1; }

# ── 1. cadeia intacta: valid + o que foi verificado ──────────────────────
H=$(monta) || exit 2
out=$(V "$H")
if printf '%s' "$out" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null; then
    ok "JSON válido"
else
    bad "JSON válido" "veio: $(printf '%s' "$out" | head -c 80)"
fi
r=$(printf '%s' "$out" | python3 -c "import json,sys; print(json.load(sys.stdin)['valid'])" 2>/dev/null)
[ "$r" = "True" ] && ok "cadeia intacta → valid:true" || bad "cadeia intacta" "valid=$r"

# ── 2. o veredito DECLARA o que sustenta ────────────────────────────────
# Sem isto, é o `valid:true` cru que o JINV-024 existiu para matar.
for campo in checks not_verified limits why_not; do
    r=$(printf '%s' "$out" | python3 -c "import json,sys; print('$campo' in json.load(sys.stdin))" 2>/dev/null)
    [ "$r" = "True" ] && ok "declara '$campo'" \
        || bad "campo '$campo'" "o veredito afirma validade sem dizer o que sustenta"
done
r=$(printf '%s' "$out" | python3 -c "
import json,sys
c=json.load(sys.stdin)['checks']
print(all(k in c for k in ('hash_chain','attested_seq_monotonicity','ratchet_position')))" 2>/dev/null)
[ "$r" = "True" ] && ok "os três checks estão nomeados" || bad "checks nomeados" "faltando"
# E o que NÃO pode ser verificado tem que estar dito.
printf '%s' "$out" | grep -q "per_entry_tags" \
    && ok "declara que as tags por entrada NÃO foram verificadas" \
    || bad "not_verified honesto" "omite o que é impossível verificar"
printf '%s' "$out" | grep -qi "forward-secure" \
    && ok "explica POR QUE são impossíveis (forward-secure)" \
    || bad "why_not" "diz o que falta mas não por quê"
rm -rf "$H"

# ── 3. log truncado: valid:false com o número exato ─────────────────────
H=$(monta) || exit 2
cf=$(chainfile "$H"); n=$(wc -l < "$cf")
sed -n "1,$((n-3))p" "$cf" > "$H/.jash/c.t"
mv "$H/.jash/c.t" "$cf"
out=$(V "$H")
r=$(printf '%s' "$out" | python3 -c "
import json,sys
d=json.load(sys.stdin)
print(d['valid'], d.get('failure'), d.get('missing'))" 2>/dev/null)
case "$r" in
    "False log_truncated 3") ok "truncamento → valid:false, failure=log_truncated, missing=3" ;;
    *) bad "truncamento detectado" "veio '$r'" ;;
esac
rm -rf "$H"

# ── 4. log apagado inteiro ──────────────────────────────────────────────
H=$(monta) || exit 2
: > "$(chainfile "$H")"
r=$(V "$H" | python3 -c "
import json,sys
d=json.load(sys.stdin)
print(d['valid'], d.get('failure'))" 2>/dev/null)
case "$r" in
    "False log_truncated") ok "log apagado → valid:false (não 'intacto com 0 entradas')" ;;
    *) bad "log apagado" "veio '$r'" ;;
esac
rm -rf "$H"

# ── 4b. a forma CURTA `sigil verify --json` (JINV-053) ─────────────────
# O JINV-051 entregou a saída sob `sigil chain verify --json`, mas quem consome
# escreve `sigil verify --json` — e essa forma caía no texto de ajuda. Uma
# feature entregue numa invocação que o chamador não usa é feature não
# entregue.
H=$(monta) || exit 2
out=$(Vs "$H")
if printf '%s' "$out" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null; then
    ok "'sigil verify --json' (forma curta) devolve JSON"
else
    bad "forma curta" "caiu no texto de ajuda: $(printf '%s' "$out" | head -c 60)"
fi
# As duas formas têm que concordar no veredito.
a=$(printf '%s' "$out"     | python3 -c "import json,sys; print(json.load(sys.stdin)['valid'])" 2>/dev/null)
b=$(V "$H" | python3 -c "import json,sys; print(json.load(sys.stdin)['valid'])" 2>/dev/null)
if [ "$a" = "$b" ]; then ok "as duas invocações concordam (valid=$a)"
else bad "invocações divergem" "curta=$a longa=$b"; fi
# E a forma curta em TEXTO não pode ter regredido.
txt=$(HOME="$H" "$JASH" -c 'sigil verify' 2>/dev/null)
case "$txt" in
    *"chain intact"*) ok "'sigil verify' em texto continua funcionando" ;;
    *) bad "forma curta em texto" "veio: $(printf '%s' "$txt" | head -c 60)" ;;
esac
rm -rf "$H"

# ── 5. o modo texto não regrediu ────────────────────────────────────────
H=$(monta) || exit 2
HOME="$H" "$JASH" -c 'sigil chain verify' 2>/dev/null | grep -q "chain intact" \
    && ok "modo texto intacto" || bad "modo texto" "a saída humana mudou"
rm -rf "$H"

echo ""
echo "=== JINV-051: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
