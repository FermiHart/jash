#!/usr/bin/env bash
# JINV-025 — o portão de capacidade de módulo tem que ser CAPAZ de negar.
#
# O mecanismo sempre existiu e sempre esteve correto: o manifesto declara
# `caps_declared`, e `jmod_load` recusa quando `caps_declared & ~granted_caps`.
# Mas TODOS os chamadores passavam `0xFFFFFFFF`:
#
#   c/builtins/module.c:11   #define M1_GRANTED_CAPS 0xFFFFFFFF
#   c/builtins/sigil.c:45    jmod_load(spath, 0xFFFFFFFF);
#   c/main.c:1776, :1791     jmod_load(spath, 0xFFFFFFFF);
#   c/core/jash_ai.c:114     jmod_load(sp, 0xFFFFFFFF);
#
# Com isso `missing` era sempre zero: código de segurança real, incapaz de
# negar por construção. O manifesto parecia significar alguma coisa e não
# significava — o mecanismo vivia na camada de UX, não na de imposição, que é
# o diagnóstico central da auditoria de 2026-07-27.
#
# Este gate não checa "existe uma checagem". Ele exige que a checagem
# DISPARE: um módulo cuja capacidade foi negada NÃO pode carregar.
#
# O padrão continua concedendo o que o módulo declarou — nenhum módulo
# existente muda de comportamento. O que mudou é o que o portão é CAPAZ de
# fazer.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
# Avisos de UX (nudge/adoption/loopguard) vao para o STDERR — o que esta
# certo. Silencia-los aqui evita comparar ruido de interface com saida de
# programa; ja quebrou este gate uma vez.
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
MOD="$PWD/modules/sigil/sigil.modjash"
[ -f "$MOD" ] || { echo "SKIP: modules/sigil/sigil.modjash nao construido"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

H=$(mktemp -d) || exit 2
trap 'rm -rf "$H"' EXIT INT TERM
mkdir -p "$H/.jash/modules"
cp "$MOD" "$H/.jash/modules/sigil.modjash"
chmod 0700 "$H/.jash" "$H/.jash/modules"
chmod 0600 "$H/.jash/modules/sigil.modjash"

falhou_carga() { printf '%s' "$1" | grep -q "pede caps"; }

# ── 1. o padrao nao muda nada ─────────────────────────────────────────────
out=$(HOME="$H" "$JASH" -c 'sigil chain verify' 2>&1)
if falhou_carga "$out"; then
    bad "padrao carrega normalmente" \
        "o modulo passou a ser negado sem politica alguma — isto quebraria todo mundo"
else
    ok "sem politica, o modulo carrega como antes"
fi

# ── 2. STRICT sem concessao NEGA (o portao dispara) ──────────────────────
out=$(HOME="$H" JASH_MODULE_CAPS_STRICT=1 "$JASH" -c 'sigil chain verify' 2>&1)
if falhou_carga "$out"; then
    ok "STRICT sem politica NEGA a carga (o portao dispara)"
else
    bad "STRICT nega" \
        "carregou mesmo em modo estrito sem concessao — o portao continua inerte"
fi

# ── 3. a recusa diz COMO conceder ────────────────────────────────────────
# Uma recusa que nao ensina a saida vira "desliga a checagem", que e como
# portoes de seguranca morrem na pratica.
if printf '%s' "$out" | grep -q "policy.conf"; then
    ok "a recusa ensina o caminho (policy.conf)"
else
    bad "recusa didatica" "a mensagem nao diz como conceder"
fi

# ── 4. concessao explicita destrava ──────────────────────────────────────
printf 'sigil: +fs_read +fs_write +net +exec +state_read +state_write +tty +crypto +module_load +hook_pre_cmd +hook_post_cmd +hook_exit\n' \
    > "$H/.jash/modules/policy.conf"
chmod 0600 "$H/.jash/modules/policy.conf"
out=$(HOME="$H" JASH_MODULE_CAPS_STRICT=1 "$JASH" -c 'sigil chain verify' 2>&1)
if falhou_carga "$out"; then
    bad "concessao explicita destrava" "negou mesmo com todas as caps concedidas: $(printf '%s' "$out" | head -1)"
else
    ok "concessao explicita em policy.conf destrava a carga"
fi

# ── 5. negacao explicita bloqueia, mesmo fora do modo estrito ────────────
printf 'sigil: -crypto\n' > "$H/.jash/modules/policy.conf"
chmod 0600 "$H/.jash/modules/policy.conf"
out=$(HOME="$H" "$JASH" -c 'sigil chain verify' 2>&1)
if falhou_carga "$out"; then
    ok "negacao explicita (-crypto) bloqueia a carga"
else
    bad "negacao explicita" "carregou apesar de -crypto — a negacao nao tem efeito"
fi

# ── 6. o bit que falta e EXATAMENTE o negado ─────────────────────────────
# JMOD_CAP_CRYPTO = 1 << 7 = 0x80. Se a mensagem apontar outro bit, a politica
# esta mexendo em caps que nao devia.
if printf '%s' "$out" | grep -q "faltando 0x00000080"; then
    ok "o bit faltante e exatamente JMOD_CAP_CRYPTO (0x80)"
else
    bad "bit correto" \
        "esperava 'faltando 0x00000080', veio: $(printf '%s' "$out" | grep -o 'faltando 0x[0-9a-f]*' | head -1)"
fi

# ── 6b. uma policy existente mas gravavel por terceiros falha fechada ─────
chmod 0666 "$H/.jash/modules/policy.conf"
out=$(HOME="$H" "$JASH" -c 'sigil chain verify' 2>&1)
if printf '%s' "$out" | grep -q "policy exists but is not"; then
    ok "policy.conf insegura bloqueia a carga em vez de ser ignorada"
else
    bad "policy insegura fail-closed" "veio: $(printf '%s' "$out" | head -1)"
fi

# ── 7. a concessao nunca excede o declarado ──────────────────────────────
# `+cap` de algo que o modulo nao declarou nao pode inflar o grant.
if grep -q "grant |= (bit & caps_declared)" c/module/loader.c; then
    ok "policy.conf nunca concede alem do que o modulo declarou"
else
    bad "teto do declarado" \
        "a mascara '& caps_declared' sumiu — policy.conf pode conceder poderes nao declarados"
fi

# ── 8. nenhum chamador voltou a conceder tudo cru ────────────────────────
# 0xFFFFFFFF ainda pode aparecer como TETO do chamador (a politica estreita
# depois), mas o estreitamento tem que continuar existindo.
if grep -q "granted_caps &= jmod_policy_grant" c/module/loader.c; then
    ok "jmod_load estreita a concessao pela politica"
else
    bad "estreitamento" \
        "jmod_load parou de aplicar jmod_policy_grant — o portao volta a ser inerte"
fi

echo ""
echo "=== JINV-025: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
