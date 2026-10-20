#!/usr/bin/env bash
# JINV-112/JINV-171 — verify e recovery tem que concordar sobre um ledger real.
#
# A cadeia e um registro MISTO e sempre foi:
#   nivel LOG (c/module/sigil.c)   hash = 64 zeros, tag= texto livre
#   nivel SIGN/SEAL (o modulo)     hash = BLAKE3 real, tag= 64 hex
#
# O escritor sabia (ancora em scan.last_s7, valida com sigil_hex64).
# O scanner sabia (JINV-103/108 classificam a linha de hash-zero como
# `unattested` e dizem que ela nao pode ancorar prev_hash).
# O verificador nao sabia: comecava em expected_seq = 0 e recalculava o hash de
# TODA linha. Resultado: `sigil chain verify` nunca deu verde, em nenhum SO, e o
# produto vendia prova que nao rodava.
#
# Este gate cobre as seis asercoes que a correcao precisa sustentar, e cada
# uma morre se o comportamento for invertido:
#   1. ledger misto (LOG + atestadas) verifica
#   2. o relatorio SEPARA atestado de contado — um verde que somasse os dois
#      mentiria sobre o tamanho da prova
#   3. seq de fallback LOG nao altera o estado da cadeia atestada
#   4. a ordem atestada e monotonica, nao necessariamente continua
#   5. retrocesso de seq em entrada ATESTADA continua REPROVANDO
#   6. hash S7 zerado com tag criptografica continua sendo ATESTADO DANIFICADO
#
# F E R M I ∞ H A R T

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
runs() { [ -x "$1" ] && JASH_SIGIL=0 "$1" -c 'exit 0' >/dev/null 2>&1; }
prereq_fail() { printf 'FAIL  prerequisite: %s\n' "$1" >&2; exit 1; }

if [ "${JASH+x}" = x ]; then
    [ -n "$JASH" ] || prereq_fail "JASH foi fornecido vazio"
    runs "$JASH" || prereq_fail "JASH fornecido nao executa: $JASH"
else
    JASH="$ROOT/build/jash"
    if ! runs "$JASH"; then
        JASH="$ROOT/build-speed/jash-release"
        runs "$JASH" || prereq_fail "nenhum Jash requerido executa"
    fi
fi

# The test enters its sandbox below, so preserve the exact selected executable
# as an absolute path. An explicit JASH is never replaced after this point.
case "$JASH" in
  /*) ;;
  *) JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")" ;;
esac

REQUIRED_GIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null)"
[ -n "$REQUIRED_GIT" ] || prereq_fail "nao foi possivel determinar o HEAD requerido"
WHOAMI="$("$JASH" --whoami 2>/dev/null)" || prereq_fail "JASH nao responde --whoami: $JASH"
case "$WHOAMI" in
  *" git="*" modabi="*) ;;
  *) prereq_fail "JASH nao fornece identidade valida: $JASH" ;;
esac
JASH_GIT="${WHOAMI#* git=}"
JASH_GIT="${JASH_GIT%% *}"
JASH_GIT="${JASH_GIT%+dirty}"
[ "$JASH_GIT" = "$REQUIRED_GIT" ] || \
    prereq_fail "JASH obsoleto: reporta $JASH_GIT, requer $REQUIRED_GIT ($JASH)"
while IFS= read -r source; do
    [ -n "$source" ] || continue
    case "$source" in c/*.c|c/*.h) ;; *) continue ;; esac
    [ "$JASH" -nt "$ROOT/$source" ] || \
        prereq_fail "JASH obsoleto: nao foi reconstruido apos $source"
done < <(printf '%s\n' c/module/sigil.c c/builtins/sigil.c c/module/loader.c \
                         c/module/loader.h c/module/modjash.h;
         git -C "$ROOT" diff --name-only HEAD -- ':(glob)c/**/*.[ch]')

ok=0; fail=0
pass() { ok=$((ok+1));   printf '  \033[32mok\033[0m   %s\n' "$1"; }
no()   { fail=$((fail+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; [ -n "${2:-}" ] && printf '       %s\n' "$2"; }

# Sandbox com HOME proprio: o ledger de teste nunca toca o do operador.
SB="$(mktemp -d "${TMPDIR:-/tmp}/jinv112.XXXXXX")"
trap 'rm -rf "$SB"' EXIT

# Build from the owned source on every run. Using the checked-in .modjash here
# let this regression pass against stale verifier code while the source under
# review was never loaded.
MODSRC="$SB/module-src/sigil"
mkdir -p "$MODSRC"
ln -s "$ROOT/modules/sigil/sigil_module.c" "$MODSRC/sigil_module.c"
ln -s "$ROOT/modules/sigil/manifest.toml" "$MODSRC/manifest.toml"
if ! HOME="$SB" bash "$ROOT/modules/build_module.sh" "$MODSRC" >"$SB/module-build.log" 2>&1; then
    no "rebuild isolado do modulo Sigil" "$(tail -20 "$SB/module-build.log" | tr '\n' ' ')"
    exit 1
fi

# Gera um ledger com as duas naturezas de registro. As entradas atestadas sao
# escritas pelo proprio jash (JASH_SIGIL=2) para que hash e ratchet sejam reais
# — um ledger sintetico provaria apenas que o parser aceita o que eu inventei.
cd "$SB" || exit 1
for i in 1 2 3 4 5; do
    if ! HOME="$SB" JASH_SIGIL=2 JASH_NUDGE=0 JASH_BRIEF=0 \
         "$JASH" -c "echo entrada-$i" >/dev/null 2>&1; then
        no "Jash requerido carrega o modulo Sigil atual" "falhou ao gerar entrada-$i: $JASH"
        exit 1
    fi
done

CHAIN="$(ls -t "$SB"/.jash/chain*.log 2>/dev/null | head -1)"
if [ -z "$CHAIN" ] || [ ! -s "$CHAIN" ]; then
    no "modulo Sigil reconstruido foi carregado" "JASH_SIGIL=2 nao produziu cadeia"
    exit 1
fi

v() { HOME="$SB" JASH_NUDGE=0 JASH_BRIEF=0 "$JASH" -c 'sigil chain verify' 2>&1; }
v_json() { HOME="$SB" JASH_NUDGE=0 JASH_BRIEF=0 "$JASH" -c 'sigil chain verify --json' 2>&1; }
v_range() { HOME="$SB" JASH_NUDGE=0 JASH_BRIEF=0 "$JASH" -c "sigil chain verify $1..$1" 2>&1; }

# `sigil chain verify` GRAVA uma entrada na propria cadeia (auto-referencia,
# medido: 3 comandos -> 3 linhas, apos um verify -> 4). Sem restaurar entre as
# asercoes, cada uma herda o efeito das anteriores e o gate mede acumulacao em
# vez do que declara. Snapshot antes, restaura antes de cada caso.
# O snapshot inclui as CHAVES DE RATCHET, nao so a cadeia: o ratchet avanca a
# cada verify (auto-referencia) e nao rebobina — restaurar so o log deixaria o
# contador adiante das entradas, e a checagem do JINV-031 reportaria
# "LOG TRUNCADO" num ledger que este gate acabou de montar.
SNAPD="$SB/snapshot"
mkdir -p "$SNAPD"
snap_take()  { cp "$CHAIN" "$SNAPD/chain"; cp "$SB"/.jash/ratchet*.key "$SNAPD/" 2>/dev/null; :; }
reset_chain() { cp "$SNAPD/chain" "$CHAIN"
                for k in "$SNAPD"/ratchet*.key; do
                    [ -e "$k" ] && cp "$k" "$SB/.jash/$(basename "$k")"
                done; :; }
snap_take
SNAP="$SNAPD/chain"

Z=0000000000000000000000000000000000000000000000000000000000000000
LAST_SEQ="$(awk 'END{print $1}' "$SNAP")"
DAMAGED_SEQ="$(awk 'NR == 2 {print $1}' "$SNAP")"
log_line() {   # log_line <seq> <marcador>
    printf '%s 1785900042.000000000 %s tag=start rc=0 cwd=%s repo=deadbeef dur=0 sess=- cmd=echo %s\n' "$1" "$Z" "$SB" "$2" >> "$CHAIN"
}

# Reuse the real hashes/tags exactly as emitted, changing only the sequence
# field (which is not part of the hash payload). This reproduces the observed
# mixed ledger: attested S7 431, fallback LOG 255, attested S7 432.
mixed_rollback_fixture() {
    awk -v z="$Z" -v sb="$SB" '
        NR == 1 {
            sub(/^[0-9]+/, "431"); print
            printf "255 1785900042.000000000 %s tag=start rc=0 cwd=%s repo=deadbeef dur=0 sess=- cmd=echo fallback-log\n", z, sb
            next
        }
        { sub(/^[0-9]+/, 430 + NR); print }
    ' "$SNAP" > "$CHAIN"
}

# Same sequence rollback, but the 255 record remains genuinely attested.
attested_rollback_fixture() {
    awk '
        NR == 1 { sub(/^[0-9]+/, "431"); print; next }
        NR == 2 { sub(/^[0-9]+/, "255"); print; next }
        { sub(/^[0-9]+/, 430 + NR); print }
    ' "$SNAP" > "$CHAIN"
}

# Increasing gaps are permitted: the ratchet witnesses entry count and the
# attested sequence field witnesses ordering, not contiguous allocation.
attested_gap_fixture() {
    awk '{ sub(/^[0-9]+/, 429 + NR * 2); print }' "$SNAP" > "$CHAIN"
}

# Damage only the S7 hash. Its original 64-hex ratchet tag and every other
# byte remain, so treating this as a core fallback would hide corruption.
zeroed_attested_hash_fixture() {
    awk -v z="$Z" 'NR == 2 {old = $3; sub(old, z)} {print}' "$SNAP" > "$CHAIN"
}

# ── 1. o ledger recem-criado verifica ────────────────────────────────────────
out="$(v)"
case "$out" in
  *"chain intact"*) pass "ledger atestado verifica" ;;
  *) no "ledger atestado verifica" "$(printf '%s' "$out" | tail -1)" ;;
esac

# ── 2. ledger MISTO: o caso que reprovava TUDO antes da correcao ─────────────
reset_chain; log_line "$((LAST_SEQ + 1))" log-record
out="$(v)"
case "$out" in
  *"chain intact"*) pass "ledger MISTO (LOG + atestadas) verifica" ;;
  *) no "ledger MISTO verifica" "$(printf '%s' "$out" | tail -1)" ;;
esac

# ── 3. o relatorio SEPARA atestado de contado ────────────────────────────────
# Um verde que somasse os dois mentiria sobre o tamanho da prova. Mesmo tom do
# `sigil ratchet rekey`: o nao-atestado NAO e promovido a verificado.
case "$out" in
  *"NAO atestados"*|*"não atestados"*) pass "relatorio separa atestado de contado" ;;
  *) no "relatorio separa atestado de contado" "$(printf '%s' "$out" | tail -1)" ;;
esac

# ── 4. seq de fallback LOG nao participa da ordem atestada ───────────────────
reset_chain; log_line 1 reinicio
out="$(v)"
case "$out" in
  *"chain intact"*) pass "seq de fallback LOG nao altera ordem atestada" ;;
  *) no "seq de fallback LOG nao altera ordem atestada" "$(printf '%s' "$out" | tail -2 | tr '\n' ' ')" ;;
esac

# ── 5. fallback LOG retrocede, mas a cadeia atestada continua 431 -> 432 ─────
reset_chain; mixed_rollback_fixture
out="$(v)"
case "$out" in
  *"chain intact"*) pass "texto classifica fallback LOG antes do estado atestado" ;;
  *) no "texto aceita S7 431, LOG 255, S7 432" "$(printf '%s' "$out" | tail -2 | tr '\n' ' ')" ;;
