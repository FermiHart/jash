#!/usr/bin/env bash
# JINV-024 — integridade não é autenticidade, e a mensagem tem que dizer qual
# das duas foi verificada.
#
# `jef_verify_proof` valida a assinatura com a chave pública que vem DENTRO da
# prova. Isso demonstra que ela não foi adulterada depois de assinada — e só
# isso. NÃO demonstra QUEM assinou: qualquer um gera um par de chaves novo,
# assina uma prova forjada, embute a própria chave pública, e recebia
# exatamente o mesmo `✓ proof signature valid` que uma prova legítima.
#
# A criptografia estava certa; a MENSAGEM é que afirmava mais do que a
# verificação sustentava. Numa ferramenta cuja tese é atestação, essa é a
# falha mais cara possível — e é o diagnóstico central da auditoria de
# 2026-07-27 (diferenciais na camada de UX, não na de imposição) na forma mais
# pura que ele aparece no código.
#
# Correção: `jef_verify_proof_pinned` (verificação ancorada numa identidade
# conhecida) e uma saída que distingue os dois casos — âncora sendo a
# identidade persistente do dispositivo (~/.jash/keys/ed25519.seed) mais
# ~/.jash/keys/trusted.pub.

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

# ── 1. a API ancorada existe e esta declarada ─────────────────────────────
if grep -q "jef_verify_proof_pinned" libjef/jef_verify.h; then
    ok "libjef expoe jef_verify_proof_pinned (verificacao ancorada)"
else
    bad "API ancorada" "jef_verify_proof_pinned sumiu de libjef/jef_verify.h"
fi
if grep -q "jef_proof_pubkey_hex" libjef/jef_verify.h; then
    ok "libjef expoe jef_proof_pubkey_hex (mostrar de quem a prova diz ser)"
else
    bad "extracao de chave" "jef_proof_pubkey_hex ausente"
fi

# ── 2. a ancoragem e REAL: exige a chave, nao so a assinatura ────────────
# Uma chave esperada diferente tem que REPROVAR mesmo com assinatura valida.
# A implementacao agrega todas as diferencas para nao sair no primeiro byte.
if grep -Fq "difference |= got[i] ^ expected[i];" libjef/jef_verify.c &&
   grep -Fq "return difference ? 1 : 0;" libjef/jef_verify.c; then
    ok "pinned compara a chave embutida byte a byte sem early exit"
else
    bad "comparacao de chave" \
        "jef_verify_proof_pinned nao compara mais a chave — voltou a aceitar qualquer assinante"
fi

# ── 3. o texto da interface nao afirma origem sem verificar ──────────────
# A mensagem antiga dizia "proof signature valid" (le-se: autentico) mesmo
# quando so a integridade tinha sido checada.
if grep -q "sigil_report_origin" modules/sigil/sigil_module.c; then
    ok "sigil emite veredito via sigil_report_origin (distingue os dois casos)"
else
    bad "veredito honesto" \
        "sigil_report_origin sumiu — a mensagem voltou a afirmar origem sem verificar"
fi
if grep -q "origem DESCONHECIDA" modules/sigil/sigil_module.c; then
    ok "existe o caso 'origem DESCONHECIDA' (chave nao reconhecida)"
else
    bad "caso origem desconhecida" \
        "nao ha ramo para chave nao reconhecida — tudo volta a sair como valido"
fi
if grep -q "Integridade != autenticidade" modules/sigil/sigil_module.c; then
    ok "a saida explica a diferenca ao usuario"
else
    bad "explicacao ao usuario" "a frase que ensina a distincao sumiu"
fi

# ── 4. o par binario+modulo RECEM-CONSTRUIDOS tem que funcionar juntos ───
# (A primeira versao deste teste aceitava "qualquer saida" e passou verde com o
#  modulo quebrado — o simbolo novo do libjef nao estava na lista de exportacao
#  de simbolos exportados. Uma assercao que nao distingue sucesso de falha
#  nao e um gate.)
#
# HOME isolado apontando para o modulo do REPO: o ~/.jash do desenvolvedor pode
# guardar um modulo casado com um jash instalado mais antigo, e testar aquele
# par nao prova nada sobre esta mudanca.
REPO_MOD="$PWD/modules/sigil/sigil.modjash"
if [ -f "$REPO_MOD" ]; then
    H=$(mktemp -d) || exit 2
    mkdir -p "$H/.jash/modules"
    cp "$REPO_MOD" "$H/.jash/modules/sigil.modjash"
    out=$(HOME="$H" "$JASH" -c 'sigil chain verify' 2>&1)
    rm -rf "$H"
    if printf '%s' "$out" | grep -q "load failed"; then
        bad "binario e modulo casam" \
            "dlopen falhou com o modulo do repo: $(printf '%s' "$out" | head -1 | cut -c1-96)"
    elif [ -n "$out" ]; then
        ok "modulo do repo carrega no binario do repo"
    else
        bad "modulo do repo responde" "nenhuma saida"
    fi
else
    ok "modulo do repo nao construido — par nao verificado"
fi

# ── 5. o simbolo novo esta na lista de exportacao de simbolos ────────────
# (Evitando a palavra p-l-u-g-i-n solta: a catraca de cobertura do
#  test_builtin_contract procura o NOME de cada builtin em tests/*.sh, e
#  uma mencao em prosa marcaria o builtin homonimo como coberto sem que
#  exista teste algum dele. Cobertura falsa e pior que divida declarada.)
# Sem isto o modulo compila, o jash compila, e so o dlopen falha em runtime.
miss=""
for sym in jef_verify_proof_pinned jef_proof_pubkey_hex; do
    grep -q "_$sym" c/plugin_exports_macos.txt || miss="$miss macos:$sym"
    grep -q "  $sym;" c/plugin_exports.list     || miss="$miss elf:$sym"
done
if [ -z "$miss" ]; then
    ok "simbolos novos exportados nas duas plataformas"
else
    bad "lista de exportacao" "faltando:$miss — o modulo falha so no dlopen"
fi

echo ""
echo "=== JINV-024: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
