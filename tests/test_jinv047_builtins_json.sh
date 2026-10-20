#!/usr/bin/env bash
# JINV-047 — inventário de builtins com EFEITO declarado.
#
# `--capabilities-json` dizia `"builtins":152`. Uma contagem não ajuda quem
# precisa decidir a única pergunta que importa: **posso rodar isto sem
# perguntar?**
#
# Hoje um agente responde isso adivinhando por regex sobre o NOME do comando.
# Quem pediu esta feature foi mordido duas vezes pelo mesmo erro no mesmo dia:
# uma comparação sensível a caixa deixou um alias `Bash` furar o guard, e a
# allow-list de leitura deixou `grep` minúsculo passar. A raiz é sempre a
# mesma — segurança inferida de um nome.
#
# A correção não é uma regex melhor: é o shell DECLARAR o efeito. Quem sabe o
# que `rm` faz é quem o implementa.
#
# ═══ A regra que este gate protege ═══
#
# A classificação é EXPLÍCITA, nome a nome (`builtins/builtin_effects.c`).
# Derivá-la de heurística sobre o nome reproduziria exatamente o bug que ela
# existe para matar.
#
# E `unknown` é o padrão — resposta legítima, não buraco: um `effect` errado é
# pior que ausente, porque um guard que confia num `read` falso executa a coisa
# perigosa **com confiança**. Este gate exige que os campos existam e batam com
# o comportamento, não que estejam todos preenchidos.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
command -v python3 >/dev/null || { echo "SKIP: sem python3"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM
"$JASH" --builtins-json > "$T/b.json" 2>/dev/null || { echo "FAIL: --builtins-json falhou"; exit 1; }

q() { python3 -c "
import json,sys
d=json.load(open('$T/b.json'))
b={x['name']:x for x in d['builtins']}
$1" 2>/dev/null; }

# ── 1. JSON válido e uma linha só ────────────────────────────────────────
if [ "$(grep -c . "$T/b.json")" = "1" ]; then ok "uma linha só"; else bad "uma linha" "$(grep -c . "$T/b.json")"; fi
if python3 -c "import json;json.load(open('$T/b.json'))" 2>/dev/null; then
    ok "JSON válido"
else
    bad "JSON válido" "o parser recusou"
fi

# ── 2. cobre TODOS os builtins, sem inventar nem faltar ─────────────────
n=$(q "print(len(d['builtins']))")
c=$("$JASH" --capabilities-json 2>/dev/null | grep -oE '"builtins":[0-9]+' | grep -oE '[0-9]+')
if [ "$n" = "$c" ]; then
    ok "o inventário cobre os $n builtins que o descritor conta"
else
    bad "cobertura" "inventário=$n, descritor=$c — as duas fontes divergiram"
fi

# ── 3. os campos são de dado REAL, não inventado ────────────────────────
for campo in name help argv_native effect; do
    r=$(q "print(all('$campo' in x for x in d['builtins']))")
    [ "$r" = "True" ] && ok "todo builtin tem '$campo'" \
                      || bad "campo '$campo'" "faltando em algum builtin"
done
# `help` tem que vir da tabela, não ser string vazia em massa.
vazios=$(q "print(sum(1 for x in d['builtins'] if not x['help']))")
if [ "${vazios:-999}" -le 5 ]; then
    ok "o campo 'help' vem da tabela real ($vazios vazios)"
else
    bad "help real" "$vazios builtins com help vazio — o campo virou decoração"
fi

# ── 4. os casos de aceitação do pedido ──────────────────────────────────
for par in "rm:mutate" "grep:read" "speculate:exec" "eval:exec" \
           "ls:mutate" "digest:mutate" "context:exec" "doctor:mutate" \
           "printf:mutate" "history:mutate" "statusbar:mutate" \
           "type:mutate" "which:mutate" "jobs:mutate" \
           "stat:exec" "tdiff:exec" "osif:exec" "json:exec"; do
    nome="${par%%:*}"; esperado="${par##*:}"
    got=$(q "print(b.get('$nome',{}).get('effect','AUSENTE'))")
    if [ "$got" = "$esperado" ]; then
        ok "$nome → $got"
    else
        bad "efeito de $nome" "esperado '$esperado', veio '$got'"
    fi
done

# ── 5. o efeito é declarado, NÃO derivado do nome ───────────────────────
# Nomes parecidos nao compartilham efeito: tread e nativo read-only, enquanto
# tdiff pode cair em execlp e por isso recebe a autoridade maxima.
for par in "trash:mutate" "tread:read" "tdiff:exec"; do
    nome="${par%%:*}"; esperado="${par##*:}"
    got=$(q "print(b.get('$nome',{}).get('effect','AUSENTE'))")
    [ "$got" = "$esperado" ] && ok "$nome → $got (nome parecido, efeito diferente)" \
                             || bad "efeito de $nome" "esperado '$esperado', veio '$got'"
done
if grep -q "EFFECTS\[i\].name, name" c/builtins/builtin_effects.c; then
    ok "a classificação é por igualdade de nome, não por padrão/regex"
else
    bad "classificação explícita" \
        "a busca virou heurística — é o bug que este invariante existe para matar"
fi

# ── 6. 'unknown' é o padrão fail-closed ─────────────────────────────────
if grep -q 'return "unknown"' c/builtins/builtin_effects.c; then
    ok "builtin não classificado devolve 'unknown' (fail-closed)"
else
    bad "padrão fail-closed" \
        "o retorno padrão sumiu — um builtin novo nasceria sem efeito declarado"
fi

# ── 7. a flag entrou na lista de conhecidas (JINV-045) ──────────────────
"$JASH" --builtins-json >/dev/null 2>&1
[ $? -eq 0 ] && ok "--builtins-json é uma flag reconhecida" \
             || bad "flag reconhecida" "a guarda de flags desconhecidas a barrou"

echo ""
echo "=== JINV-047: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