esac

reset_chain; mixed_rollback_fixture
out="$(v_json)"
case "$out" in
  *'"seq_continuity"'*) no "JSON nomeia monotonicidade atestada com precisao" "$(printf '%s' "$out" | tail -1)" ;;
  *'"valid":true'*'"entries_unattested":1'*'"attested_seq_monotonicity":"pass"'*) pass "JSON classifica fallback LOG antes do estado atestado" ;;
  *) no "JSON aceita S7 431, LOG 255, S7 432" "$(printf '%s' "$out" | tail -1)" ;;
esac

# ── 6. a garantia declarada e monotonicidade, nao continuidade ───────────────
reset_chain; attested_gap_fixture
out="$(v)"
case "$out" in
  *"chain intact"*) pass "texto aceita lacunas atestadas crescentes" ;;
  *) no "texto aplica monotonicidade atestada" "$(printf '%s' "$out" | tail -1)" ;;
esac

reset_chain; attested_gap_fixture
out="$(v_json)"
case "$out" in
  *'"valid":true'*'"attested_seq_monotonicity":"pass"'*) pass "JSON declara monotonicidade atestada" ;;
  *) no "JSON aplica monotonicidade atestada" "$(printf '%s' "$out" | tail -1)" ;;
esac

# ── 7. o mesmo retrocesso em entrada ATESTADA continua reprovando ────────────
reset_chain; attested_rollback_fixture
out="$(v)"
case "$out" in
  *RETROCEDEU*|*"seq went BACKWARDS"*) pass "texto reprova retrocesso atestado" ;;
  *) no "texto reprova retrocesso atestado" "$(printf '%s' "$out" | tail -1)" ;;
esac

reset_chain; attested_rollback_fixture
out="$(v_json)"
case "$out" in
  *'"valid":false'*'"failure":"seq_rollback"'*) pass "JSON reprova retrocesso atestado" ;;
  *) no "JSON reprova retrocesso atestado" "$(printf '%s' "$out" | tail -1)" ;;
esac

# ── 8. hash S7 zerado nao pode virar fallback LOG ────────────────────────────
reset_chain; zeroed_attested_hash_fixture
out="$(v)"
case "$out" in
  *"CHAIN BROKEN"*"$DAMAGED_SEQ"*) pass "texto completo reprova hash S7 zerado" ;;
  *) no "texto completo valida hash S7 zerado" "$(printf '%s' "$out" | tail -2 | tr '\n' ' ')" ;;
esac

reset_chain; zeroed_attested_hash_fixture
out="$(v_range "$DAMAGED_SEQ")"
case "$out" in
  *"CHAIN BROKEN"*"$DAMAGED_SEQ"*) pass "texto por faixa reprova hash S7 zerado" ;;
  *) no "texto por faixa valida hash S7 zerado" "$(printf '%s' "$out" | tail -2 | tr '\n' ' ')" ;;
esac

reset_chain; zeroed_attested_hash_fixture
out="$(v_json)"
case "$out" in
  *'"valid":false'*'"failure":"hash_mismatch"'*"\"at_seq\":$DAMAGED_SEQ"*) pass "JSON reprova hash S7 zerado" ;;
  *) no "JSON valida hash S7 zerado" "$(printf '%s' "$out" | tail -1)" ;;
esac

# ── 9. identidade do ledger fica fixa mesmo se HOME mudar no processo ────────
install_current_module() {
    mkdir -p "$1/.jash/modules"
    cp "$MODSRC/sigil.modjash" "$1/.jash/modules/"
    chmod 0700 "$1/.jash" "$1/.jash/modules"
    chmod 0600 "$1/.jash/modules/sigil.modjash"
}
run_sigil() {
    local home=$1 repo=$2 level=$3 command=$4
    (cd "$repo" && HOME="$home" JASH_SIGIL="$level" JASH_NUDGE=0 \
        JASH_BRIEF=0 JASH_LOOPGUARD=off "$JASH" -c "$command")
}
run_sigil_manifest_fault() {
    local home=$1 repo=$2 fault=$3 command=$4
    (cd "$repo" && HOME="$home" JASH_SIGIL=0 JASH_NUDGE=0 JASH_BRIEF=0 \
        JASH_LOOPGUARD=off JASH_SIGIL_TEST_MANIFEST_FAIL="$fault" \
        "$JASH" -c "$command")
}
run_sigil_quarantine_fault() {
    local home=$1 repo=$2 fault=$3 manifest_fault=$4 command=$5
    (cd "$repo" && HOME="$home" JASH_SIGIL=0 JASH_NUDGE=0 JASH_BRIEF=0 \
        JASH_LOOPGUARD=off JASH_SIGIL_TEST_QUARANTINE_FAIL="$fault" \
        JASH_SIGIL_TEST_MANIFEST_FAIL="$manifest_fault" \
        "$JASH" -c "$command")
}
run_sigil_proof_fault() {
    local home=$1 repo=$2 fault=$3 command=$4
    (cd "$repo" && HOME="$home" JASH_SIGIL=0 JASH_NUDGE=0 JASH_BRIEF=0 \
        JASH_LOOPGUARD=off JASH_SIGIL_TEST_PROOF_FAIL="$fault" \
        "$JASH" -c "$command")
}
# Scoped recovery fixtures need a real repository marker. An empty .git
# directory is deliberately not a repository for the shared identity engine.
init_repo() {
    mkdir -p "$1" && git -C "$1" init -q ||
        prereq_fail "nao foi possivel inicializar repo de teste: $1"
}
single_repo_chain() {
    local f
    local chains=()
    for f in "$1"/.jash/chain*.log; do
        [ -f "$f" ] && chains+=("$f")
    done
    if [ "${#chains[@]}" -ne 1 ]; then
        printf 'FAIL  fixture: expected one scoped chain in %s, found %s\n' \
            "$1/.jash" "${#chains[@]}" >&2
        return 1
    fi
    case "${chains[0]}" in
      */chain-*.log) printf '%s\n' "${chains[0]}" ;;
      *) printf 'FAIL  fixture: expected scoped chain, got %s\n' "${chains[0]}" >&2
         return 1 ;;
    esac
}

PIN_A="$SB/home-pin-a"
PIN_B="$SB/home-pin-b"
PIN_R="$SB/repo-pin"
init_repo "$PIN_R"
mkdir -p "$PIN_B"
install_current_module "$PIN_A"
printf 'echo before-home-change\nexport HOME=%s\necho after-home-change\nsigil off\nsigil proof emit\n' "$PIN_B" |
    (cd "$PIN_R" && HOME="$PIN_A" JASH_SIGIL=2 JASH_NUDGE=0 JASH_BRIEF=0 \
        JASH_LOOPGUARD=off "$JASH") >/dev/null 2>&1
PIN_CHAIN="$(single_repo_chain "$PIN_A")"
pin_verdict="$(run_sigil "$PIN_A" "$PIN_R" 0 'sigil verify --json' 2>/dev/null || true)"
case "$pin_verdict" in
  *'"valid":true'*) pass "mudanca de HOME nao divide cadeia/hash" ;;
  *) no "mudanca de HOME dividiu cadeia/hash" "$pin_verdict" ;;
esac
if grep -q 'before-home-change' "$PIN_CHAIN" && grep -q 'after-home-change' "$PIN_CHAIN"; then
    pass "stream fixo conserva entradas antes/depois do export HOME"
else
    no "stream fixo perdeu entrada apos export HOME" "$PIN_CHAIN"
fi
if [ -s "$PIN_A/.jash/keys/ed25519.seed" ] &&
   [ -s "$PIN_A/.jash/session.jashproof" ]; then
    pass "chave Ed25519 e caminho padrao/automatico da prova ficam no HOME original"
else
    no "chave/prova automatica escaparam da identidade original" \
       "key=$PIN_A/.jash/keys/ed25519.seed proof=$PIN_A/.jash/session.jashproof"
fi
if pin_proof_verdict="$(run_sigil "$PIN_A" "$PIN_R" 0 'sigil proof verify' 2>&1)"; then
    case "$pin_proof_verdict" in
      *"CONHECIDA"*"chain_tip matches"*) pass "assinatura/prova verificam na identidade original" ;;
      *) no "prova original nao confirmou identidade e chain tip" "$pin_proof_verdict" ;;
    esac
else
    no "assinatura/prova falharam na identidade original" "$pin_proof_verdict"
fi
set -- "$PIN_B"/.jash/chain*.log "$PIN_B"/.jash/ratchet*.key \
       "$PIN_B"/.jash/chain*.scan "$PIN_B"/.jash/chain*.ckptkey \
       "$PIN_B"/.jash/chain*.txnlock \
       "$PIN_B"/.jash/keys/ed25519.seed "$PIN_B"/.jash/session.jashproof \
       "$PIN_B"/.jash/session.jashproof.dsse "$PIN_B"/.jash/quarantine
if [ ! -e "$1" ] && [ ! -e "$2" ] && [ ! -e "$3" ] && [ ! -e "$4" ] &&
   [ ! -e "$5" ] && [ ! -e "$6" ] && [ ! -e "$7" ] && [ ! -e "$8" ] &&
   [ ! -e "$9" ]; then
    pass "HOME novo nao recebe ledger, chave, prova, checkpoint ou quarentena"
else
    no "HOME novo recebeu estado da identidade antiga" "$(printf '%s ' "$@")"
fi

# ── 10. apply explicito recupera mismatch que o scanner estrutural nao ve ────
REC_H="$SB/home-recovery"
REC_R="$SB/repo-recovery"
init_repo "$REC_R"
install_current_module "$REC_H"
for i in 1 2 3 4; do
    run_sigil "$REC_H" "$REC_R" 2 "echo recovery-$i" >/dev/null 2>&1
done
REC_CHAIN="$(single_repo_chain "$REC_H")"
set -- "$REC_H"/.jash/ratchet-*.key
REC_RATCHET=$1
cp "$REC_CHAIN" "$SB/recovery-chain.before"
cp "$REC_RATCHET" "$SB/recovery-ratchet.before"
awk 'NR == 2 {old = $3; first = substr(old, 1, 1); repl = first == "0" ? "1" : "0";
                    sub(old, repl substr(old, 2))} {print}' \
    "$SB/recovery-chain.before" > "$REC_CHAIN"
cp "$REC_CHAIN" "$SB/recovery-chain.damaged"
REC_DAMAGED_SHA="$(sha256sum "$REC_CHAIN")"
REC_DAMAGED_SHA="${REC_DAMAGED_SHA%% *}"

rec_status="$(run_sigil "$REC_H" "$REC_R" 0 'sigil quarantine status --json' 2>/dev/null || true)"
case "$rec_status" in
  *'"active":null'*) pass "status estrutural permanece null no hash mismatch" ;;
  *) no "status promoveu mismatch a erro estrutural" "$rec_status" ;;
esac

rec_apply="$(run_sigil "$REC_H" "$REC_R" 0 'sigil quarantine apply --json' 2>/dev/null || true)"
case "$rec_apply" in
  *'"applied":1'*'"failed":0'*'"active_reset":true'*'"tail_salvaged":false'*)
      pass "apply explicito quarentena mismatch inteiro" ;;
  *) no "apply nao recuperou hash mismatch" "$rec_apply" ;;
esac

QCHAIN=''; QRATCHET=''; MANIFEST=''
for p in "$REC_H"/.jash/quarantine/hash_mismatch-*; do
    case "$p" in
      *.manifest.json) MANIFEST=$p ;;
      *ratchet-*.key) QRATCHET=$p ;;
      *chain-*.log) QCHAIN=$p ;;
    esac
