#!/usr/bin/env bash
# JINV-106: a sealed boundary narrows what the ratchet must witness, so it is a
# load-bearing security claim. This is a MUTATION test: it passes only while the
# nonce gate exists. Delete the strcmp against the key-side nonce and case B
# turns green->red, because a boundary would then be a thing anyone can TYPE.
#
#   A. legitimate rekey  -> valid, witness_scope=current_segment
#   B. boundary whose nonce does NOT match the one stored beside the key
#      -> the verifier must REFUSE to narrow, and fail loudly
#   C. the narrowed verdict must SAY it was narrowed (an auditor reading
#      "valid":true has to see which entries the ratchet actually covered)

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
MOD="$PWD/modules/sigil/sigil.modjash"
[ -x "$JASH" ] || { echo "SKIP: $JASH missing"; exit 0; }
[ -f "$MOD" ] || { echo "SKIP: $MOD missing"; exit 0; }

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT
pass=0 fail=0
ok() { pass=$((pass + 1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

setup_case() {
    H="$T/$1/home"; R="$T/$1/repo"
    mkdir -p "$R"
    git init -q "$R" || return 1
    bash "$PWD/tests/setup_sigil_modules.sh" "$H"
}

run_jash() {
    (cd "$2" && HOME="$1" JASH_SIGIL=2 JASH_NUDGE=0 JASH_BRIEF=0 \
        JASH_LOOPGUARD=off JASH_MEMO=0 "$JASH" -c "$3" 2>&1)
}

# Build: 3 entries, seal a boundary, 2 more entries. The current ratchet then
# legitimately witnesses 2 -- not 5.
build_segmented() {
    setup_case "$1"
    run_jash "$H" "$R" 'module load sigil; true; true; true' >/dev/null
    run_jash "$H" "$R" 'module load sigil; sigil ratchet rekey' >/dev/null
    run_jash "$H" "$R" 'module load sigil; true; true' >/dev/null
}

# ---- A: a legitimate rekey must not look like an attack -------------------
build_segmented a
OUT=$(run_jash "$H" "$R" 'module load sigil; sigil chain verify --json')
case "$OUT" in
    *'"valid":true'*) ok "A rekey legitimo continua valido" ;;
    *) bad "A rekey legitimo continua valido" "$OUT" ;;
esac
case "$OUT" in
    *'"witness_scope":"current_segment"'*)
        ok "A testemunha se declara restrita ao segmento" ;;
    *) bad "A testemunha se declara restrita ao segmento" "$OUT" ;;
esac

# ---- B: THE MUTATION -- an unrecognised nonce must not narrow anything -----
build_segmented b
KEYF=
for candidate in "$H"/.jash/ratchet-*.key; do
    [ -f "$candidate" ] && { KEYF=$candidate; break; }
done
if [ -z "$KEYF" ] || [ ! -f "$KEYF" ]; then
    bad "B arquivo de ratchet localizado" "nada em $H/.jash"
else
    # The attacker owns the log but not the key: rewrite the key-side nonce so
    # it no longer matches the sealed boundary. Nothing else is touched.
    awk '{ if (NF >= 3) $3 = "ffffffffffffffffffffffffffffffff"; print }' \
        "$KEYF" > "$KEYF.tmp" && mv "$KEYF.tmp" "$KEYF"
    OUT=$(run_jash "$H" "$R" 'module load sigil; sigil chain verify --json')
    case "$OUT" in
        *'"valid":false'*)
            ok "B fronteira nao reconhecida NAO relaxa a verificacao" ;;
        *) bad "B fronteira nao reconhecida NAO relaxa a verificacao" \
               "uma fronteira digitavel valeria como selada: $OUT" ;;
    esac
    case "$OUT" in
        *'"witness_scope":"current_segment"'*)
            bad "B nao pode afirmar escopo de segmento" "$OUT" ;;
        *) ok "B nao pode afirmar escopo de segmento" ;;
    esac
fi

# ---- C: the narrowed verdict must be self-describing ----------------------
build_segmented c
OUT=$(run_jash "$H" "$R" 'module load sigil; sigil chain verify --json')
case "$OUT" in
    *'segmento apos a ultima fronteira selada'*)
        ok "C o veredito explica o que o ratchet NAO cobre" ;;
    *) bad "C o veredito explica o que o ratchet NAO cobre" "$OUT" ;;
esac

# ---- D: rotating a local key cannot erase evidence of truncation ------------
setup_case d
for i in 1 2 3; do
    run_jash "$H" "$R" "module load sigil; echo local-rotate-$i" >/dev/null
done
CHAIN=$(printf '%s\n' "$H"/.jash/chain-*.log)
lines=$(wc -l < "$CHAIN")
sed -n "1,$((lines - 1))p" "$CHAIN" > "$CHAIN.truncated"
mv "$CHAIN.truncated" "$CHAIN"
if OUT=$(run_jash "$H" "$R" 'module load sigil; sigil ratchet rekey'); then
    rc=0
else
    rc=$?
fi
if [ "$rc" -ne 0 ] && printf '%s' "$OUT" | grep -q 'refusing rotation'; then
    ok "D rotacao local recusa ledger truncado"
else
    bad "D rotacao local recusa ledger truncado" "rc=$rc $OUT"
fi
OUT=$(run_jash "$H" "$R" 'module load sigil; sigil verify --json' || true)
case "$OUT" in
    *'"valid":false'*'log_truncated'*)
        ok "D truncamento continua visivel apos rotacao recusada" ;;
    *) bad "D rotacao nao pode apagar testemunha de truncamento" "$OUT" ;;
esac

printf '\n=== JINV-106: %d ok / %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
