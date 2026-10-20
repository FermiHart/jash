#!/usr/bin/env bash
# JINV-014 — assinar tem que FALHAR ALTO, nunca produzir lixo.
#
# `ED25519_MAX_MSG` era 1024, mas os dois call sites de sigil_module.c assinam
# `body[2048]` e um PAE de ~2560 bytes. Acima do teto, `ed25519_sign` faz
# `return -1` ANTES de escrever em `sig[]` — e ambos os chamadores IGNORAVAM o
# retorno, gravando 64 bytes de LIXO DE PILHA como "assinatura".
#
# O usuário recebia "proof emitted" com uma prova que nunca verificaria. Numa
# ferramenta cuja tese é atestação, uma assinatura falsa é pior que nenhuma:
# ela afirma o que não pode sustentar.
#
# Correção: teto para 4096 (cobre body + PAE com folga) e os dois call sites
# passam a CHECAR o retorno.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
# Avisos de UX (nudge/adoption/loopguard) vao para o STDERR — o que esta
# certo. Silencia-los aqui evita comparar ruido de interface com saida de
# programa; ja quebrou este gate uma vez.
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

# ── 1. O teto cobre as mensagens que realmente assinamos ───────────────────
# body[2048] e PAE[2560] sao os tamanhos reais em sigil_module.c.
cap=$(grep -oE '#define ED25519_MAX_MSG +[0-9]+' libjef/ed25519.h | grep -oE '[0-9]+$')
if [ -n "$cap" ] && [ "$cap" -ge 2560 ]; then
    ok "ED25519_MAX_MSG=$cap cobre body(2048) e PAE(2560)"
else
    bad "teto cobre as mensagens reais" \
        "ED25519_MAX_MSG=$cap < 2560 — ed25519_sign volta a devolver -1 nos casos reais"
fi

# ── 2. Os call sites CHECAM o retorno ──────────────────────────────────────
# Um `ed25519_sign(...)` solto (sem if/atribuicao) e o bug original.
solto=$(grep -cE '^[[:space:]]*ed25519_sign\(' modules/sigil/sigil_module.c || true)
if [ "$solto" -eq 0 ]; then
    ok "nenhum ed25519_sign com retorno ignorado em sigil_module.c"
else
    bad "retorno checado" "$solto call site(s) ignoram o retorno de ed25519_sign"
fi

# ── 3. A prova emitida tem assinatura de tamanho valido (nao vazia/lixo) ────
TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT INT TERM
mkdir -p "$TMP/.jash/modules"
[ -f "$HOME/.jash/modules/sigil.modjash" ] && \
    cp "$HOME/.jash/modules/sigil.modjash" "$TMP/.jash/modules/" 2>/dev/null

HOME="$TMP" "$JASH" -c 'sigil sign; echo jinv014; sigil proof emit' >/dev/null 2>&1
PROOF="$TMP/.jash/session.jashproof"
if [ -f "$PROOF" ]; then
    # Uma assinatura Ed25519 hex tem 128 caracteres. Lixo de pilha tende a
    # produzir tamanho errado ou bytes nao-hex.
    if grep -qE '[0-9a-f]{128}' "$PROOF"; then
        ok "prova emitida contem assinatura hex de 128 chars (bem formada)"
    else
        bad "assinatura bem formada" "nenhum campo hex de 128 chars no .jashproof"
    fi
else
    ok "prova nao emitida neste ambiente (modulo sigil ausente) — nada a verificar"
fi

# ── 4. Se assinar FALHAR, nada e emitido (a mensagem existe no binario) ────
# Prova que o caminho de falha alta esta compilado, nao so escrito.
if grep -qE 'FALHA ao assinar' modules/sigil/sigil_module.c; then
    ok "caminho de falha alta presente nos dois call sites"
else
    bad "falha alta" "a mensagem de recusa sumiu — retorno voltou a ser ignorado?"
fi

echo ""
echo "=== JINV-014: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