done
if [ -n "$QCHAIN" ] && cmp -s "$SB/recovery-chain.damaged" "$QCHAIN" &&
   [ -n "$QRATCHET" ] && cmp -s "$SB/recovery-ratchet.before" "$QRATCHET"; then
    pass "quarentena preserva cadeia completa e ratchet scoped byte a byte"
else
    no "quarentena truncou/promoveu bytes" "chain=$QCHAIN ratchet=$QRATCHET"
fi
if [ -n "$MANIFEST" ] && grep -q '"reason":"hash_mismatch"' "$MANIFEST" &&
   grep -q "\"sha256\":\"$REC_DAMAGED_SHA\"" "$MANIFEST"; then
    pass "manifest identifica hash_mismatch e SHA da cadeia completa"
else
    no "manifest de hash_mismatch/SHA ausente" "$MANIFEST"
fi

run_sigil "$REC_H" "$REC_R" 2 'echo fresh-after-quarantine' >/dev/null 2>&1
fresh_verdict="$(run_sigil "$REC_H" "$REC_R" 0 'sigil verify --json' 2>/dev/null || true)"
case "$fresh_verdict" in
  *'"valid":true'*) pass "nova cadeia/ratchet verifica apos quarentena" ;;
  *) no "nova cadeia nao verifica apos quarentena" "$fresh_verdict" ;;
esac

HEALTHY_CHAIN="$(single_repo_chain "$REC_H")"
set -- "$REC_H"/.jash/ratchet-*.key
HEALTHY_RATCHET=$1
healthy_chain_sha="$(sha256sum "$HEALTHY_CHAIN")"; healthy_chain_sha="${healthy_chain_sha%% *}"
healthy_ratchet_sha="$(sha256sum "$HEALTHY_RATCHET")"; healthy_ratchet_sha="${healthy_ratchet_sha%% *}"
healthy_apply="$(run_sigil "$REC_H" "$REC_R" 0 'sigil quarantine apply --json' 2>/dev/null || true)"
healthy_chain_after="$(sha256sum "$HEALTHY_CHAIN")"; healthy_chain_after="${healthy_chain_after%% *}"
healthy_ratchet_after="$(sha256sum "$HEALTHY_RATCHET")"; healthy_ratchet_after="${healthy_ratchet_after%% *}"
if [ "$healthy_chain_sha" = "$healthy_chain_after" ] &&
   [ "$healthy_ratchet_sha" = "$healthy_ratchet_after" ]; then
    case "$healthy_apply" in
      *'"applied":0'*'"failed":0'*) pass "apply saudavel nao altera cadeia/ratchet" ;;
      *) no "apply saudavel reportou mutacao" "$healthy_apply" ;;
    esac
else
    no "apply saudavel foi destrutivo" "$healthy_apply"
fi

# JINV-171: o scanner estrutural nao enxerga um prefixo criptograficamente
# valido cujo witness esta adiante. O verifier chama isso `log_truncated`; o
# apply precisa preservar o par inteiro e recomecar, nunca reportar no-op verde.
TR_H="$SB/home-count-truncated"
TR_R="$SB/repo-count-truncated"
init_repo "$TR_R"
install_current_module "$TR_H"
for i in 1 2 3 4; do
    run_sigil "$TR_H" "$TR_R" 2 "echo count-truncated-$i" >/dev/null 2>&1
done
TR_CHAIN="$(single_repo_chain "$TR_H")"
set -- "$TR_H"/.jash/ratchet-*.key
TR_RATCHET=$1
cp "$TR_RATCHET" "$SB/count-truncated-ratchet.before"
TR_LINES="$(wc -l < "$TR_CHAIN")"
sed -n "1,$((TR_LINES - 2))p" "$TR_CHAIN" > "$TR_CHAIN.short"
mv "$TR_CHAIN.short" "$TR_CHAIN"
cp "$TR_CHAIN" "$SB/count-truncated-chain.before"
truncated_verdict="$(run_sigil "$TR_H" "$TR_R" 0 \
    'sigil verify --json' 2>/dev/null || true)"
case "$truncated_verdict" in
  *'"valid":false'*'"failure":"log_truncated"'*'"missing":2'*)
      pass "verifier classifica duas entradas ausentes" ;;
  *) no "verifier nao mediu truncamento" "$truncated_verdict" ;;
esac
truncated_apply="$(run_sigil "$TR_H" "$TR_R" 0 \
    'sigil quarantine apply --json' 2>/dev/null || true)"
TR_QCHAIN=''; TR_QRATCHET=''; TR_MANIFEST=''
for p in "$TR_H"/.jash/quarantine/log_truncated-*; do
    case "$p" in
      *.manifest.json) TR_MANIFEST=$p ;;
      *ratchet-*.key) TR_QRATCHET=$p ;;
      *chain-*.log) TR_QCHAIN=$p ;;
    esac
done
if printf '%s' "$truncated_apply" | grep -q \
      '"applied":1.*"failed":0.*"active_reset":true' &&
   [ -n "$TR_QCHAIN" ] &&
   cmp -s "$SB/count-truncated-chain.before" "$TR_QCHAIN" &&
   [ -n "$TR_QRATCHET" ] &&
   cmp -s "$SB/count-truncated-ratchet.before" "$TR_QRATCHET" &&
   [ -n "$TR_MANIFEST" ] &&
   grep -q '"reason":"log_truncated"' "$TR_MANIFEST"; then
    pass "apply preserva par truncado e publica reset fresco"
else
    no "apply ignorou ou reescreveu log truncado" \
       "$truncated_apply chain=$TR_QCHAIN ratchet=$TR_QRATCHET manifest=$TR_MANIFEST"
fi
truncated_fresh="$(run_sigil "$TR_H" "$TR_R" 0 \
    'sigil verify --json' 2>/dev/null || true)"
case "$truncated_fresh" in
  *'"valid":true'*) pass "par fresco verifica depois de log truncado" ;;
  *) no "reset de log truncado nao verifica" "$truncated_fresh" ;;
esac

# ── 11. writer com inode antigo reabre a cadeia ativa apos quarantine ────────
CON_H="$SB/home-concurrent-quarantine"
CON_R="$SB/repo-concurrent-quarantine"
CON_READY="$SB/concurrent-writer.ready"
init_repo "$CON_R"
install_current_module "$CON_H"
for i in 1 2 3 4; do
    run_sigil "$CON_H" "$CON_R" 2 "echo concurrent-seed-$i" >/dev/null 2>&1
done
CON_CHAIN="$(single_repo_chain "$CON_H")"
cp "$CON_CHAIN" "$SB/concurrent-chain.before"
awk 'NR == 2 {old = $3; first = substr(old, 1, 1); repl = first == "0" ? "1" : "0";
                    sub(old, repl substr(old, 2))} {print}' \
    "$SB/concurrent-chain.before" > "$CON_CHAIN"
printf 'echo writer-opened-old-inode\ntouch %s\nsleep 2\necho writer-after-quarantine\n' "$CON_READY" |
    (cd "$CON_R" && HOME="$CON_H" JASH_SIGIL=2 JASH_NUDGE=0 JASH_BRIEF=0 \
        JASH_LOOPGUARD=off "$JASH") >/dev/null 2>&1 &
CON_PID=$!
for _ in $(seq 1 200); do
    [ -e "$CON_READY" ] && break
    sleep 0.01
done
if [ ! -e "$CON_READY" ]; then
    no "writer concorrente abriu inode antigo" "ready timeout"
else
    con_apply="$(run_sigil "$CON_H" "$CON_R" 0 'sigil quarantine apply --json' 2>/dev/null || true)"
    case "$con_apply" in
      *'"applied":1'*'"failed":0'*) pass "quarantine serializa com writer de inode antigo" ;;
      *) no "quarantine concorrente falhou" "$con_apply" ;;
    esac
fi
wait "$CON_PID" || true
CON_ACTIVE="$(single_repo_chain "$CON_H")"
CON_QCHAIN=''
for p in "$CON_H"/.jash/quarantine/hash_mismatch-*chain-*.log; do
    [ -f "$p" ] && { CON_QCHAIN=$p; break; }
done
if grep -q 'writer-after-quarantine' "$CON_ACTIVE" &&
   [ -n "$CON_QCHAIN" ] && ! grep -q 'writer-after-quarantine' "$CON_QCHAIN"; then
    pass "writer em espera reabre ativo e nunca anexa inode em quarentena"
else
    no "writer anexou inode em quarentena" "active=$CON_ACTIVE quarantined=$CON_QCHAIN"
fi
con_verdict="$(run_sigil "$CON_H" "$CON_R" 0 'sigil verify --json' 2>/dev/null || true)"
case "$con_verdict" in
  *'"valid":true'*) pass "cadeia ativa verifica apos corrida quarantine/writer" ;;
  *) no "cadeia ativa quebrou apos corrida quarantine/writer" "$con_verdict" ;;
esac

# ── 12. falha de write/close do manifest faz rollback integral ───────────────
for fault in write close; do
    FAIL_H="$SB/home-manifest-$fault"
    FAIL_R="$SB/repo-manifest-$fault"
    init_repo "$FAIL_R"
    install_current_module "$FAIL_H"
    for i in 1 2 3; do
        run_sigil "$FAIL_H" "$FAIL_R" 2 "echo manifest-$fault-$i" >/dev/null 2>&1
    done
    FAIL_CHAIN="$(single_repo_chain "$FAIL_H")"
    set -- "$FAIL_H"/.jash/ratchet-*.key
    FAIL_RATCHET=$1
    cp "$FAIL_CHAIN" "$SB/manifest-$fault-chain.before"
    cp "$FAIL_RATCHET" "$SB/manifest-$fault-ratchet.before"
    awk 'NR == 2 {old = $3; first = substr(old, 1, 1); repl = first == "0" ? "1" : "0";
                        sub(old, repl substr(old, 2))} {print}' \
        "$SB/manifest-$fault-chain.before" > "$FAIL_CHAIN"
    cp "$FAIL_CHAIN" "$SB/manifest-$fault-chain.damaged"
    fault_apply="$(run_sigil_manifest_fault "$FAIL_H" "$FAIL_R" "$fault" \
        'sigil quarantine apply --json' 2>/dev/null || true)"
    case "$fault_apply" in
      *'"applied":0'*'"failed":1'*'"active_reset":false'*)
          pass "falha manifest $fault reprova apply" ;;
      *) no "falha manifest $fault foi aceita" "$fault_apply" ;;
    esac
    if cmp -s "$SB/manifest-$fault-chain.damaged" "$FAIL_CHAIN" &&
       cmp -s "$SB/manifest-$fault-ratchet.before" "$FAIL_RATCHET"; then
        pass "falha manifest $fault restaura cadeia e ratchet byte a byte"
    else
        no "falha manifest $fault nao fez rollback integral" "$FAIL_H/.jash"
    fi
    set -- "$FAIL_H"/.jash/quarantine/hash_mismatch-*
    if [ ! -e "$1" ]; then
        pass "falha manifest $fault nao publica artefato nao confiavel"
    else
        no "falha manifest $fault deixou artefato parcial" "$(printf '%s ' "$@")"
    fi
done

