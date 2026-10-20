#!/usr/bin/env bash
# JINV-031 — truncar o log de auditoria não pode passar como "intacto".
#
# `sigil chain verify` recomputava apenas a CADEIA DE HASH. Isso detecta uma
# entrada alterada e — pela numeração — uma entrada removida do meio. Mas não
# detecta truncamento: remover as últimas entradas deixa um prefixo
# perfeitamente válido, onde todos os hashes encadeiam e não há lacuna de
# `seq`, porque o fim simplesmente não existe mais.
#
# Medido antes desta correção:
#
#   apagar as 3 últimas entradas  →  "✓ chain intact — 4 entries verified"
#   apagar o log INTEIRO          →  "✓ chain intact — 0 entries verified"
#
# Um checkmark verde para "o log de auditoria sumiu". Numa ferramenta cuja tese
# é atestação, esse é o único veredito que não pode existir.
#
# O campo `tag=` (MAC de ratchet) era escrito em toda entrada e **nunca lido**.
# Como ele é forward-secure, tags antigas não podem ser recomputadas nem pelo
# próprio shell — mas a POSIÇÃO do ratchet pode: ele avança exatamente uma vez
# por entrada, e a derivação `k_{n+1} = BLAKE3(k_n || "r")` é de sentido único.
# A contagem de avanços é, portanto, uma testemunha INDEPENDENTE de quantas
# entradas existiram, e um atacante que apaga o fim do log não consegue
# desfazer os avanços que já aconteceram.
#
# LIMITE HONESTO, declarado em vez de escondido: apagar o log E a chave juntos
# é indistinguível de uma instalação nova. Nenhum esquema puramente local
# resolve isso — só uma testemunha externa (a prova assinada do JINV-024).

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
# Avisos de UX (nudge/adoption/loopguard) vao para o STDERR — o que esta
# certo. Silencia-los aqui evita comparar ruido de interface com saida de
# programa; ja quebrou este gate uma vez.
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off
MOD="$PWD/modules/sigil/sigil.modjash"
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
[ -f "$MOD" ]  || { echo "SKIP: modulo sigil nao construido"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

# Monta um HOME isolado com N entradas de cadeia; ecoa o caminho.
# JASH_SIGIL=1 é OBRIGATÓRIO: o ultra-fast -c path (main.c:1980) retorna antes
# do auto-init quando o env não liga o sigil — `echo` one-liner nunca chegaria
# à chain. Sem esta linha o teste mede o caminho errado (o rápido), não o atestado.
monta() {
    local h; h=$(mktemp -d) || return 1
    bash "$PWD/tests/setup_sigil_modules.sh" "$h"
    HOME="$h" JASH_SIGIL=2 "$JASH" -c 'sigil 2' >/dev/null 2>&1
    local i
    for i in 1 2 3 4 5 6; do HOME="$h" JASH_SIGIL=2 "$JASH" -c "echo e-$i" >/dev/null 2>&1; done
    printf '%s' "$h"
}
verifica() { HOME="$1" "$JASH" -c 'sigil chain verify' 2>&1; }
# ADR-0051 W6: inside a repo the chain is chain-<key>.log; outside, chain.log.
chainfile() { ls "$1"/.jash/chain*.log 2>/dev/null | head -1; }
# The ratchet follows the same repository scope as the chain; only executions
# outside a repository use the legacy ratchet.key fallback.
ratchetfile() { ls "$1"/.jash/ratchet*.key 2>/dev/null | head -1; }

# ── 1. cadeia intacta continua passando ───────────────────────────────────
H=$(monta) || exit 2
out=$(verifica "$H")
if printf '%s' "$out" | grep -q "chain intact"; then
    ok "cadeia intacta passa"
else
    bad "cadeia intacta" "recusou uma cadeia legítima: $(printf '%s' "$out" | head -1)"
fi

# ── 2. e a mensagem diz O QUE foi verificado ──────────────────────────────
# "chain intact" sozinho deixa o leitor supor mais do que foi checado.
if printf '%s' "$out" | grep -q "posicao do ratchet"; then
    ok "a mensagem nomeia as duas checagens (hash + posicao)"
else
    bad "mensagem honesta" "o veredito nao diz o que sustenta"
fi
rm -rf "$H"

# ── 3. TRUNCAR O FIM é detectado (o buraco central) ───────────────────────
H=$(monta) || exit 2
cf=$(chainfile "$H")
n=$(wc -l < "$cf" | tr -d ' ')
sed -n "1,$((n-3))p" "$cf" > "$H/.jash/c.t"
mv "$H/.jash/c.t" "$cf"
out=$(verifica "$H")
if printf '%s' "$out" | grep -q "TRUNCADO"; then
    ok "truncar as 3 ultimas entradas e DETECTADO"
else
    bad "truncamento detectado" \
        "passou como intacto: $(printf '%s' "$out" | head -1)"
fi
if printf '%s' "$out" | grep -qE "Faltam 3"; then
    ok "a mensagem diz QUANTAS entradas faltam"
else
    bad "mensagem quantitativa" "nao informa o numero de entradas removidas"
fi
rm -rf "$H"

# ── 4. apagar o log INTEIRO é detectado ───────────────────────────────────
H=$(monta) || exit 2
: > "$(chainfile "$H")"
out=$(verifica "$H")
if printf '%s' "$out" | grep -q "TRUNCADO"; then
    ok "apagar o log inteiro e DETECTADO"
else
    bad "log vazio detectado" \
        "'chain intact — 0 entries' e um checkmark verde para o log ter sumido"
fi
rm -rf "$H"

# ── 5. entrada acrescentada sem ratchet é detectada ───────────────────────
H=$(monta) || exit 2
cf=$(chainfile "$H"); tail -1 "$cf" | sed 's/^[0-9]*/99/' >> "$cf"
out=$(verifica "$H")
if printf '%s' "$out" | grep -qiE "gap|FORJADAS|BROKEN"; then
    ok "entrada acrescentada e DETECTADA"
else
    bad "entrada forjada" "aceitou uma entrada que nao passou pelo ratchet"
fi
rm -rf "$H"

# ── 6. a posição é PERSISTIDA junto da chave ──────────────────────────────
H=$(monta) || exit 2
key=$(ratchetfile "$H")
if [ -f "$key" ] && grep -qE '^[0-9a-f]{64} [0-9]+' "$key"; then
    ok "ratchet scoped guarda chave E posicao ($(wc -c < "$key" | tr -d ' ') bytes)"
else
    bad "posicao persistida" \
        "o ratchet voltou a guardar so a chave — sem a posicao, truncar passa de novo"
fi

# ── 7. formato antigo (só a chave) degrada, não acusa falsamente ─────────
# Um usuário com ratchet.key legado não pode receber "LOG TRUNCADO" errado.
head -c 64 "$key" > "$key.legado" && mv "$key.legado" "$key"
out=$(verifica "$H")
if printf '%s' "$out" | grep -q "TRUNCADO"; then
    bad "compatibilidade com formato antigo" \
        "uma chave legada (so 64 bytes) gerou acusacao falsa de truncamento"
else
    ok "chave em formato antigo degrada em silencio (sem acusacao falsa)"
fi
rm -rf "$H"

echo ""
echo "=== JINV-031: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
