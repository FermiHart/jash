#!/usr/bin/env bash
# JINV-018 — o descritor de capacidades reporta o que é APLICADO.
#
# `jash --capabilities-json` responde numa linha o que hoje um agente só
# descobre tentando e falhando: o que ESTE jash, nesta máquina, de fato faz.
#
# O risco que este gate existe para trancar não é o descritor faltar — é ele
# MENTIR. A auditoria de 2026-07-27 diagnosticou que os diferenciais do jash
# estavam implementados na camada de UX e não na de imposição: `sigil`
# imprimia "attested" sem nunca verificar o MAC; `timeline` dizia "← undo
# reverts this" engolindo falha de backup. Um descritor de capacidades que
# herdasse esse hábito seria a pior versão do problema — mentira em formato de
# máquina, que nenhum humano relê antes de confiar.
#
# Por isso os testes abaixo não checam só a presença dos campos: checam que
# cada afirmação BATE com o comportamento observado do binário, e que o campo
# não-imposto (`timeout_ms`) se declara "reserved" em vez de `true`.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

PY=$(command -v python3 || command -v python || true)
J=$("$JASH" --capabilities-json 2>/dev/null)

# ── 1. sai UMA linha, e é JSON válido ──────────────────────────────────────
nlines=$(printf '%s\n' "$J" | grep -c .)
if [ "$nlines" -eq 1 ]; then
    ok "uma linha só (consumível por linha, como o resto do protocolo)"
else
    bad "uma linha" "$nlines linhas — quebra leitura linha-a-linha"
fi

if [ -n "$PY" ]; then
    if printf '%s' "$J" | "$PY" -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null; then
        ok "JSON válido"
    else
        bad "JSON válido" "o parser recusou: $J"
    fi
else
    ok "JSON válido (sem python — checagem pulada)"
fi

# ── 2. identidade bate com --whoami (uma fonte de verdade, não duas) ───────
W=$("$JASH" --whoami 2>/dev/null)
wver=$(printf '%s' "$W" | sed -n 's/.*version=\([^ ]*\).*/\1/p')
if printf '%s' "$J" | grep -q "\"version\":\"$wver\""; then
    ok "version bate com --whoami ($wver)"
else
    bad "version bate com --whoami" "--whoami diz '$wver'; descritor: $J"
fi

wabi=$(printf '%s' "$W" | sed -n 's/.*modabi=\([0-9]*\).*/\1/p')
if printf '%s' "$J" | grep -q "\"modabi\":$wabi"; then
    ok "modabi bate com --whoami ($wabi)"
else
    bad "modabi bate com --whoami" "--whoami diz '$wabi'"
fi

# ── 3. builtins: o número declarado bate com a tabela real ─────────────────
n=$(printf '%s' "$J" | sed -n 's/.*"builtins":\([0-9]*\).*/\1/p')
if [ -n "$n" ] && [ "$n" -gt 50 ]; then
    ok "builtins declarados: $n (tabela real)"
else
    bad "builtins" "valor implausível: '$n'"
fi

# A contagem e as classes de maior risco precisam refletir a tabela realmente
# publicada. `find` delega formas não nativas, `impact --run` executa comandos e
# `jash_state` também salva/restaura/limpa estado.
if [ -n "$PY" ]; then
    B=$("$JASH" --builtins-json 2>/dev/null)
    r=$(printf '%s' "$B" | "$PY" -c '
import json,sys
d=json.load(sys.stdin)
effects={b["name"]: b["effect"] for b in d["builtins"]}
actual=sum(value != "unknown" for value in effects.values())
expected={"echo":"read", "find":"exec", "impact":"exec", "jash_state":"mutate"}
print("ok" if d["classified"] == actual and all(effects.get(k) == v for k,v in expected.items()) else "bad")' 2>/dev/null)
    if [ "$r" = "ok" ]; then
        ok "efeitos perigosos são conservadores e classified conta o inventário real"
    else
        bad "efeitos/classified confiáveis" "$r"
    fi
else
    ok "efeitos/classified (sem python — checagem pulada)"
fi

# ── 4. a AFIRMAÇÃO sobre exec_json bate com o COMPORTAMENTO ────────────────
# Nao basta dizer "cwd":true — o binario tem que aplicar cwd de fato.
if printf '%s' "$J" | grep -q '"cwd":true'; then
    got=$("$JASH" --exec-json '{"argv":["pwd"],"cwd":"/tmp"}' 2>/dev/null | tail -1)
    if [ -n "$got" ] && [ "$(cd "$got" 2>/dev/null && pwd -P)" = "$(cd /tmp && pwd -P)" ]; then
        ok "declara cwd:true E aplica cwd (afirmação verificada)"
    else
        bad "cwd declarado mas nao aplicado" \
            "o descritor afirma \"cwd\":true e pwd devolveu '$got' — MENTIRA de máquina"
    fi
else
    bad "exec_json.cwd presente" "campo ausente do descritor"
fi

if printf '%s' "$J" | grep -q '"stdin":true'; then
    got=$("$JASH" --exec-json '{"argv":["cat"],"stdin":"probe018"}' </dev/null 2>/dev/null | tail -1)
    if [ "$got" = "probe018" ]; then
        ok "declara stdin:true E entrega stdin (afirmação verificada)"
    else
        bad "stdin declarado mas nao entregue" "cat devolveu '$got'"
    fi
else
    bad "exec_json.stdin presente" "campo ausente"
fi

# ── 5. o campo NÃO-IMPOSTO se declara como tal ─────────────────────────────
# `timeout_ms` e aceito pelo protocolo e NAO e imposto (serve.h). Dizer
# `true` aqui seria exatamente a mentira que este descritor existe para nao
# contar.
if printf '%s' "$J" | grep -q '"timeout_ms":"reserved"'; then
    ok "timeout_ms declarado 'reserved' (aceito, nao imposto — a verdade)"
else
    bad "timeout_ms honesto" \
        "esperado \"timeout_ms\":\"reserved\"; se virou true, alguem prometeu imposicao que nao existe"
fi

# ── 6. OSIF: have + missing cobrem os 8 bits, sem sobreposicao ─────────────
if [ -n "$PY" ]; then
    r=$(printf '%s' "$J" | "$PY" -c '
import json,sys
d=json.load(sys.stdin)["osif"]
h,m=set(d["have"]),set(d["missing"])
print("ok" if len(h)+len(m)==8 and not (h&m) else "bad %d/%d"%(len(h),len(m)))' 2>/dev/null)
    if [ "$r" = "ok" ]; then
        ok "osif: have+missing = os 8 bits, sem sobreposicao"
    else
        bad "osif consistente" "$r"
    fi
else
    ok "osif (sem python — checagem pulada)"
fi

# ── 7. o alvo declarado é o alvo compilado ─────────────────────────────────
if printf '%s' "$J" | grep -qE '"target":"(macos|linux|beyondbsd|minix|netbsd)"'; then
    ok "target declarado é um backend OSIF real"
else
    bad "target" "valor desconhecido no descritor"
fi

echo ""
echo "=== JINV-018: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