# ── 13. fallback core e attest obedecem quarantine na mesma ordem de locks ──
# O fd 9 segura o inode antigo. Quarantine entra primeiro na fila desse inode e
# ja possui o txnlock; o writer core entra depois. Sem txnlock + recheck no
# core, ele acorda apos o rename e escreve no inode que virou quarentena.
core_quarantine_race() {
    local name=$1 marker=$2 command=$3
    local home="$SB/home-core-$name" repo="$SB/repo-core-$name"
    local ready=0 qpid wpid chain active qchain apply_out
    init_repo "$repo"
    install_current_module "$home"
    for i in 1 2 3; do
        run_sigil "$home" "$repo" 2 "echo core-$name-seed-$i" >/dev/null 2>&1
    done
    chain="$(single_repo_chain "$home")"
    awk 'NR == 2 {old = $3; first = substr(old, 1, 1); repl = first == "0" ? "1" : "0";
                     sub(old, repl substr(old, 2))} {print}' "$chain" > "$chain.damaged"
    mv "$chain.damaged" "$chain"

    exec 9>>"$chain"
    flock -x 9
    run_sigil "$home" "$repo" 0 'sigil quarantine apply --json' \
        >"$SB/core-$name-apply.out" 2>&1 &
    qpid=$!
    for _ in $(seq 1 200); do
        if ! flock -n "$chain.txnlock" -c true 2>/dev/null; then ready=1; break; fi
        sleep 0.01
    done
    mv "$home/.jash/modules/sigil.modjash" "$home/.jash/modules/sigil.modjash.hidden"
    (cd "$repo" && HOME="$home" JASH_SIGIL=1 JASH_NUDGE=0 JASH_BRIEF=0 \
        JASH_LOOPGUARD=off JASH_SIGIL_ALLOW_DEGRADED=1 "$JASH" -c "$command") \
        >"$SB/core-$name-writer.out" 2>&1 &
    wpid=$!
    sleep 0.1
    flock -u 9
    exec 9>&-
    qrc=0; wrc=0
    wait "$qpid" || qrc=$?
    wait "$wpid" || wrc=$?
    mv "$home/.jash/modules/sigil.modjash.hidden" "$home/.jash/modules/sigil.modjash"

    apply_out="$(cat "$SB/core-$name-apply.out")"
    active="$(single_repo_chain "$home")"
    qchain=''
    for p in "$home"/.jash/quarantine/hash_mismatch-*chain-*.log; do
        [ -f "$p" ] && { qchain=$p; break; }
    done
    if [ "$ready" -eq 1 ] && [ "$qrc" -eq 0 ] && [ "$wrc" -eq 0 ] &&
       printf '%s' "$apply_out" | grep -q '"applied":1' &&
       grep -q "$marker" "$active" &&
       [ -n "$qchain" ] && ! grep -q "$marker" "$qchain"; then
        pass "writer core $name reabre ativo apos quarantine"
    else
        no "writer core $name escreveu inode em quarentena" \
           "ready=$ready qrc=$qrc wrc=$wrc active=$active quarantined=$qchain apply=$apply_out writer=$(cat "$SB/core-$name-writer.out")"
    fi
}

core_quarantine_race fallback core-fallback-after-quarantine \
    'echo core-fallback-after-quarantine'
core_quarantine_race attest core-attest-after-quarantine \
    'gap core-attest-after-quarantine'

# ── 14. ratchet le e publica toda a transicao sob a mesma serializacao ──────
RAT_H="$SB/home-ratchet-overlap"
RAT_R="$SB/repo-ratchet-overlap"
init_repo "$RAT_R"
install_current_module "$RAT_H"
for i in 1 2 3; do
    run_sigil "$RAT_H" "$RAT_R" 2 "echo ratchet-seed-$i" >/dev/null 2>&1
done
RAT_CHAIN="$(single_repo_chain "$RAT_H")"
exec 9>>"$RAT_CHAIN.txnlock"
flock -x 9
run_sigil "$RAT_H" "$RAT_R" 2 'echo ratchet-writer-before-transition' \
    >"$SB/ratchet-writer-before.out" 2>&1 &
RAT_W1=$!
sleep 0.1
run_sigil "$RAT_H" "$RAT_R" 0 'sigil ratchet rekey' \
    >"$SB/ratchet-transition.out" 2>&1 &
RAT_T=$!
sleep 0.1
run_sigil "$RAT_H" "$RAT_R" 2 'echo ratchet-writer-after-transition' \
    >"$SB/ratchet-writer-after.out" 2>&1 &
RAT_W2=$!
sleep 0.1
flock -u 9
exec 9>&-
RAT_W1_RC=0; RAT_T_RC=0; RAT_W2_RC=0
wait "$RAT_W1" || RAT_W1_RC=$?
wait "$RAT_T" || RAT_T_RC=$?
wait "$RAT_W2" || RAT_W2_RC=$?
RAT_BOUNDARY="$(grep 'cmd=#sigil-boundary v1' "$RAT_CHAIN" | tail -1)"
RAT_REPORTED_ENTRIES="${RAT_BOUNDARY#* prev_entries=}"
RAT_REPORTED_ENTRIES="${RAT_REPORTED_ENTRIES%% *}"
RAT_REPORTED_ADV="${RAT_BOUNDARY#* prev_advances=}"
RAT_REPORTED_ADV="${RAT_REPORTED_ADV%% *}"
RAT_ACTUAL_BEFORE="$(awk '/cmd=#sigil-boundary v1/{print n + 0; exit} / tag=/{n++}' "$RAT_CHAIN")"
lock_order_source_ok="$(python3 - "$ROOT/modules/sigil/sigil_ratchet_observed.inc" <<'PY'
import sys
s = open(sys.argv[1]).read()
f = s[s.index('static int sigil_ratchet_transition_observed('):]
f = f[:f.index('\nstatic ', 20)]
lock = f.index('chain_transaction_lock(')
print(1 if lock < f.index('ratchet_read_at(') and lock < f.index('sigil_scan_chain(') else 0)
PY
)"
if [ "$RAT_W1_RC" -eq 0 ] && [ "$RAT_T_RC" -eq 0 ] &&
   [ "$RAT_W2_RC" -eq 0 ] && [ -n "$RAT_BOUNDARY" ] &&
   [ "$lock_order_source_ok" = 1 ] &&
   [ "$RAT_REPORTED_ENTRIES" = "$RAT_ACTUAL_BEFORE" ] &&
   [ "$RAT_REPORTED_ADV" = "$RAT_ACTUAL_BEFORE" ]; then
    pass "ratchet adquire locks antes de ler estado sobreposto"
else
    no "ratchet publicou fronteira de snapshot obsoleto" \
       "rc=$RAT_W1_RC/$RAT_T_RC/$RAT_W2_RC reported_entries=$RAT_REPORTED_ENTRIES reported_adv=$RAT_REPORTED_ADV actual=$RAT_ACTUAL_BEFORE"
fi
rat_overlap_verdict="$(run_sigil "$RAT_H" "$RAT_R" 0 'sigil verify --json' 2>/dev/null || true)"
case "$rat_overlap_verdict" in
  *'"valid":true'*) pass "ratchet retém serializacao ate witness substituto duravel" ;;
  *) no "writer atravessou publicacao do witness substituto" "$rat_overlap_verdict" ;;
esac

# Exercise the real core fallback and jash_sigil_attest after a boundary. The
# LOG line between them must not become the cryptographic anchor, and the core
# ratchet round-trip must retain the boundary nonce byte-for-byte.
CORE_H="$SB/home-core-anchor-nonce"
CORE_R="$SB/repo-core-anchor-nonce"
init_repo "$CORE_R"
install_current_module "$CORE_H"
for i in 1 2 3; do
    run_sigil "$CORE_H" "$CORE_R" 2 "echo core-anchor-seed-$i" >/dev/null 2>&1
done
run_sigil "$CORE_H" "$CORE_R" 0 'sigil ratchet rekey' >/dev/null 2>&1
CORE_CHAIN="$(single_repo_chain "$CORE_H")"
set -- "$CORE_H"/.jash/ratchet-*.key
CORE_RATCHET=$1
CORE_NONCE_BEFORE="$(awk '{print $3}' "$CORE_RATCHET")"
mv "$CORE_H/.jash/modules/sigil.modjash" "$CORE_H/.jash/modules/sigil.modjash.hidden"
core_log_rc=0; core_attest_rc=0
(cd "$CORE_R" && HOME="$CORE_H" JASH_SIGIL=1 JASH_SIGIL_ALLOW_DEGRADED=1 \
    JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off \
    "$JASH" -c 'echo core-log-between') >/dev/null 2>&1 || core_log_rc=$?
(cd "$CORE_R" && HOME="$CORE_H" JASH_SIGIL=1 JASH_SIGIL_ALLOW_DEGRADED=1 \
    JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off \
    "$JASH" -c 'gap core-attest-after-log') >/dev/null 2>&1 || core_attest_rc=$?
CORE_NONCE_AFTER="$(awk '{print $3}' "$CORE_RATCHET")"
mv "$CORE_H/.jash/modules/sigil.modjash.hidden" "$CORE_H/.jash/modules/sigil.modjash"
core_anchor_verdict="$(run_sigil "$CORE_H" "$CORE_R" 0 'sigil verify --json' 2>/dev/null || true)"
if [ "$core_log_rc" -eq 0 ] && [ "$core_attest_rc" -eq 0 ] &&
   [ -n "$CORE_NONCE_BEFORE" ] && [ "$CORE_NONCE_BEFORE" = "$CORE_NONCE_AFTER" ] &&
   grep -q 'core-log-between' "$CORE_CHAIN" &&
   grep -q 'core-attest-after-log' "$CORE_CHAIN" &&
   printf '%s' "$core_anchor_verdict" | grep -q '"valid":true'; then
    pass "core attest ancora no ultimo hash atestado e preserva nonce"
else
    no "core attest ancorou LOG zero ou perdeu nonce" \
       "rc=$core_log_rc/$core_attest_rc nonce=$CORE_NONCE_BEFORE/$CORE_NONCE_AFTER verify=$core_anchor_verdict"
fi

AP_H="$SB/home-ratchet-append-fail"
AP_R="$SB/repo-ratchet-append-fail"
init_repo "$AP_R"
install_current_module "$AP_H"
for i in 1 2 3; do
    run_sigil "$AP_H" "$AP_R" 2 "echo append-fail-seed-$i" >/dev/null 2>&1
done
AP_CHAIN="$(single_repo_chain "$AP_H")"
AP_LINES_BEFORE="$(wc -l < "$AP_CHAIN")"
AP_READY="$SB/append-fail-preloaded.ready"
AP_GO="$SB/append-fail-preloaded.go"
(cd "$AP_R" && HOME="$AP_H" JASH_SIGIL=2 JASH_NUDGE=0 JASH_BRIEF=0 \
    JASH_LOOPGUARD=off "$JASH" -c \
    "touch '$AP_READY'; while [ ! -e '$AP_GO' ]; do sleep 0.02; done; echo preloaded-must-not-append") \
    >"$SB/append-fail-preloaded.out" 2>&1 &
AP_PRELOADED_PID=$!
for _ in $(seq 1 200); do
    [ -e "$AP_READY" ] && break
    sleep 0.01
done
[ -e "$AP_READY" ] || {
    kill "$AP_PRELOADED_PID" 2>/dev/null || true
    wait "$AP_PRELOADED_PID" 2>/dev/null || true
    prereq_fail "writer preloaded nao alcancou barreira antes do peer"
}
(cd "$AP_R" && HOME="$AP_H" JASH_SIGIL=2 JASH_NUDGE=0 JASH_BRIEF=0 \
    JASH_LOOPGUARD=off JASH_SIGIL_TEST_RATCHET_FAIL=append \
    "$JASH" -c 'echo ratchet-publish-will-fail') >/dev/null 2>&1 || true
AP_LINES_DAMAGED="$(wc -l < "$AP_CHAIN")"
touch "$AP_GO"
wait "$AP_PRELOADED_PID" || true
AP_LINES_PRELOADED="$(wc -l < "$AP_CHAIN")"
AP_PRELOADED_OUT="$(cat "$SB/append-fail-preloaded.out")"
ap_restart_out="$(run_sigil "$AP_H" "$AP_R" 2 \
    'echo restart-must-not-append' 2>&1 || true)"
