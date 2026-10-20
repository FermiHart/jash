#!/usr/bin/env bash
# tests/test_rm_safety.sh — ADR-0044 Arc A: rm safety + parity.
#
# The audit rated rm's -i/-I a BROKEN NO-OP (deleted without asking) and found a
# real DATA-LOSS bug: rm_recursive used stat(), so `rm -r symlink-to-dir` followed
# the link and deleted the TARGET's contents — files outside the named tree.
# This gate locks in: lstat (never follow a symlink during recursion), a working
# -i/-I prompt (EOF = no delete), -d (empty dir only), and `--` for dash-named
# files. The dangerous rm_recursive path only runs when trash is OFF, so we force
# the plain delete path with an isolated HOME + JASH_TRASH=off.
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$PROJECT_ROOT/build/jash-release}"
[[ -x "$JASH" ]] || JASH="$PROJECT_ROOT/build/jash"
[[ -x "$JASH" ]] || JASH="$PROJECT_ROOT/build-speed/jash-release"
if [[ ! -x "$JASH" ]]; then echo "SKIP: no jash binary"; exit 0; fi
JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"

H="$(mktemp -d /tmp/jash_rmsafe_home.XXXXXX)"   # isolated: no persisted trash toggle
trap 'rm -rf "$H"' EXIT

pass=0; fail=0
ok(){ pass=$((pass+1)); echo "  ✓ $1"; }
no(){ fail=$((fail+1)); echo "  ✗ $1"; }
# Plain (non-trash) delete path so rm_recursive is actually exercised.
RM(){ env -i HOME="$H" PATH=/usr/bin:/bin JASH_TRASH=off JASH_SIGIL=0 JASH_ADOPT=0 \
      JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 "$JASH" -c "$1"; }

# ── 1. DATA LOSS: rm -rf symlink→dir must remove the LINK, not the target ────
W="$(mktemp -d)"; mkdir "$W/precious"; echo important > "$W/precious/keep.txt"
mkdir "$W/work"; ln -s "$W/precious" "$W/work/link"
RM "rm -rf '$W/work'" >/dev/null 2>&1
if [[ -f "$W/precious/keep.txt" && ! -e "$W/work" ]]; then
    ok "rm -rf symlink→dir kept target contents (lstat, unlink the link)"
else no "DATA LOSS — rm -rf followed the symlink into the target"; fi
rm -rf "$W"

# ── 2. rm symlink→dir WITHOUT -r removes the link (GNU: no -r needed) ─────────
W="$(mktemp -d)"; mkdir "$W/d"; echo x > "$W/d/f"; ln -s "$W/d" "$W/lnk"
RM "rm '$W/lnk'" >/dev/null 2>&1
if [[ ! -e "$W/lnk" && -f "$W/d/f" ]]; then ok "rm link (no -r) removed link, kept target"
else no "rm link (no -r) wrong"; fi
rm -rf "$W"

# ── 3. rm -i with no answer (EOF/non-tty) must NOT delete ────────────────────
W="$(mktemp -d)"; echo hi > "$W/f"
RM "rm -i '$W/f'" </dev/null >/dev/null 2>&1
[[ -f "$W/f" ]] && ok "rm -i EOF kept the file (no silent delete)" || no "rm -i deleted on EOF"
rm -rf "$W"

# ── 4. rm -i with 'y' deletes ────────────────────────────────────────────────
W="$(mktemp -d)"; echo hi > "$W/f"
printf 'y\n' | RM "rm -i '$W/f'" >/dev/null 2>&1
[[ ! -f "$W/f" ]] && ok "rm -i 'y' deleted" || no "rm -i 'y' failed to delete"
rm -rf "$W"

# ── 5. -f overrides -i: `rm -if` deletes without prompting ───────────────────
W="$(mktemp -d)"; echo hi > "$W/f"
RM "rm -if '$W/f'" </dev/null >/dev/null 2>&1
[[ ! -f "$W/f" ]] && ok "rm -if (force last) deleted without prompt" || no "rm -if did not delete"
rm -rf "$W"

# ── 6. rm -d removes empty dir, refuses non-empty ────────────────────────────
W="$(mktemp -d)"; mkdir "$W/empty" "$W/full"; echo x > "$W/full/f"
RM "rm -d '$W/empty'" >/dev/null 2>&1
RM "rm -d '$W/full'"  >/dev/null 2>&1
if [[ ! -d "$W/empty" && -d "$W/full" ]]; then ok "rm -d removed empty, refused non-empty"
else no "rm -d wrong"; fi
rm -rf "$W"

# ── 7. rm -- dash-named file ─────────────────────────────────────────────────
W="$(mktemp -d)"; echo x > "$W/-weird"
RM "cd '$W' && rm -- -weird" >/dev/null 2>&1
[[ ! -f "$W/-weird" ]] && ok "rm -- removed the dash-named file" || no "rm -- failed"
rm -rf "$W"

# ── 8. regression: rm -rf a real tree still deletes ──────────────────────────
W="$(mktemp -d)"; mkdir -p "$W/a/b/c"; echo x > "$W/a/b/c/f"
RM "rm -rf '$W/a'" >/dev/null 2>&1
[[ ! -d "$W/a" ]] && ok "rm -rf real tree deleted" || no "rm -rf real tree failed"
rm -rf "$W"

# ── 9. BUG-165 DATA LOSS: a quoted filename with a space is ONE argument ──────
# The old string-args rm re-split its joined argv on whitespace (strtok " \t"),
# so `rm 'a b'` became two words → it deleted an unrelated `b` and left `a b`.
# The argv_fn migration (quote-aware dispatch) fixes this at the root. The decoy
# `b` MUST survive; the real target `a b` MUST go.
W="$(mktemp -d)"; : > "$W/a b"; : > "$W/b"
RM "cd '$W' && rm 'a b'" >/dev/null 2>&1
if [[ ! -e "$W/a b" && -e "$W/b" ]]; then
    ok "rm 'a b' removed the spaced file, left decoy 'b' (BUG-165 fixed)"
else no "DATA LOSS — rm re-split a quoted spaced filename (BUG-165 regressed)"; fi
rm -rf "$W"

echo "── rm safety (ADR-0044 Arc A) ──"
echo "PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ "$fail" -eq 0 ]]
