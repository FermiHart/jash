#!/usr/bin/env bash
# JINV-048 — `explain --json`: a opinião do shell sobre perigo, legível por
# máquina.
#
# O texto do `explain` já dizia tudo o que um guard precisa — efeito por passo
# e raio de impacto. Mas um agente que lê TEXTO volta a adivinhar, que é
# exatamente a raiz do problema que o JINV-047 atacou (segurança inferida por
# regex sobre o nome do comando).
#
# Com esta saída, o gate de perigo de um agente deixa de ser regex e passa a
# ser **a opinião do shell**, que é quem sabe o que `rm` faz.
#
# ═══ A propriedade que este gate protege ═══
#
# O JSON sai dos **mesmos contadores** que produzem o texto — não há segunda
# fonte de verdade. Por isso o teste compara as duas saídas entre si: se
# alguém acrescentar um efeito novo e esquecer um dos ramos, os números
# divergem e o gate reprova. Duas fontes que podem divergir é o defeito, não a
# feature.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
command -v python3 >/dev/null || { echo "SKIP: sem python3"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

E() { JASH_SIGIL=0 JASH_NUDGE=0 JASH_BRIEF=0 "$JASH" -c "$1" 2>/dev/null; }

# ── 1. JSON válido, uma linha ────────────────────────────────────────────
out=$(E 'explain --json rm -rf /tmp/zz')
if printf '%s' "$out" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null; then
    ok "JSON válido"
else
    bad "JSON válido" "o parser recusou: $(printf '%s' "$out" | head -c 90)"
fi
[ "$(printf '%s\n' "$out" | grep -c .)" = "1" ] && ok "uma linha só" \
    || bad "uma linha" "$(printf '%s\n' "$out" | grep -c .) linhas"

# ── 2. os campos que o consumidor precisa ────────────────────────────────
for campo in cmd effect recoverable how writes target; do
    r=$(printf '%s' "$out" | python3 -c "
import json,sys
d=json.load(sys.stdin)
print(all('$campo' in s for s in d['steps']))" 2>/dev/null)
    [ "$r" = "True" ] && ok "todo passo tem '$campo'" \
                      || bad "campo '$campo'" "ausente em algum passo"
done
r=$(printf '%s' "$out" | python3 -c "
import json,sys
d=json.load(sys.stdin)['blast']
print(all(k in d for k in ('mutations','delete','write','network','opaque')))" 2>/dev/null)
[ "$r" = "True" ] && ok "o bloco 'blast' tem os cinco contadores" \
                  || bad "blast completo" "falta contador"

# ── 3. a classificação bate com o comportamento esperado ────────────────
chk() { # chk <comando> <efeito esperado>
    local got
    got=$(E "explain --json $1" | python3 -c "
import json,sys
print(json.load(sys.stdin)['steps'][0]['effect'])" 2>/dev/null)
    [ "$got" = "$2" ] && ok "'$1' → $got" || bad "efeito de '$1'" "esperado '$2', veio '$got'"
}
chk 'rm x'                delete
chk 'ls'                  read
chk 'mv a b'              move
chk 'cp a b'              copy
chk 'mkdir d'             create
chk 'curl http://exemplo' network

# ── 4. redirect é detectado como escrita ────────────────────────────────
r=$(E "explain --json 'echo oi > /tmp/alvo.txt'" | python3 -c "
import json,sys
s=json.load(sys.stdin)['steps'][0]
print(s['writes'], s['target'])" 2>/dev/null)
case "$r" in
    "True /tmp/alvo.txt") ok "redirect vira writes=true com o alvo" ;;
    *) bad "redirect detectado" "veio '$r'" ;;
esac

# ── 5. TEXTO e JSON não podem divergir (a propriedade central) ──────────
# Os dois saem dos mesmos contadores; se alguém acrescentar um efeito e
# esquecer um ramo, os números separam e este teste denuncia.
cmd='rm a; curl http://b > c.txt; ls'
jt=$(E "explain --json '$cmd'" | python3 -c "
import json,sys
b=json.load(sys.stdin)['blast']
print(b['mutations'], b['delete'], b['write'], b['network'], b['opaque'])" 2>/dev/null)
tt=$(E "explain '$cmd'" | grep -oE "blast radius: [0-9]+ mutation\(s\) — [0-9]+ delete, [0-9]+ write, [0-9]+ network; [0-9]+" \
     | grep -oE "[0-9]+" | tr '\n' ' ' | sed 's/ $//')
if [ "$jt" = "$tt" ]; then
    ok "texto e JSON concordam nos contadores ($jt)"
else
    bad "texto vs JSON" \
        "json=[$jt] texto=[$tt] — duas fontes de verdade divergiram"
fi

# ── 6. o modo texto não regrediu ────────────────────────────────────────
E 'explain rm -rf /tmp/zz' | grep -q "DELETE" \
    && ok "modo texto intacto" || bad "modo texto" "a saída humana mudou"
# E `--json` continua sendo a ÚNICA flag aceita pelo explain (JINV-046).
E 'explain --nao-existe true' >/dev/null 2>&1
[ $? -ne 0 ] && ok "flag desconhecida no explain continua recusada" \
             || bad "guarda de flags" "o explain voltou a engolir flags"

echo ""
echo "=== JINV-048: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