AP_LINES_AFTER="$(wc -l < "$AP_CHAIN")"
ap_verdict="$(run_sigil "$AP_H" "$AP_R" 0 'sigil verify --json' 2>/dev/null || true)"
set -- "$AP_H"/.jash/ratchet-*.key
AP_RATCHET=$1
AP_CHAIN_BEFORE_STATUS="$(shasum -a 256 "$AP_CHAIN" | cut -d' ' -f1)"
AP_RATCHET_BEFORE_STATUS="$(shasum -a 256 "$AP_RATCHET" | cut -d' ' -f1)"
ap_status="$(run_sigil "$AP_H" "$AP_R" 0 'sigil quarantine status --json' 2>/dev/null || true)"
ap_status_check="$(printf '%s' "$ap_status" | python3 -c '
import json,sys
d=json.load(sys.stdin)
a=d.get("active") or {}
w=d.get("witness") or {}
print(a.get("reason"),w.get("position"),w.get("ratchet_advances"),
      w.get("entries"),d.get("recommended_action"))
' 2>/dev/null)"
AP_CHAIN_AFTER_STATUS="$(shasum -a 256 "$AP_CHAIN" | cut -d' ' -f1)"
AP_RATCHET_AFTER_STATUS="$(shasum -a 256 "$AP_RATCHET" | cut -d' ' -f1)"
if [ "$AP_LINES_DAMAGED" -gt "$AP_LINES_BEFORE" ] &&
   [ "$AP_LINES_PRELOADED" -eq "$AP_LINES_DAMAGED" ] &&
   [ "$AP_LINES_AFTER" -eq "$AP_LINES_DAMAGED" ] &&
   printf '%s' "$AP_PRELOADED_OUT" | grep -q 'ratchet_position_mismatch' &&
   printf '%s' "$ap_restart_out" | grep -q 'ratchet_position_mismatch' &&
   [ "$ap_status_check" = "ratchet_position_mismatch mismatch $((AP_LINES_DAMAGED - 1)) $AP_LINES_DAMAGED sigil quarantine apply --json" ] &&
   [ "$AP_CHAIN_BEFORE_STATUS" = "$AP_CHAIN_AFTER_STATUS" ] &&
   [ "$AP_RATCHET_BEFORE_STATUS" = "$AP_RATCHET_AFTER_STATUS" ] &&
   printf '%s' "$ap_verdict" | grep -q '"valid":false.*"failure":"entries_forged"'; then
    pass "falha de publish bloqueia append; status diagnostica sem mutar"
else
     no "restart atravessou witness obsoleto ou status omitiu divergencia" \
        "lines=$AP_LINES_BEFORE/$AP_LINES_DAMAGED/$AP_LINES_PRELOADED/$AP_LINES_AFTER preloaded=$AP_PRELOADED_OUT out=$ap_restart_out verify=$ap_verdict status=$ap_status_check raw=$ap_status"
fi

# O inverso (`entries_forged`) tambem e uma divergencia conclusiva de contagem.
# Preservar cadeia e witness distintos e obrigatorio para a autopsia.
set -- "$AP_H"/.jash/ratchet-*.key
AP_RATCHET=$1
cp "$AP_CHAIN" "$SB/entries-forged-chain.before"
cp "$AP_RATCHET" "$SB/entries-forged-ratchet.before"
ap_apply="$(run_sigil "$AP_H" "$AP_R" 0 \
    'sigil quarantine apply --json' 2>/dev/null || true)"
AP_QCHAIN=''; AP_QRATCHET=''; AP_MANIFEST=''
for p in "$AP_H"/.jash/quarantine/entries_forged-*; do
    case "$p" in
      *.manifest.json) AP_MANIFEST=$p ;;
      *ratchet-*.key) AP_QRATCHET=$p ;;
      *chain-*.log) AP_QCHAIN=$p ;;
    esac
done
if printf '%s' "$ap_apply" | grep -q \
      '"applied":1.*"failed":0.*"active_reset":true' &&
   [ -n "$AP_QCHAIN" ] && cmp -s "$SB/entries-forged-chain.before" "$AP_QCHAIN" &&
   [ -n "$AP_QRATCHET" ] && cmp -s "$SB/entries-forged-ratchet.before" "$AP_QRATCHET" &&
   [ -n "$AP_MANIFEST" ] && grep -q '"reason":"entries_forged"' "$AP_MANIFEST"; then
    pass "apply preserva par com entradas sem witness"
else
    no "apply ignorou ou reescreveu entries_forged" \
       "$ap_apply chain=$AP_QCHAIN ratchet=$AP_QRATCHET manifest=$AP_MANIFEST"
fi
ap_fresh="$(run_sigil "$AP_H" "$AP_R" 0 'sigil verify --json' 2>/dev/null || true)"
case "$ap_fresh" in
  *'"valid":true'*) pass "par fresco verifica depois de entries_forged" ;;
  *) no "reset de entries_forged nao verifica" "$ap_fresh" ;;
esac

# A failed replacement happens after the boundary is durable. The advanced
# outgoing witness therefore needs its own durable recovery name, while both
# this process and the next one must refuse to append through the unfinished
# boundary.
RF_H="$SB/home-ratchet-replacement-fail"
RF_R="$SB/repo-ratchet-replacement-fail"
init_repo "$RF_R"
install_current_module "$RF_H"
for i in 1 2 3; do
    run_sigil "$RF_H" "$RF_R" 2 "echo replacement-seed-$i" >/dev/null 2>&1
done
RF_CHAIN="$(single_repo_chain "$RF_H")"
set -- "$RF_H"/.jash/ratchet-*.key
RF_RATCHET=$1
RF_ADV_BEFORE="$(awk '{print $2}' "$RF_RATCHET")"
rf_transition_rc=0
(cd "$RF_R" && HOME="$RF_H" JASH_SIGIL=0 JASH_NUDGE=0 JASH_BRIEF=0 \
    JASH_LOOPGUARD=off JASH_SIGIL_TEST_RATCHET_FAIL=replacement \
    "$JASH" -c 'sigil ratchet rekey') >"$SB/replacement-fail.out" 2>&1 || \
    rf_transition_rc=$?
set -- "$RF_H"/.jash/retired-ratchet-*.key
RF_RETIRED=$1
RF_ADV_AFTER="$(awk '{print $2}' "$RF_RATCHET")"
RF_LINES_BEFORE="$(wc -l < "$RF_CHAIN")"
rf_writer_out="$(run_sigil "$RF_H" "$RF_R" 2 \
    'echo writer-must-not-cross-failed-replacement' 2>&1 || true)"
RF_LINES_AFTER="$(wc -l < "$RF_CHAIN")"
rf_verdict="$(run_sigil "$RF_H" "$RF_R" 0 'sigil verify --json' 2>/dev/null || true)"
if [ "$rf_transition_rc" -ne 0 ] && [ -f "$RF_RETIRED" ] &&
   cmp -s "$RF_RATCHET" "$RF_RETIRED" &&
   [ "$RF_ADV_AFTER" -eq "$((RF_ADV_BEFORE + 1))" ] &&
   [ "$RF_LINES_BEFORE" = "$RF_LINES_AFTER" ] &&
   printf '%s' "$rf_writer_out" | grep -q 'sigil append blocked' &&
   printf '%s' "$rf_verdict" | grep -q '"valid":false' &&
   printf '%s' "$rf_verdict" | grep -q 'ratchet_replacement_incomplete'; then
    pass "replacement falho preserva witness avancado e bloqueia continuidade"
else
    no "replacement falho perdeu witness ou permitiu append" \
       "rc=$rf_transition_rc adv=$RF_ADV_BEFORE/$RF_ADV_AFTER lines=$RF_LINES_BEFORE/$RF_LINES_AFTER retired=$RF_RETIRED verify=$rf_verdict writer=$rf_writer_out"
fi

# ── 15. falhas de suffix preservam a unica copia e reportam estado real ─────
for fault in manifest truncate fsync dir-fsync; do
    SUF_H="$SB/home-suffix-$fault"
    SUF_R="$SB/repo-suffix-$fault"
    init_repo "$SUF_R"
    install_current_module "$SUF_H"
    run_sigil "$SUF_H" "$SUF_R" 2 "echo suffix-$fault-seed" >/dev/null 2>&1
    SUF_CHAIN="$(single_repo_chain "$SUF_H")"
    cp "$SUF_CHAIN" "$SB/suffix-$fault-prefix"
    printf 'suffix-%s-recovery-bytes\n' "$fault" >> "$SUF_CHAIN"
    cp "$SUF_CHAIN" "$SB/suffix-$fault-damaged"
    suffix_apply="$(run_sigil_quarantine_fault "$SUF_H" "$SUF_R" \
        "suffix-$fault" '' 'sigil quarantine apply --json' 2>/dev/null || true)"
    SUF_Q=''
    for p in "$SUF_H"/.jash/quarantine/corrupted_newline_fragment-*chain-*.log; do
        case "$p" in *.manifest.json) continue ;; esac
        [ -f "$p" ] && { SUF_Q=$p; break; }
    done
    if [ "$fault" = manifest ] || [ "$fault" = truncate ]; then
        if printf '%s' "$suffix_apply" | grep -q '"applied":0.*"failed":1' &&
           cmp -s "$SB/suffix-$fault-damaged" "$SUF_CHAIN"; then
            pass "suffix $fault falha sem destruir ativo"
        else
            no "suffix $fault mentiu ou perdeu bytes" "$suffix_apply q=$SUF_Q"
        fi
    else
        if printf '%s' "$suffix_apply" | grep -q '"applied":1.*"failed":1' &&
           printf '%s' "$suffix_apply" | grep -q '"tail_salvaged":true' &&
           cmp -s "$SB/suffix-$fault-prefix" "$SUF_CHAIN" && [ -n "$SUF_Q" ] &&
           grep -q "suffix-$fault-recovery-bytes" "$SUF_Q"; then
            pass "suffix $fault conserva recovery apos mutacao parcial"
        else
            no "suffix $fault apagou unica recovery ou reportou estado falso" \
               "$suffix_apply q=$SUF_Q"
        fi
    fi
done

# Once suffix truncation has changed the active inode, the same process must
# refresh its scanner state even if the final durability barrier reports fail.
RESUME_H="$SB/home-suffix-resume"
RESUME_R="$SB/repo-suffix-resume"
init_repo "$RESUME_R"
install_current_module "$RESUME_H"
run_sigil "$RESUME_H" "$RESUME_R" 2 'echo suffix-resume-seed' >/dev/null 2>&1
RESUME_CHAIN="$(single_repo_chain "$RESUME_H")"
printf 'suffix-resume-fragment' >>"$RESUME_CHAIN"
resume_out="$(cd "$RESUME_R" && HOME="$RESUME_H" JASH_SIGIL=2 JASH_NUDGE=0 \
    JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL_TEST_QUARANTINE_FAIL=suffix-fsync \
    "$JASH" -c 'sigil quarantine apply --json; echo suffix-resumed' 2>&1 || true)"
if printf '%s' "$resume_out" | grep -q 'suffix-resumed' &&
   grep -q 'cmd=.*suffix-resumed' "$RESUME_CHAIN"; then
    pass "suffix parcial refresca estado e permite append no mesmo processo"
else
    no "suffix parcial deixou processo bloqueado apos alterar ativo" "$resume_out"
fi

# Full quarantine must treat directory fsync as part of commit. The move and
# manifest are rolled back byte-for-byte when that durability barrier fails.
FD_H="$SB/home-full-dir-fsync"
FD_R="$SB/repo-full-dir-fsync"
init_repo "$FD_R"
install_current_module "$FD_H"
for i in 1 2 3; do
    run_sigil "$FD_H" "$FD_R" 2 "echo full-dir-fsync-$i" >/dev/null 2>&1
