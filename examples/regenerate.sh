#!/bin/bash
set -e
ROOT=/Users/admin/refatorados/Jash
J="$ROOT/build/jash"
EX="$ROOT/examples/session-01"
TAMP="$ROOT/examples/tampered"
mkdir -p "$EX" "$TAMP"

# Isolated HOME so the captured session is clean + reproducible-ish.
TH=$(mktemp -d)
run(){ HOME="$TH" JASH_SIGIL=2 "$J" -c "$1" >/dev/null 2>&1; }

# A realistic little agent session.
run 'echo "building project"'
run 'mkdir -p /tmp/_jash_ex && touch /tmp/_jash_ex/app.o'
run 'echo "tests: 12 passed"'
run 'rm -f /tmp/_jash_ex/app.o'
# Emit both proof forms.
HOME="$TH" JASH_SIGIL=2 "$J" -c 'sigil proof emit' >/dev/null 2>&1
HOME="$TH" JASH_SIGIL=2 "$J" -c 'sigil proof dsse' >/dev/null 2>&1

cp "$TH/.jash/chain.log"             "$EX/chain.log"
cp "$TH/.jash/session.jashproof"     "$EX/session.jashproof"
cp "$TH/.jash/session.jashproof.dsse" "$EX/session.jashproof.dsse"
# Public key only (the secret seed stays in $TH and is thrown away).
grep '^pubkey: ' "$EX/session.jashproof" | sed 's/^pubkey: //' > "$EX/pubkey.hex"

# Tampered copy: flip a command in the chain (1 byte / word).
sed 's/tests: 12 passed/tests: 99 passed/' "$EX/chain.log" > "$TAMP/chain.log"
cp "$EX/session.jashproof" "$TAMP/session.jashproof"
# also a tampered proof: change the user line (breaks the ed25519 body)
sed 's/^user: /user: mallory-/' "$EX/session.jashproof" > "$TAMP/session.jashproof.bad" || true

rm -rf "$TH"
echo "=== generated artifacts ==="
ls -la "$EX" "$TAMP"
echo "=== chain.log (real) ==="
cat "$EX/chain.log"
echo "=== proof head ==="
grep -E 'jashproof|chain_tip|pubkey|sig_algo' "$EX/session.jashproof"