done
FD_CHAIN="$(single_repo_chain "$FD_H")"
set -- "$FD_H"/.jash/ratchet-*.key
FD_RATCHET=$1
awk 'NR == 2 {old = $3; first = substr(old, 1, 1); repl = first == "0" ? "1" : "0";
                 sub(old, repl substr(old, 2))} {print}' "$FD_CHAIN" > "$FD_CHAIN.damaged"
mv "$FD_CHAIN.damaged" "$FD_CHAIN"
cp "$FD_CHAIN" "$SB/full-dir-chain.before"
cp "$FD_RATCHET" "$SB/full-dir-ratchet.before"
full_dir_apply="$(run_sigil_quarantine_fault "$FD_H" "$FD_R" \
    full-dir-fsync '' 'sigil quarantine apply --json' 2>/dev/null || true)"
set -- "$FD_H"/.jash/quarantine/hash_mismatch-*
if printf '%s' "$full_dir_apply" | grep -q '"applied":0.*"failed":1.*"active_reset":false' &&
   cmp -s "$SB/full-dir-chain.before" "$FD_CHAIN" &&
   cmp -s "$SB/full-dir-ratchet.before" "$FD_RATCHET" && [ ! -e "$1" ]; then
    pass "full dir-fsync falho restaura ativo e nao publica commit"
else
    no "full dir-fsync falho deixou estado nao duravel" "$full_dir_apply"
fi

# Moving the damaged pair is not itself an active reset. If creation of the
# replacement pair fails, recovery artifacts stay byte-exact and JSON must keep
# active_reset=false rather than claiming an absent/undurable namespace.
FP_H="$SB/home-fresh-pair-fail"
FP_R="$SB/repo-fresh-pair-fail"
init_repo "$FP_R"
install_current_module "$FP_H"
for i in 1 2 3; do
    run_sigil "$FP_H" "$FP_R" 2 "echo fresh-pair-fail-$i" >/dev/null 2>&1
done
FP_CHAIN="$(single_repo_chain "$FP_H")"
set -- "$FP_H"/.jash/ratchet-*.key
FP_RATCHET=$1
awk 'NR == 2 {old = $3; first = substr(old, 1, 1); repl = first == "0" ? "1" : "0";
                 sub(old, repl substr(old, 2))} {print}' "$FP_CHAIN" >"$FP_CHAIN.damaged"
mv "$FP_CHAIN.damaged" "$FP_CHAIN"
cp "$FP_CHAIN" "$SB/fresh-pair-chain.before"
cp "$FP_RATCHET" "$SB/fresh-pair-ratchet.before"
fresh_pair_apply="$(run_sigil_quarantine_fault "$FP_H" "$FP_R" \
    fresh-chain '' 'sigil quarantine apply --json' 2>/dev/null || true)"
FP_QCHAIN=''; FP_QRATCHET=''
for p in "$FP_H"/.jash/quarantine/hash_mismatch-*; do
    case "$p" in
      *chain-*.log) FP_QCHAIN=$p ;;
      *ratchet-*.key) FP_QRATCHET=$p ;;
    esac
done
if printf '%s' "$fresh_pair_apply" | grep -q \
      '"applied":1.*"failed":1.*"active_reset":false' &&
   [ ! -e "$FP_CHAIN" ] && [ ! -e "$FP_RATCHET" ] &&
   [ -n "$FP_QCHAIN" ] && cmp -s "$SB/fresh-pair-chain.before" "$FP_QCHAIN" &&
   [ -n "$FP_QRATCHET" ] && cmp -s "$SB/fresh-pair-ratchet.before" "$FP_QRATCHET"; then
    pass "active_reset exige novo par duravel"
else
    no "active_reset alegou par ausente ou perdeu recovery" \
       "$fresh_pair_apply chain=$FP_QCHAIN ratchet=$FP_QRATCHET"
fi

# A crash between fresh witness and empty-chain publication leaves a pristine
# advances=0 witness. Startup must complete that staging state, while refusing
# any nonzero orphan witness in chain_resume.
ST_H="$SB/home-fresh-pair-stage"
ST_R="$SB/repo-fresh-pair-stage"
init_repo "$ST_R"
install_current_module "$ST_H"
run_sigil "$ST_H" "$ST_R" 2 'echo seed-staging-key' >/dev/null 2>&1
ST_CHAIN="$(single_repo_chain "$ST_H")"
set -- "$ST_H"/.jash/ratchet-*.key
ST_RATCHET=$1
awk '{print $1, 0}' "$ST_RATCHET" > "$ST_RATCHET.stage"
mv "$ST_RATCHET.stage" "$ST_RATCHET"
rm -f "$ST_CHAIN" "$ST_CHAIN.scan" "$ST_CHAIN.ckptkey"
stage_rc=0
run_sigil "$ST_H" "$ST_R" 2 'echo recovered-fresh-stage' >/dev/null 2>&1 || stage_rc=$?
stage_verdict="$(run_sigil "$ST_H" "$ST_R" 0 'sigil verify --json' 2>/dev/null || true)"
if [ "$stage_rc" -eq 0 ] && grep -q 'recovered-fresh-stage' "$ST_CHAIN" &&
   printf '%s' "$stage_verdict" | grep -q '"valid":true'; then
    pass "fresh-pair staging sem chain e recuperavel e verificavel"
else
    no "fresh-pair staging nao completou atomicamente" \
       "rc=$stage_rc verify=$stage_verdict"
fi

# A non-pristine witness without its chain is not fresh-pair staging. Status
# must expose it, and explicit recovery must preserve the orphan before
# publishing a new pair instead of returning active:null forever.
OR_H="$SB/home-orphan-ratchet"
OR_R="$SB/repo-orphan-ratchet"
init_repo "$OR_R"
install_current_module "$OR_H"
for i in 1 2 3; do
    run_sigil "$OR_H" "$OR_R" 2 "echo orphan-seed-$i" >/dev/null 2>&1
done
OR_CHAIN="$(single_repo_chain "$OR_H")"
set -- "$OR_H"/.jash/ratchet-*.key
OR_RATCHET=$1
cp "$OR_RATCHET" "$SB/orphan-ratchet.before"
rm -f "$OR_CHAIN" "$OR_CHAIN.scan" "$OR_CHAIN.ckptkey"
orphan_status="$(run_sigil "$OR_H" "$OR_R" 0 \
    'sigil quarantine status --json' 2>/dev/null || true)"
orphan_apply="$(run_sigil "$OR_H" "$OR_R" 0 \
    'sigil quarantine apply --json' 2>/dev/null || true)"
OR_QRATCHET=''; OR_MANIFEST=''
for p in "$OR_H"/.jash/quarantine/orphaned_ratchet_without_chain-*; do
    case "$p" in
      *.manifest.json) OR_MANIFEST=$p ;;
      *ratchet-*.key) OR_QRATCHET=$p ;;
    esac
done
orphan_verify="$(run_sigil "$OR_H" "$OR_R" 0 \
    'sigil verify --json' 2>/dev/null || true)"
if printf '%s' "$orphan_status" | grep -q \
      '"reason":"orphaned_ratchet_without_chain"' &&
   printf '%s' "$orphan_apply" | grep -q \
      '"applied":1.*"failed":0.*"active_reset":true' &&
   [ -n "$OR_QRATCHET" ] &&
   cmp -s "$SB/orphan-ratchet.before" "$OR_QRATCHET" &&
   [ -n "$OR_MANIFEST" ] &&
   grep -q '"reason":"orphaned_ratchet_without_chain"' "$OR_MANIFEST" &&
   printf '%s' "$orphan_verify" | grep -q '"valid":true'; then
    pass "ratchet orfao e visivel, preservado e recuperavel"
else
    no "ratchet orfao permaneceu invisivel ou irrecuperavel" \
       "status=$orphan_status apply=$orphan_apply verify=$orphan_verify qratchet=$OR_QRATCHET"
fi

# Full quarantine must move the witness first. A process death after the first
# rename may leave a chain without its witness (an existing recoverable state),
# but must never recreate the recurring inverse orphan.
CO_H="$SB/home-quarantine-crash-order"
CO_R="$SB/repo-quarantine-crash-order"
init_repo "$CO_R"
install_current_module "$CO_H"
for i in 1 2 3; do
    run_sigil "$CO_H" "$CO_R" 2 "echo crash-order-$i" >/dev/null 2>&1
done
CO_CHAIN="$(single_repo_chain "$CO_H")"
set -- "$CO_H"/.jash/ratchet-*.key
CO_RATCHET=$1
awk 'NR == 2 {old = $3; first = substr(old, 1, 1); repl = first == "0" ? "1" : "0";
                 sub(old, repl substr(old, 2))} {print}' "$CO_CHAIN" > "$CO_CHAIN.damaged"
mv "$CO_CHAIN.damaged" "$CO_CHAIN"
crash_order_rc=0
run_sigil_quarantine_fault "$CO_H" "$CO_R" crash-after-ratchet-move '' \
    'sigil quarantine apply --json' >/dev/null 2>&1 || crash_order_rc=$?
crash_order_status="$(run_sigil "$CO_H" "$CO_R" 0 \
    'sigil quarantine status --json' 2>/dev/null || true)"
crash_order_apply="$(run_sigil "$CO_H" "$CO_R" 0 \
    'sigil quarantine apply --json' 2>/dev/null || true)"
crash_order_verify="$(run_sigil "$CO_H" "$CO_R" 0 \
    'sigil verify --json' 2>/dev/null || true)"
if [ "$crash_order_rc" -eq 86 ] && [ -f "$CO_CHAIN" ] &&
   printf '%s' "$crash_order_status" | grep -q \
      '"reason":"missing_scoped_ratchet"' &&
   printf '%s' "$crash_order_apply" | grep -q \
      '"applied":1.*"failed":0.*"active_reset":true' &&
   printf '%s' "$crash_order_verify" | grep -q '"valid":true'; then
    pass "crash de full quarantine nao cria ratchet orfao"
else
    no "ordem de full quarantine ainda permite ratchet orfao" \
       "rc=$crash_order_rc status=$crash_order_status apply=$crash_order_apply verify=$crash_order_verify"
fi

# Full quarantine: force manifest failure and then force both rollback renames
# to fail. The moved chain/ratchet are now the sole recovery artifacts; they
# must remain published and the JSON must say the active ledger was reset.
RB_H="$SB/home-full-rollback"
RB_R="$SB/repo-full-rollback"
init_repo "$RB_R"
install_current_module "$RB_H"
for i in 1 2 3; do
    run_sigil "$RB_H" "$RB_R" 2 "echo full-rollback-$i" >/dev/null 2>&1
done
RB_CHAIN="$(single_repo_chain "$RB_H")"
set -- "$RB_H"/.jash/ratchet-*.key
RB_RATCHET=$1
awk 'NR == 2 {old = $3; first = substr(old, 1, 1); repl = first == "0" ? "1" : "0";
                 sub(old, repl substr(old, 2))} {print}' "$RB_CHAIN" > "$RB_CHAIN.damaged"
mv "$RB_CHAIN.damaged" "$RB_CHAIN"
cp "$RB_CHAIN" "$SB/full-rollback-chain.before"
cp "$RB_RATCHET" "$SB/full-rollback-ratchet.before"
rollback_apply="$(run_sigil_quarantine_fault "$RB_H" "$RB_R" \
    full-rollback write 'sigil quarantine apply --json' 2>/dev/null || true)"
RB_QCHAIN=''; RB_QRATCHET=''
for p in "$RB_H"/.jash/quarantine/hash_mismatch-*; do
    case "$p" in
      *chain-*.log) RB_QCHAIN=$p ;;
      *ratchet-*.key) RB_QRATCHET=$p ;;
    esac
done
rollback_status="$(run_sigil "$RB_H" "$RB_R" 0 \
    'sigil quarantine status --json' 2>/dev/null || true)"
if printf '%s' "$rollback_apply" | grep -q '"applied":1.*"failed":1.*"active_reset":true' &&
   [ -f "$RB_CHAIN" ] && [ -f "$RB_RATCHET" ] &&
   ! cmp -s "$SB/full-rollback-chain.before" "$RB_CHAIN" &&
   ! cmp -s "$SB/full-rollback-ratchet.before" "$RB_RATCHET" &&
   [ -n "$RB_QCHAIN" ] && cmp -s "$SB/full-rollback-chain.before" "$RB_QCHAIN" &&
   [ -n "$RB_QRATCHET" ] && cmp -s "$SB/full-rollback-ratchet.before" "$RB_QRATCHET" &&
   printf '%s' "$rollback_status" | grep -q '"recovery_artifacts"' &&
   printf '%s' "$rollback_status" | grep -q "$(basename "$RB_QCHAIN")" &&
   printf '%s' "$rollback_status" | grep -q "$(basename "$RB_QRATCHET")"; then
    pass "rollback full falho preserva recovery e reporta estado movido"
else
    no "rollback full falho apagou recovery ou reportou estado falso" \
       "$rollback_apply chain=$RB_QCHAIN ratchet=$RB_QRATCHET"
fi

RS_H="$SB/home-full-reset-same-process"
RS_R="$SB/repo-full-reset-same-process"
init_repo "$RS_R"
install_current_module "$RS_H"
for i in 1 2 3; do
    run_sigil "$RS_H" "$RS_R" 2 "echo full-reset-seed-$i" >/dev/null 2>&1
done
RS_CHAIN="$(single_repo_chain "$RS_H")"
awk 'NR == 2 {old = $3; first = substr(old, 1, 1); repl = first == "0" ? "1" : "0";
                 sub(old, repl substr(old, 2))} {print}' "$RS_CHAIN" >"$RS_CHAIN.damaged"
mv "$RS_CHAIN.damaged" "$RS_CHAIN"
reset_same_out="$(cd "$RS_R" && HOME="$RS_H" JASH_SIGIL=2 JASH_NUDGE=0 \
    JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL_TEST_QUARANTINE_FAIL=full-rollback \
    JASH_SIGIL_TEST_MANIFEST_FAIL=write "$JASH" -c \
    'sigil quarantine apply --json; echo full-reset-continues' 2>&1 || true)"
RS_NEW_CHAIN="$(single_repo_chain "$RS_H" 2>/dev/null || true)"
if printf '%s' "$reset_same_out" | grep -q 'full-reset-continues' &&
   [ -n "$RS_NEW_CHAIN" ] && grep -q 'full-reset-continues' "$RS_NEW_CHAIN"; then
    pass "full quarantine parcial refresca estado no mesmo processo"
else
    no "full quarantine parcial manteve estado bloqueado obsoleto" "$reset_same_out"
fi

partial_rollback_ok=1
for rollback_mode in rollback-chain rollback-ratchet; do
    PH="$SB/home-$rollback_mode"
    PR="$SB/repo-$rollback_mode"
    init_repo "$PR"
    install_current_module "$PH"
    for i in 1 2 3; do
        run_sigil "$PH" "$PR" 2 "echo $rollback_mode-$i" >/dev/null 2>&1
    done
    PC="$(single_repo_chain "$PH")"
    set -- "$PH"/.jash/ratchet-*.key
    PK=$1
    awk 'NR == 2 {old = $3; first = substr(old, 1, 1); repl = first == "0" ? "1" : "0";
                     sub(old, repl substr(old, 2))} {print}' "$PC" >"$PC.damaged"
    mv "$PC.damaged" "$PC"
    cp "$PC" "$SB/$rollback_mode-chain.before"
    cp "$PK" "$SB/$rollback_mode-ratchet.before"
    partial_apply="$(run_sigil_quarantine_fault "$PH" "$PR" \
        "$rollback_mode" write 'sigil quarantine apply --json' 2>/dev/null || true)"
    PQC=''; PQR=''
    for p in "$PH"/.jash/quarantine/hash_mismatch-*; do
        case "$p" in
          *chain-*.log) PQC=$p ;;
          *ratchet-*.key) PQR=$p ;;
        esac
    done
    partial_verify="$(run_sigil "$PH" "$PR" 0 'sigil verify --json' 2>/dev/null || true)"
    if ! printf '%s' "$partial_apply" | grep -q \
          '"applied":1.*"failed":1.*"active_reset":true' ||
       [ ! -e "$PC" ] || [ ! -e "$PK" ] || [ -z "$PQC" ] || [ -z "$PQR" ]; then
        partial_rollback_ok=0
    elif ! cmp -s "$SB/$rollback_mode-chain.before" "$PQC" ||
         ! cmp -s "$SB/$rollback_mode-ratchet.before" "$PQR" ||
         cmp -s "$SB/$rollback_mode-chain.before" "$PC" ||
         cmp -s "$SB/$rollback_mode-ratchet.before" "$PK" ||
         ! printf '%s' "$partial_verify" | grep -q '"valid":true'; then
        partial_rollback_ok=0
    fi
done
if [ "$partial_rollback_ok" -eq 1 ]; then
    pass "rollback parcial preserva bytes pareados e publica novo par verificavel"
else
    no "rollback parcial separou bytes ou publicou par nao verificavel" \
       "$partial_apply verify=$partial_verify qchain=$PQC qratchet=$PQR"
fi


# If ratchet restoration and chain requarantine both fail, rollback completes
# the active pair instead of leaving one active and one quarantined.
CR_H="$SB/home-rollback-ratchet-requarantine"
CR_R="$SB/repo-rollback-ratchet-requarantine"
init_repo "$CR_R"
install_current_module "$CR_H"
for i in 1 2 3; do
    run_sigil "$CR_H" "$CR_R" 2 "echo combined-rollback-$i" >/dev/null 2>&1
done
CR_CHAIN="$(single_repo_chain "$CR_H")"
set -- "$CR_H"/.jash/ratchet-*.key
CR_RATCHET=$1
awk 'NR == 2 {old = $3; first = substr(old, 1, 1); repl = first == "0" ? "1" : "0";
                 sub(old, repl substr(old, 2))} {print}' "$CR_CHAIN" >"$CR_CHAIN.damaged"
mv "$CR_CHAIN.damaged" "$CR_CHAIN"
combined_apply="$(run_sigil_quarantine_fault "$CR_H" "$CR_R" \
    rollback-ratchet-requarantine write \
    'sigil quarantine apply --json' 2>/dev/null || true)"
if printf '%s' "$combined_apply" | grep -q '"applied":0.*"failed":1.*"active_reset":false' &&
   [ -f "$CR_CHAIN" ] && [ -f "$CR_RATCHET" ]; then
    pass "rollback combinado restaura chain e ratchet no mesmo namespace"
else
    no "rollback combinado separou chain e ratchet" "$combined_apply"
fi

# ── 16. proof/DSSE snapshot bloqueado e publicacao atomica/duravel ──────────
PR_H="$SB/home-proof-transaction"
PR_R="$SB/repo-proof-transaction"
init_repo "$PR_R"
install_current_module "$PR_H"
for i in 1 2 3; do
    run_sigil "$PR_H" "$PR_R" 2 "echo proof-seed-$i" >/dev/null 2>&1
done
PR_CHAIN="$(single_repo_chain "$PR_H")"
for kind in emit dsse; do
    PR_OUT="$SB/proof-lock-$kind"
    exec 9>>"$PR_CHAIN.txnlock"
    flock -x 9
    run_sigil "$PR_H" "$PR_R" 0 "sigil proof $kind $PR_OUT" \
        >"$SB/proof-lock-$kind.out" 2>&1 &
    PR_PID=$!
    sleep 0.2
    if kill -0 "$PR_PID" 2>/dev/null && [ ! -e "$PR_OUT" ]; then
        pass "proof $kind espera txnlock para snapshot count+tip"
    else
        no "proof $kind publicou fora do snapshot serializado" \
           "pid=$PR_PID output=$PR_OUT"
    fi
    flock -u 9
    exec 9>&-
    wait "$PR_PID" || true
    proof_snapshot_verdict="$(run_sigil "$PR_H" "$PR_R" 0 \
        "sigil proof verify $PR_OUT" 2>&1 || true)"
    if [ "$kind" = emit ]; then
        case "$proof_snapshot_verdict" in
          *'chain_tip matches'*) pass "proof $kind publica count+tip do mesmo snapshot" ;;
          *) no "proof $kind mistura count e tip" "$proof_snapshot_verdict" ;;
        esac
    else
        dsse_payload="$(awk -F'"' '/"payload":/{print $4; exit}' "$PR_OUT" | base64 -d 2>/dev/null)"
        dsse_entries="$(printf '%s\n' "$dsse_payload" | awk '/^chain_entries:/{print $2}')"
        dsse_tip="$(printf '%s\n' "$dsse_payload" | awk '/^chain_tip:/{print $2}')"
        live_entries="$(grep ' cmd=' "$PR_CHAIN" | grep -vc ' cmd=SESSION_END')"
        live_tip="$(awk '$3 !~ /^0+$/{tip=$3} END{print tip}' "$PR_CHAIN")"
        case "$proof_snapshot_verdict" in
          *CONHECIDA*) dsse_sig_ok=1 ;;
          *) dsse_sig_ok=0 ;;
        esac
        if [ "$dsse_sig_ok" -eq 1 ] && [ "$dsse_entries" = "$live_entries" ] &&
           [ "$dsse_tip" = "$live_tip" ]; then
            pass "proof $kind publica count+tip do mesmo snapshot"
        else
            no "proof $kind mistura count e tip" \
               "entries=$dsse_entries/$live_entries tip=$dsse_tip/$live_tip verify=$proof_snapshot_verdict"
        fi
    fi
done

for kind in emit dsse; do
    proof_faults_ok=1
    for fault in write close fsync rename dir-fsync; do
        PF_OUT="$SB/proof-fault-$kind-$fault"
        printf 'old-complete-artifact\n' > "$PF_OUT"
        if run_sigil_proof_fault "$PR_H" "$PR_R" "$fault" \
            "sigil proof $kind $PF_OUT" >/dev/null 2>&1; then
            proof_faults_ok=0
            continue
        fi
        if [ "$fault" = dir-fsync ]; then
            case "$kind" in
              emit) grep -q '^jashproof/1$' "$PF_OUT" || proof_faults_ok=0 ;;
              dsse) grep -q '"payloadType"' "$PF_OUT" || proof_faults_ok=0 ;;
            esac
        elif ! grep -q '^old-complete-artifact$' "$PF_OUT"; then
            proof_faults_ok=0
        fi
        set -- "$PF_OUT".tmp.*
        [ ! -e "$1" ] || proof_faults_ok=0
    done
    if [ "$proof_faults_ok" -eq 1 ]; then
        pass "proof $kind checa write/close/fsync/rename/dir-fsync sem parcial"
    else
        no "proof $kind aceitou fault ou publicou parcial" "$SB/proof-fault-$kind-*"
    fi
done

relative_proofs_ok=1
for kind in emit dsse; do
    PR_REL="proof-relative-$kind"
    rm -f "$PR_R/$PR_REL"
    if ! run_sigil "$PR_H" "$PR_R" 0 "sigil proof $kind $PR_REL" \
         >/dev/null 2>&1 || [ ! -s "$PR_R/$PR_REL" ]; then
        relative_proofs_ok=0
    fi
done
if [ "$relative_proofs_ok" -eq 1 ]; then
    pass "proof emit/dsse publica nomes relativos com fsync do cwd"
else
    no "proof relativo publicou artefato mas reportou falha" "$PR_R/proof-relative-*"
fi

cat >"$SB/core-truncate.c" <<'C'
#define JASH_AGENT_ONLY 1
#include "module/sigil.c"
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    if (argv[2][0] && setenv("JASH_SIGIL_TEST_TRUNCATE_FAIL", argv[2], 1) != 0)
        return 3;
    int rc = truncate_chain(argv[1], 2);
    int emergency = strcmp(argv[2], "rollback") == 0 ||
                    strcmp(argv[2], "cleanup") == 0;
    return argv[2][0] ? (rc == (emergency ? 2 : -1) ? 0 : 1)
                      : (rc == 1 ? 0 : 1);
}
C
if cc -std=gnu11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
      -ffunction-sections -fdata-sections -Wl,--gc-sections \
      -I"$ROOT/c" -I"$ROOT/libjef" "$SB/core-truncate.c" \
      -o "$SB/core-truncate" >/dev/null 2>&1; then
    truncate_ok=1
    # The real fallback only trims beyond CHAIN_MAX_LINES + 1000. Keep the
    # fault cases above that hysteresis boundary so every injected path runs.
    awk 'BEGIN { for (i = 0; i < 11000; i++) print "prefix-" i;
                 print "three"; print "four" }' >"$SB/core-chain.before"
    cp "$SB/core-chain.before" "$SB/core-chain"
    "$SB/core-truncate" "$SB/core-chain" '' || truncate_ok=0
    printf 'three\nfour\n' >"$SB/core-chain.expected"
    cmp -s "$SB/core-chain" "$SB/core-chain.expected" || truncate_ok=0
    set -- "$SB"/core-chain.archive.*
    [ "$#" -eq 1 ] && [ -f "$1" ] &&
        cmp -s "$1" "$SB/core-chain.before" || truncate_ok=0
    for fault in write fsync rename dir-fsync rollback cleanup; do
        cp "$SB/core-chain.before" "$SB/core-chain"
        rm -f "$SB"/core-chain.rollback.*
        "$SB/core-truncate" "$SB/core-chain" "$fault" || truncate_ok=0
        if [ "$fault" = rollback ] || [ "$fault" = cleanup ]; then
            set -- "$SB"/core-chain.rollback.*
            [ -f "$1" ] && cmp -s "$1" "$SB/core-chain.before" || truncate_ok=0
        else
            cmp -s "$SB/core-chain" "$SB/core-chain.before" || truncate_ok=0
        fi
    done
    if [ "$truncate_ok" -eq 1 ]; then
        pass "fallback truncate checa write/fsync/rename e preserva ativo"
    else
        no "fallback truncate publicou parcial ou mentiu sucesso" "$SB/core-chain"
    fi
else
    no "harness fallback truncate compila" "$SB/core-truncate.c"
fi

# ── 17. parsers e snapshots de leitura falham fechados e ficam frescos ──────
MAL_H="$SB/home-malformed-ratchet"
MAL_R="$SB/repo-malformed-ratchet"
init_repo "$MAL_R"
install_current_module "$MAL_H"
for i in 1 2 3; do
    run_sigil "$MAL_H" "$MAL_R" 2 "echo malformed-ratchet-seed-$i" >/dev/null 2>&1
done
MAL_CHAIN="$(single_repo_chain "$MAL_H")"
set -- "$MAL_H"/.jash/ratchet-*.key
MAL_RATCHET=$1
cp "$MAL_RATCHET" "$SB/malformed-ratchet.good"
MAL_KEY="$(awk '{print $1}' "$MAL_RATCHET")"
malformed_ratchet_ok=1
for bad_count in not-a-count 7junk 18446744073709551616; do
    printf '%s %s\n' "$MAL_KEY" "$bad_count" >"$MAL_RATCHET"
    mal_status="$(run_sigil "$MAL_H" "$MAL_R" 0 \
        'sigil quarantine status --json' 2>/dev/null || true)"
    mal_verify="$(run_sigil "$MAL_H" "$MAL_R" 0 \
        'sigil verify --json' 2>/dev/null || true)"
    if ! printf '%s' "$mal_status" | grep -q 'malformed_ratchet' ||
       ! printf '%s' "$mal_verify" | grep -q '"valid":false' ||
       printf '%s' "$mal_verify" | grep -q '"valid":true' ||
       printf '%s' "$mal_verify" | grep -q '"ratchet_position":"unavailable"'; then
        malformed_ratchet_ok=0
    fi
done
cp "$SB/malformed-ratchet.good" "$MAL_RATCHET"
if [ "$malformed_ratchet_ok" -eq 1 ]; then
    pass "ratchet rejeita count malformado e verifier falha fechado"
else
    no "ratchet malformado virou valid/unavailable" \
       "status=$mal_status verify=$mal_verify"
fi

cp "$MAL_CHAIN" "$SB/physical-line.good"
printf 'unterminated-physical-fragment' >>"$MAL_CHAIN"
physical_status="$(run_sigil "$MAL_H" "$MAL_R" 0 \
    'sigil quarantine status --json' 2>/dev/null || true)"
physical_verify="$(run_sigil "$MAL_H" "$MAL_R" 0 \
    'sigil verify --json' 2>/dev/null || true)"
if printf '%s' "$physical_status" | grep -q 'corrupted_newline_fragment' &&
   printf '%s' "$physical_verify" | grep -q '"valid":false'; then
    pass "status/verify rejeitam linha fisica sem terminador"
else
    no "linha fisica malformada foi ignorada" \
       "status=$physical_status verify=$physical_verify"
fi
cp "$SB/physical-line.good" "$MAL_CHAIN"

# A module instance first observes a healthy chain, then the file changes while
# that same process sleeps. The second status must rescan the locked active
# inode rather than replaying chain_resume() globals.
FRESH_READY="$SB/fresh-status.ready"
printf 'sigil quarantine status --json\ntouch %s\nsleep 2\nsigil quarantine status --json\n' \
       "$FRESH_READY" |
    (cd "$MAL_R" && HOME="$MAL_H" JASH_SIGIL=0 JASH_NUDGE=0 JASH_BRIEF=0 \
        JASH_LOOPGUARD=off "$JASH") >"$SB/fresh-status.out" 2>&1 &
FRESH_PID=$!
for _ in $(seq 1 200); do
    [ -e "$FRESH_READY" ] && break
    sleep 0.01
done
[ -e "$FRESH_READY" ] && printf 'changed-after-first-status' >>"$MAL_CHAIN"
fresh_rc=0
wait "$FRESH_PID" || fresh_rc=$?
if [ "$fresh_rc" -eq 0 ] &&
   grep -q '"active":null' "$SB/fresh-status.out" &&
   grep -q 'corrupted_newline_fragment' "$SB/fresh-status.out"; then
    pass "status no mesmo processo observa arquivo alterado"
else
    no "status reutilizou snapshot obsoleto" \
       "rc=$fresh_rc output=$(tr '\n' ' ' <"$SB/fresh-status.out")"
fi
cp "$SB/physical-line.good" "$MAL_CHAIN"

# ── 18. HOME relativo vira identidade absoluta; OCI nao relê HOME mutavel ───
REL_BASE="$SB/relative-home"
REL_HOME="$REL_BASE/home"
REL_REPO="$REL_BASE/repo"
init_repo "$REL_REPO"
mkdir -p "$REL_REPO/sub"
install_current_module "$REL_HOME"
printf 'echo relative-before\ncd sub\necho relative-after\nsigil off\nsigil proof emit\nsigil proof dsse\nsigil export\n' |
    (cd "$REL_REPO" && HOME='../home' JASH_SIGIL=2 JASH_NUDGE=0 JASH_BRIEF=0 \
        JASH_LOOPGUARD=off "$JASH") >"$SB/relative-home.out" 2>&1
REL_CHAIN="$(single_repo_chain "$REL_HOME" 2>/dev/null || true)"
if [ -n "$REL_CHAIN" ] && grep -q 'relative-before' "$REL_CHAIN" &&
   grep -q 'relative-after' "$REL_CHAIN" &&
   [ -s "$REL_HOME/.jash/session.jashproof" ] &&
   [ -s "$REL_HOME/.jash/session.jashproof.dsse" ]; then
    pass "HOME relativo e canonicalizado numa identidade absoluta estavel"
else
    no "HOME relativo mudou de identidade apos cd" \
       "chain=$REL_CHAIN proof=$REL_HOME/.jash/session.jashproof"
fi
if [ -f "$REL_HOME/.jash/oci-export/index.json" ] &&
   [ ! -e "$REL_REPO/home/.jash" ]; then
    pass "OCI default usa HOME pinned e nunca HOME relativo mutavel"
else
    no "OCI default releu HOME relativo depois do init" \
       "pinned=$REL_HOME/.jash/oci-export escaped=$REL_REPO/home/.jash"
fi

LONG_H="$SB/long-home"
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do
    LONG_H="$LONG_H/segment-$i-abcdefghijklmnopqrstuvwxyz0123456789"
done
LONG_R="$SB/repo-long-home"
init_repo "$LONG_R"
mkdir -p "$LONG_H"
install_current_module "$LONG_H"
LONG_RUN="$(run_sigil "$LONG_H" "$LONG_R" 2 'echo long-home-ledger' 2>&1 || true)"
LONG_CHAIN="$(single_repo_chain "$LONG_H" 2>/dev/null || true)"
set -- "$LONG_H"/.jash/ratchet-*.key
LONG_RATCHET=$1
LONG_VERIFY="$(run_sigil "$LONG_H" "$LONG_R" 0 'sigil verify --json' 2>/dev/null || true)"
if [ -n "$LONG_CHAIN" ] && grep -q 'long-home-ledger' "$LONG_CHAIN" &&
   [ -s "$LONG_RATCHET" ] && printf '%s' "$LONG_VERIFY" | grep -q '"valid":true'; then
    pass "HOME valido acima de 640 bytes preserva chain e ratchet"
else
    LONG_FILES="$(for f in "$LONG_H"/.jash/*; do
        [ -e "$f" ] && printf '%s(%s) ' "${f##*/}" "$(stat -c %s "$f")"
    done)"
    no "HOME longo desativou ou redirecionou ledger" \
       "home_len=${#LONG_H} chain=$LONG_CHAIN files=$LONG_FILES verify=$LONG_VERIFY output=$LONG_RUN home=$LONG_H"
fi

# Verify-before-first-append must not poison the session. Attestation lands
# at top-level eval end, so `echo hi; sigil chain verify --json` in one -c
# verifies an empty ledger (no_chain_file, rc 1 — correct) and THEN records.
# The old code turned that honest failure into chain_verification_failed and
# the just-run commands were never recorded at all.
FRESH_H="$SB/home-fresh-verify"
FRESH_R="$SB/repo-fresh-verify"
init_repo "$FRESH_R"
install_current_module "$FRESH_H"
FRESH_OUT="$(run_sigil "$FRESH_H" "$FRESH_R" 2 'echo fresh-hello; sigil chain verify --json' 2>&1 || true)"
FRESH_CHAIN="$(single_repo_chain "$FRESH_H" 2>/dev/null || true)"
FRESH_VERIFY="$(run_sigil "$FRESH_H" "$FRESH_R" 0 'sigil verify --json' 2>/dev/null || true)"
if printf '%s' "$FRESH_OUT" | grep -q 'no_chain_file' &&
   ! printf '%s' "$FRESH_OUT" | grep -q 'append blocked' &&
   [ -n "$FRESH_CHAIN" ] && [ -s "$FRESH_CHAIN" ] &&
   printf '%s' "$FRESH_VERIFY" | grep -q '"valid":true'; then
    pass "verify em HOME fresco nao envenena appends da sessao"
else
    no "verify fresco envenenou a sessao" \
        "out=$FRESH_OUT chain=$FRESH_CHAIN verify=$FRESH_VERIFY"
fi

echo
echo "=== JINV-112: $ok ok / $fail fail ==="
[ "$fail" -eq 0 ]
