#!/usr/bin/env bash
# jash-agentd is a source-isolated agent runtime, not a stripped Jash UI.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${TMPDIR:-/tmp}/jash-agentd-strict.$$"
LOG="$BUILD/build.log"
trap 'rm -rf "$BUILD"' EXIT INT TERM

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
cmake --build "$BUILD" --target jash-agentd --verbose >"$LOG"
cmake --build "$BUILD" --target jef_verify >/dev/null

BIN="$BUILD/jash-agentd"
test -x "$BIN"

# The strict target must never compile or link human UI, completion, editor,
# Liquid reasoning, or dynamic extension loading.
for source in \
    c/boot.c c/readline/ c/editor/ c/completion/ c/ui/ \
    c/theme/prompt.c c/theme/theme.c c/output/highlighter.c \
    c/easter/bear_egg.c c/plugin.c c/module/loader.c c/liquid/ \
    c/builtins/compgen.c c/builtins/complete.c c/builtins/fc.c \
    c/builtins/builtin_plugin.c c/builtins/module.c \
    c/builtins/builtin_brain.c; do
    if grep -F -- "$source" "$LOG" >/dev/null; then
        echo "jash-agentd compiled forbidden source: $source" >&2
        exit 1
    fi
done

if nm "$BIN" | grep -E \
    'jash_(plugin|liquid|readline)|jmod_(load|find)|statusbar|smart_suggest|snippet_editor' \
    >/dev/null; then
    echo "jash-agentd exports a forbidden human/extension symbol" >&2
    exit 1
fi

HOME_DIR="$BUILD/home"
mkdir -p "$HOME_DIR/.jash/sessions"
PYTHON_BIN="$(command -v python3 || command -v python)"
"$PYTHON_BIN" "$ROOT/tests/test_agent_env_isolation.py" "$BIN"
"$PYTHON_BIN" "$ROOT/tests/test_agent_request_isolation.py" "$BIN"
handshake="$(HOME="$HOME_DIR" "$BIN" </dev/null)"
printf '%s\n' "$handshake" | grep -F '"runtime":"jash-agentd"' >/dev/null
printf '%s\n' "$handshake" | grep -F '"profile":"agent-strict"' >/dev/null
printf '%s\n' "$handshake" | grep -F '"capabilities_required":true' >/dev/null
printf '%s\n' "$handshake" | grep -F '"text_line_authorized":false' >/dev/null
printf '%s\n' "$handshake" | "$PYTHON_BIN" -c '
import json, sys
h = json.load(sys.stdin)
assert h["protocol"] == "jsonl" and h["protocol_version"] == 2, h
assert h["schema_version"] == 18, h
assert h["protocol_versions"] == [1, 2], h
assert h["request_version_default"] == 1, h
assert "execution_spec_sha256" in h["fields"], h
build = h["build"]
assert isinstance(build["git"], str) and build["git"], build
assert build["git_available"] == (build["git"] not in ("unknown", "nogit")), build
assert build["modabi"] == 1 and build["codename"], build
inventory = h["inventory"]
assert inventory["builtins"] > 0, inventory
assert 0 < inventory["effects_classified"] <= inventory["builtins"], inventory
effects = h["effect_map"]
assert len(effects) == inventory["builtins"], (len(effects), inventory)
unknown = {"redo", "why", "resume", "assert", "sense", "forge", "gap", "graph", "mem"}
assert {name for name, effect in effects.items() if effect == "unknown"} == unknown, effects
assert inventory["effects_classified"] == len(effects) - len(unknown), inventory
assert effects["echo"] == "read", effects
assert effects["rm"] == "mutate", effects
assert effects["timeout"] == "exec", effects
assert all(effects[name] == "exec" for name in ("proc", "breathe", "weave")), effects
assert h["external_effect"] == "exec", h
assert h["unknown_effect"] == "exec", h
limits = h["limits"]
assert limits == {
    "request_line_bytes": 32768,
    "argv_items": 64,
    "env_items": 16,
    "decoded_stdin_buffer_bytes": 196607,
    "inline_output_bytes": 32768,
}, limits
assert h["deadline_effects"] == ["effect:exec"], h
assert h["features"] == {
    "streaming": False,
    "background": False,
    "pty": False,
    "binary_stdin": False,
    "completed_state_in_response": True,
}, h
assert "cwd" in h["response_fields"], h
assert "cwd_bytes_hex" in h["response_fields"], h
for field in ("cancelable", "stdout_bytes", "stderr_bytes", "stdout_head",
              "stderr_head", "provenance"):
    assert field in h["response_fields"], (field, h)
assert h["cwd_encoding"] == "byte-preserving-u00", h
assert h["receipts"]["schema"] == "jash.agent-runtime-receipt.v2", h
assert h["receipts"]["media_type"].endswith(".v2+json"), h
'

# Human history, aliases, abbreviations and persisted -c sessions are not part
# of an agent runtime. Hostile user state must neither execute nor be exposed.
printf 'echo HISTORY_SENTINEL\n' >"$HOME_DIR/.jash_history"
printf 'cwd=/tmp\n' >"$HOME_DIR/.jash/sessions/hostile.state"
responses="$(printf '%s\n' \
    '{"id":"echo","argv":["echo","STRICT_OK"],"caps":["effect:read"]}' \
    '{"id":"stdin","argv":["cat"],"stdin":"STDIN_OK\n","caps":["effect:read"]}' \
    '{"id":"after-stdin","argv":["echo","AFTER_STDIN_OK"],"caps":["effect:read"]}' \
    '{"id":"history","argv":["history"],"caps":["effect:mutate"]}' \
    '{"id":"alias","argv":["alias"],"caps":["effect:mutate"]}' \
    '{"id":"abbr","argv":["abbr"],"caps":["effect:mutate"]}' \
    | HOME="$HOME_DIR" JASH_SESSION=hostile "$BIN")"
if printf '%s\n' "$responses" | grep -F 'HISTORY_SENTINEL' >/dev/null; then
    echo "jash-agentd exposed human history" >&2
    exit 1
fi
printf '%s\n' "$responses" \
    | grep -E '"id":"echo".*"rc":0.*STRICT_OK' >/dev/null
printf '%s\n' "$responses" | "$PYTHON_BIN" -c '
import json, sys
rows = {row.get("id"): row for row in map(json.loads, sys.stdin) if row.get("id")}
assert rows["stdin"]["rc"] == 0 and rows["stdin"]["stdout"] == "STDIN_OK\n", rows
assert rows["after-stdin"]["rc"] == 0, rows
'
for id in history alias abbr; do
    printf '%s\n' "$responses" \
        | grep -E "\"id\":\"$id\".*\"rc\":127" >/dev/null
done
test "$(cat "$HOME_DIR/.jash/sessions/hostile.state")" = 'cwd=/tmp'

# Authority is explicit and checked before evaluation: omission and insufficient
# caps leave the target untouched, while the exact declared effect can proceed.
VICTIM="$BUILD/capability-victim"
printf 'keep\n' >"$VICTIM"
cap_responses="$(printf '%s\n' \
    "{\"id\":\"missing\",\"argv\":[\"rm\",\"$VICTIM\"]}" \
    "{\"id\":\"denied\",\"argv\":[\"rm\",\"$VICTIM\"],\"caps\":[\"effect:read\"]}" \
    '{"id":"unknown","argv":["echo","no"],"caps":["effect:future"]}' \
    '{"id":"line","line":"echo no","caps":["effect:exec"]}' \
    "{\"id\":\"allowed\",\"argv\":[\"rm\",\"$VICTIM\"],\"caps\":[\"effect:mutate\"]}" \
    | HOME="$HOME_DIR" "$BIN")"
printf '%s\n' "$cap_responses" | grep -E '"id":"missing".*"rc":125' >/dev/null
printf '%s\n' "$cap_responses" | grep -E '"id":"denied".*"rc":125' >/dev/null
printf '%s\n' "$cap_responses" | grep -E '"id":"unknown".*"rc":125' >/dev/null
printf '%s\n' "$cap_responses" | grep -E '"id":"line".*"rc":125' >/dev/null
printf '%s\n' "$cap_responses" | grep -E '"id":"allowed".*"rc":0' >/dev/null
test ! -e "$VICTIM"

# Native glob is a read-only typed filesystem observation in agent-strict.
GLOB_REQUEST="{\"id\":\"glob-read\",\"argv\":[\"glob\",\"--json\",\"test_jinv*.sh\"],\"cwd\":\"$ROOT/tests\",\"caps\":[\"effect:read\"]}"
GLOB_RESPONSE="$(printf '%s\n' "$GLOB_REQUEST" | HOME="$HOME_DIR" "$BIN")"
printf '%s\n' "$GLOB_RESPONSE" | "$PYTHON_BIN" -c '
import json,sys
rows=[json.loads(line) for line in sys.stdin]
answer=next(row for row in rows if row.get("id")=="glob-read")
assert answer["rc"] == 0, answer
result=json.loads(answer["stdout"])
assert result["schema"] == "jash.glob.v1" and result["complete"] is True, result
assert result["total_matches"] > 0 and any(
    item["path"].endswith("test_jinv001_build_honesty.sh")
    for item in result["matches"]), result
'
FIND_REQUEST="{\"id\":\"find-read\",\"argv\":[\"find\",\"--json\",\"--limit\",\"2\",\".\",\"-type\",\"f\",\"-name\",\"test_*.sh\"],\"cwd\":\"$ROOT/tests\",\"caps\":[\"effect:read\"]}"
FIND_RESPONSE="$(printf '%s\n' "$FIND_REQUEST" | HOME="$HOME_DIR" "$BIN")"
printf '%s\n' "$FIND_RESPONSE" | "$PYTHON_BIN" -c '
import json,sys
rows=[json.loads(line) for line in sys.stdin]
answer=next(row for row in rows if row.get("id")=="find-read")
assert answer["rc"] == 0, answer
result=json.loads(answer["stdout"])
assert result["schema"] == "jash.query.find.v1" and result["complete"] is True, result
assert result["count"] == 2 and result["next_offset"] == 2, result
'
FIND_MARKER="$HOME_DIR/find-external-marker"
FIND_DENIED="$(printf '%s\n' "{\"id\":\"find-no-exec\",\"argv\":[\"find\",\"--json\",\".\",\"-exec\",\"/usr/bin/touch\",\"$FIND_MARKER\",\";\"],\"cwd\":\"$ROOT/tests\",\"caps\":[\"effect:read\"]}" | HOME="$HOME_DIR" "$BIN")"
printf '%s\n' "$FIND_DENIED" | "$PYTHON_BIN" -c '
import json,sys
rows=[json.loads(line) for line in sys.stdin]
answer=next(row for row in rows if row.get("id")=="find-no-exec")
assert answer["rc"] == 2, answer
'
test ! -e "$FIND_MARKER"

# Preserve denial for all formerly unclassified bare commands. proc/breathe/
# weave now declare exec, and assert classifies its actual argv as exec; the
# remaining unclassified invocations still need fallback exec authorization.
unknown_requests="$("$PYTHON_BIN" - <<'PY'
import json
names = ("redo", "why", "resume", "assert", "proc", "breathe",
         "sense", "forge", "weave", "gap", "graph", "mem")
for name in names:
    print(json.dumps({"id": "unknown-effect-" + name, "argv": [name],
                      "caps": ["effect:read"]}, separators=(",", ":")))
PY
)"
unknown_responses="$(printf '%s\n' "$unknown_requests" | HOME="$HOME_DIR" "$BIN")"
printf '%s\n' "$unknown_responses" | "$PYTHON_BIN" -c '
import json, sys
names = {"redo", "why", "resume", "assert", "proc", "breathe",
         "sense", "forge", "weave", "gap", "graph", "mem"}
rows = {r["id"]: r for r in map(json.loads, sys.stdin) if r.get("id")}
assert len(rows) == len(names), rows.keys()
for name in names:
    row = rows["unknown-effect-" + name]
    expected = ("capability denied" if name in {"proc", "breathe", "weave", "assert"}
                else "command effect unknown")
    assert row["rc"] == 125 and row["error"] == expected, row
'
unknown_exec='{"id":"unknown-exec","argv":["forge","authorized"],"caps":["effect:exec"]}'
unknown_exec_response="$(printf '%s\n' "$unknown_exec" | HOME="$HOME_DIR" "$BIN")"
printf '%s\n' "$unknown_exec_response" | "$PYTHON_BIN" -c '
import json, sys
rows = [json.loads(line) for line in sys.stdin]
result = next(row for row in rows if row.get("id") == "unknown-exec")
assert result["rc"] == 0, result
assert result["receipt_ref"], result
assert "// Description: authorized\n" in result["stdout"], result
assert "#error \"forge scaffold: implement the test before use\"" in result["stdout"], result
'

# The canonical structured forge provider is now a classified read operation.
# Its capability is exact: an unrelated exec capability is not a substitute.
forge_native_rows="$(printf '%s\n' \
    '{"id":"forge-native-denied","argv":["forge","--json","authorized"],"caps":["effect:exec"]}' \
    '{"id":"forge-native-read","argv":["forge","--json","authorized"],"caps":["effect:read"]}' \
    | HOME="$HOME_DIR" "$BIN")"
printf '%s\n' "$forge_native_rows" | "$PYTHON_BIN" -c '
import json, sys
rows = {row.get("id"): row for row in map(json.loads, sys.stdin) if row.get("id")}
denied = rows["forge-native-denied"]
assert denied["rc"] == 125 and denied["error"] == "capability denied", denied
assert "receipt_ref" not in denied, denied
allowed = rows["forge-native-read"]
assert allowed["rc"] == 0 and allowed["receipt_ref"], allowed
result = json.loads(allowed["stdout"])
assert result["schema"] == "jash.forge.v1" and result["description"] == "authorized", result
assert result["status"] == "scaffold", result
assert result["implemented"] is False and result["validated"] is False, result
assert result["files_written"] == 0, result
'

# Malformed framing must fail before evaluation and must not poison the next
# request. Every response still needs to be valid JSON for a machine client.
PARSE_VICTIM="$BUILD/parser-victim"
malformed="$(printf '%s\n' \
    "{\"id\":\"trailing\",\"argv\":[\"touch\",\"$PARSE_VICTIM\"],\"caps\":[\"effect:mutate\"]} junk" \
    '{,"id":"leading-comma","argv":["true"],"caps":["effect:read"]}' \
    '{"id":"trailing-comma","argv":["true"],"caps":["effect:read"],}' \
    '{"id":"missing-comma" "argv":["true"],"caps":["effect:read"]}' \
    '{"id":"argv-comma","argv":["true",],"caps":["effect:read"]}' \
    '{"id":"caps-comma","argv":["true"],"caps":[,"effect:read"]}' \
    '{"id":"escape","argv":["echo","bad\q"],"caps":["effect:read"]}' \
    '{"id":"unicode","argv":["echo","bad\u12xz"],"caps":["effect:read"]}' \
    '{"id":"nul","argv":["echo","bad\u0000tail"],"caps":["effect:read"]}' \
    '{"id":"first","id":"second","argv":["true"],"caps":["effect:read"]}' \
    '{"id":"cwd","argv":["true"],"cwd":"/tmp","cwd":"/","caps":["effect:read"]}' \
    '{"id":"caps","argv":["true"],"caps":[],"caps":["effect:read"]}' \
    '{"id":"argv","argv":[],"argv":["true"],"caps":["effect:read"]}' \
    '{"id":"env","argv":["true"],"env":{},"env":{},"caps":["effect:read"]}' \
    '{"id":"spec-duplicate","argv":["true"],"execution_spec_sha256":"sha256:1111111111111111111111111111111111111111111111111111111111111111","execution_spec_sha256":"sha256:2222222222222222222222222222222222222222222222222222222222222222","caps":["effect:read"]}' \
    '{"id":"spec-type","argv":["true"],"execution_spec_sha256":7,"caps":["effect:read"]}' \
    '{"id":"spec-prefix","argv":["true"],"execution_spec_sha256":"sha25:1111111111111111111111111111111111111111111111111111111111111111","caps":["effect:read"]}' \
    '{"id":"spec-upper","argv":["true"],"execution_spec_sha256":"sha256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","caps":["effect:read"]}' \
    '{"id":"spec-short","argv":["true"],"execution_spec_sha256":"sha256:1111","caps":["effect:read"]}' \
    '{"id":"spec-nonhex","argv":["true"],"execution_spec_sha256":"sha256:gggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggg","caps":["effect:read"]}' \
    '{"argv":["true"],"caps":["effect:read"]}' \
    '{"id":7,"argv":["true"],"caps":["effect:read"]}' \
    '{"id":"","argv":["true"],"caps":["effect:read"]}' \
    '{"id":"after","argv":["echo","AFTER_OK"],"caps":["effect:read"]}' \
    | HOME="$HOME_DIR" "$BIN")"
test ! -e "$PARSE_VICTIM"
printf '%s\n' "$malformed" | "$PYTHON_BIN" -c '
import json, sys
rows = [json.loads(line) for line in sys.stdin if line.strip()]
assert len(rows) == 25, len(rows)
assert all("error" in row for row in rows[1:24]), rows
assert rows[24].get("id") == "after" and rows[24].get("rc") == 0, rows[24]
assert "AFTER_OK" in rows[24].get("stdout", ""), rows[24]
'

# Raw NUL is framing data, not the JSON escape \u0000. A valid prefix must
# never execute while trailing bytes are hidden from a strlen-based parser.
"$PYTHON_BIN" - "$BIN" "$HOME_DIR" "$PARSE_VICTIM" <<'PY'
import json, os, subprocess, sys
binary, home, victim = sys.argv[1:]
proc = subprocess.Popen(
    [binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    stderr=subprocess.PIPE, env={**os.environ, "HOME": home},
)
handshake = json.loads(proc.stdout.readline())
assert handshake["runtime"] == "jash-agentd", handshake
payload = (
    b'{"id":"raw-nul","argv":["touch","' + os.fsencode(victim) +
    b'"],"caps":["effect:mutate"]}\0hidden\n'
    b'{"id":"after-nul","argv":["echo","OK"],"caps":["effect:read"]}\n'
)
stdout, stderr = proc.communicate(payload, timeout=10)
rows = [json.loads(line) for line in stdout.splitlines() if line]
assert rows[0]["error"] == "raw NUL in request", rows
assert rows[1]["id"] == "after-nul" and rows[1]["rc"] == 0, rows
assert not os.path.exists(victim), victim
assert proc.returncode == 0, (proc.returncode, stderr)
PY

# Request IDs are idempotency keys, not labels. Exact replay returns the stored
# verdict; a different body conflicts; a recorded running request is
# indeterminate and is never executed again after restart.
IDEM_VICTIM="$BUILD/idempotency-victim"
IDEM_HOME="$BUILD/idem-home"
mkdir -p "$IDEM_HOME"
printf 'keep\n' >"$IDEM_VICTIM"
IDEM_REQ="{\"id\":\"idem-rm\",\"argv\":[\"rm\",\"$IDEM_VICTIM\"],\"caps\":[\"effect:mutate\"]}"
idem_same="$(printf '%s\n%s\n' "$IDEM_REQ" "$IDEM_REQ" | HOME="$IDEM_HOME" "$BIN")"
printf '%s\n' "$idem_same" | "$PYTHON_BIN" -c '
import json, sys
rows = [json.loads(line) for line in sys.stdin if line.strip()][1:]
assert len(rows) == 2 and rows[0].get("rc") == rows[1].get("rc") == 0, rows
assert rows[1] == rows[0], rows
'
test ! -e "$IDEM_VICTIM"

printf 'new generation\n' >"$IDEM_VICTIM"
restart="$(printf '%s\n' "$IDEM_REQ" | HOME="$IDEM_HOME" "$BIN")"
printf '%s\n' "$restart" | grep -E '"id":"idem-rm".*"rc":0' >/dev/null
test -e "$IDEM_VICTIM"

IDEM_FILE="$(find "$IDEM_HOME/.jash/agent_requests" -type f -name '*.req' | grep -m1 .)"
# A completed record from an older response schema must never replay as a
# plausible current result. Strip both state fields, prove indeterminate, then
# restore the valid record for the existing running-state corruption probe.
cp "$IDEM_FILE" "$IDEM_FILE.valid"
"$PYTHON_BIN" - "$IDEM_FILE" <<'PY'
import json, sys
from pathlib import Path
p = Path(sys.argv[1])
digest, state, version, length, checksum, payload = (
    p.read_text(encoding="utf-8").split("\n", 5)
)
assert state == "running" and version == "completed-v1"
row = json.loads(payload)
row.pop("cwd", None)
row.pop("cwd_bytes_hex", None)
# Legacy completed records remain readable across this storage migration. The
# old response schema must still be rejected by the live response contract.
p.write_text(digest + "\ncompleted\n" + json.dumps(row) + "\n", encoding="utf-8")
PY
legacy_replay="$(printf '%s\n' "$IDEM_REQ" | HOME="$IDEM_HOME" "$BIN")"
printf '%s\n' "$legacy_replay" \
    | grep -E '"id":"idem-rm".*"outcome":"indeterminate"' >/dev/null
mv "$IDEM_FILE.valid" "$IDEM_FILE"
"$PYTHON_BIN" - "$IDEM_FILE" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
digest = p.read_text(encoding="utf-8").splitlines()[0]
p.write_text(digest + "\nrunning\n", encoding="utf-8")
PY
indeterminate="$(printf '%s\n' "$IDEM_REQ" | HOME="$IDEM_HOME" "$BIN")"
printf '%s\n' "$indeterminate" \
    | grep -E '"id":"idem-rm".*"outcome":"indeterminate"' >/dev/null
test -e "$IDEM_VICTIM"

CONFLICT_VICTIM="$BUILD/conflict-victim"
conflict="$(printf '%s\n' \
    '{"id":"idem-conflict","argv":["echo","first"],"caps":["effect:read"]}' \
    "{\"id\":\"idem-conflict\",\"argv\":[\"touch\",\"$CONFLICT_VICTIM\"],\"caps\":[\"effect:mutate\"]}" \
    | HOME="$IDEM_HOME" "$BIN")"
printf '%s\n' "$conflict" | grep -E '"id":"idem-conflict".*idempotency conflict' >/dev/null
test ! -e "$CONFLICT_VICTIM"

CWD_START="$BUILD/cwd-start"
CWD_FINAL="$BUILD/cwd-final"
mkdir -p "$CWD_START" "$CWD_FINAL"
cwd_rows="$(printf '%s\n' \
    "{\"id\":\"sticky-cd\",\"argv\":[\"cd\",\"$CWD_FINAL\"],\"cwd\":\"$CWD_START\",\"caps\":[\"effect:mutate\"]}" \
    '{"id":"sticky-pwd","argv":["pwd"],"caps":["effect:read"]}' \
    | HOME="$IDEM_HOME" "$BIN")"
printf '%s\n' "$cwd_rows" | "$PYTHON_BIN" -c '
import json, os, sys
rows = [json.loads(line) for line in sys.stdin if line.strip()][1:]
assert rows[0]["rc"] == 0, rows[0]
assert rows[1]["rc"] == 0, rows[1]
assert os.path.realpath(rows[0]["cwd"]) == os.path.realpath(sys.argv[1]), rows[0]
assert os.path.realpath(rows[1]["cwd"]) == os.path.realpath(sys.argv[1]), rows[1]
assert bytes.fromhex(rows[0]["cwd_bytes_hex"]).decode() == sys.argv[1], rows[0]
assert bytes.fromhex(rows[1]["cwd_bytes_hex"]).decode() == sys.argv[1], rows[1]
assert os.path.realpath(rows[1]["stdout"].strip()) == os.path.realpath(sys.argv[1]), rows[1]
' "$CWD_FINAL"

# A request cwd is logical state. Validation proves it names the actual
# directory without silently canonicalizing a symlink into another string.
CWD_LINK="$BUILD/cwd-link"
ln -s "$CWD_FINAL" "$CWD_LINK"
mkdir -p "$BUILD/logical-home"
logical_rows="$(printf '%s\n' \
    "{\"id\":\"logical-cwd\",\"argv\":[\"pwd\"],\"cwd\":\"$CWD_LINK\",\"caps\":[\"effect:read\"]}" \
    | HOME="$BUILD/logical-home" "$BIN")"
printf '%s\n' "$logical_rows" | "$PYTHON_BIN" -c '
import json, sys
row = [json.loads(line) for line in sys.stdin if line.strip()][1]
assert row["cwd"] == sys.argv[1], row
assert bytes.fromhex(row["cwd_bytes_hex"]).decode() == sys.argv[1], row
assert row["stdout"].strip() == sys.argv[1], row
' "$CWD_LINK"

REL_CWD="$("$PYTHON_BIN" -c 'import os,sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))' \
    "$CWD_FINAL" "$ROOT")"
relative_rows="$(printf '%s\n' \
    "{\"id\":\"relative-cwd\",\"argv\":[\"pwd\"],\"cwd\":\"$REL_CWD\",\"caps\":[\"effect:read\"]}" \
    | HOME="$BUILD/logical-home" "$BIN")"
printf '%s\n' "$relative_rows" | "$PYTHON_BIN" -c '
import json, os, sys
row = [json.loads(line) for line in sys.stdin if line.strip()][1]
assert row["rc"] == 0, row
assert os.path.realpath(row["cwd"]) == os.path.realpath(sys.argv[1]), row
assert row["stdout"].strip() == row["cwd"], row
' "$CWD_FINAL"

# If the directory is renamed while a command runs, recover its new pathname
# from the kernel instead of restoring a stale logical path.
RENAME_FROM="$BUILD/rename-from"
RENAME_TO="$BUILD/rename-to"
mkdir "$RENAME_FROM"
rename_rows="$(printf '%s\n' \
    "{\"id\":\"rename-cwd\",\"argv\":[\"mv\",\"$RENAME_FROM\",\"$RENAME_TO\"],\"cwd\":\"$RENAME_FROM\",\"caps\":[\"effect:mutate\"]}" \
    '{"id":"after-rename-cwd","argv":["pwd"],"caps":["effect:read"]}' \
    | HOME="$BUILD/logical-home" "$BIN")"
printf '%s\n' "$rename_rows" | "$PYTHON_BIN" -c '
import json, sys
rows = [json.loads(line) for line in sys.stdin if line.strip()][1:]
assert rows[0]["rc"] == 0 and rows[0]["cwd"] == sys.argv[1], rows[0]
assert rows[1]["rc"] == 0 and rows[1]["cwd"] == sys.argv[1], rows[1]
assert rows[1]["stdout"].strip() == sys.argv[1], rows[1]
' "$RENAME_TO"

# Request, response, capture backups and rollback capabilities belong to the
# runtime only. External commands must not inherit any fd >= 3.
mkdir -p "$BUILD/fd-home"
fd_request="$("$PYTHON_BIN" - "$PYTHON_BIN" <<'PY'
import json, sys
probe = ('import os\n'
         'seen=[]\n'
         'for fd in range(3,64):\n'
         ' try:\n  os.fstat(fd); seen.append((fd,os.readlink("/proc/self/fd/"+str(fd))))\n'
         ' except OSError:\n  pass\n'
         'print(seen)')
print(json.dumps({"id":"no-cwd-fd-leak","argv":[sys.argv[1],"-c",probe],
                  "caps":["effect:exec"]}))
PY
)"
fd_rows="$(printf '%s\n' "$fd_request" | HOME="$BUILD/fd-home" "$BIN")"
printf '%s\n' "$fd_rows" | "$PYTHON_BIN" -c '
import json, sys
row = [json.loads(line) for line in sys.stdin if line.strip()][1]
assert row["rc"] == 0 and row["stdout"].strip() == "[]", row
'

# State is byte-exact even for legal POSIX paths that are not UTF-8. The JSON
# stream must remain parseable; cwd_bytes_hex is the authoritative round-trip.
"$PYTHON_BIN" - "$BIN" "$IDEM_HOME" "$BUILD" <<'PY'
import json, os, subprocess, sys
binary, home, build = sys.argv[1:]
raw_cwd = os.fsencode(build) + b"/cwd-\xff"
os.mkdir(raw_cwd)
proc = subprocess.Popen(
    [os.fsencode(binary)], cwd=raw_cwd,
    env={**os.environ, "HOME": home},
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
)
handshake = json.loads(proc.stdout.readline())
assert handshake["cwd_encoding"] == "byte-preserving-u00", handshake
proc.stdin.write(b'{"id":"byte-cwd","argv":["true"],"caps":["effect:read"]}\n')
proc.stdin.close()
response = json.loads(proc.stdout.readline())
assert bytes.fromhex(response["cwd_bytes_hex"]) == raw_cwd, response
assert response["rc"] == 0 and proc.wait() == 0, response
PY

# A cwd that the state model cannot represent must fail before evaluation;
# the following request proves the daemon kept its prior directory.
LONG_CWD="$BUILD/cwd-long"
mkdir -p "$LONG_CWD"
while test "${#LONG_CWD}" -lt 540; do
    LONG_CWD="$LONG_CWD/segment123456789012345678901234567890"
    mkdir "$LONG_CWD"
done
long_rows="$("$PYTHON_BIN" - "$LONG_CWD" <<'PY' | HOME="$IDEM_HOME" "$BIN"
import json, sys
cwd = sys.argv[1]
print(json.dumps({"id":"long-cwd","argv":["pwd"],"cwd":cwd,
                  "caps":["effect:read"]}))
print(json.dumps({"id":"after-long-cwd","argv":["pwd"],
                  "caps":["effect:read"]}))
PY
)"
printf '%s\n' "$long_rows" | "$PYTHON_BIN" -c '
import json, os, sys
rows = [json.loads(line) for line in sys.stdin if line.strip()][1:]
assert "error" in rows[0] and "cwd" in rows[0]["error"], rows[0]
assert rows[1]["rc"] == 0, rows[1]
assert os.path.realpath(rows[1]["cwd"]) == os.path.realpath(sys.argv[1]), rows[1]
' "$ROOT"
"$PYTHON_BIN" - "$BIN" "$IDEM_HOME" "$LONG_CWD" <<'PY'
import json, os, subprocess, sys
binary, home, cwd = sys.argv[1:]
proc = subprocess.run(
    [binary], cwd=cwd, env={**os.environ, "HOME": home},
    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    text=True, check=False,
)
row = json.loads(proc.stdout)
assert proc.returncode == 125 and "initial cwd" in row["error"], (proc, row)
PY

# 511 path bytes plus NUL fit exactly in JASH_CWD_MAX=512.
"$PYTHON_BIN" - "$BIN" "$BUILD" <<'PY'
import json, os, subprocess, sys
binary, build = sys.argv[1:]
path = os.fsencode(build + "/cwd-max")
os.mkdir(path)
while len(path) < 511:
    remaining = 511 - len(path)
    size = min(100, remaining - 1)
    if size <= 0:
        raise AssertionError((len(path), remaining))
    path += b"/" + b"x" * size
    os.mkdir(path)
assert len(path) == 511, len(path)
home = os.fsencode(build + "/cwd-max-home")
os.mkdir(home)
env = os.environb.copy()
env[b"HOME"] = home
proc = subprocess.Popen(
    [os.fsencode(binary)], cwd=path, env=env,
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
)
json.loads(proc.stdout.readline())
proc.stdin.write(b'{"id":"max-cwd","argv":["true"],"caps":["effect:read"]}\n')
proc.stdin.close()
row = json.loads(proc.stdout.readline())
assert row["rc"] == 0 and bytes.fromhex(row["cwd_bytes_hex"]) == path, row
assert proc.wait() == 0
PY

# The shared cd provider refuses an unrepresentable logical cwd before chdir.
# Keep that complete refusal distinct from the daemon's post-evaluation guard:
# a short symlink can represent a long physical cwd, until the evaluated command
# removes that link. The guard must then restore its directory fd and report the
# completed filesystem effect as indeterminate, never leave split cwd state.
"$PYTHON_BIN" - "$BIN" "$IDEM_HOME" "$LONG_CWD" "$BUILD" "$ROOT" <<'PY'
import errno, json, os, shlex, subprocess, sys
binary, home, long_cwd, build, root = sys.argv[1:]
assert len(os.fsencode(long_cwd)) >= 540 and os.path.isdir(long_cwd), long_cwd
physical_root = os.path.realpath(root)

def run(requests):
    proc = subprocess.run(
        [binary], cwd=root, env={**os.environ, "HOME": home},
        input="".join(json.dumps(request) + "\n" for request in requests),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=20,
    )
    assert proc.returncode == 0, (proc.returncode, proc.stdout, proc.stderr)
    handshake, *rows = [json.loads(line) for line in proc.stdout.splitlines() if line.strip()]
    assert len(rows) == len(requests), rows
    for request, row in zip(requests, rows):
        assert row["id"] == request["id"], row
        assert row["receipt_ref"], row
        assert row["receipt_pubkey"] == handshake["receipts"]["pubkey"], row
    return rows

before, refused, after, environment, kernel = run([
    {"id":"before-builtin-long-env", "argv":["env", "--json"], "caps":["effect:read"]},
    {"id":"builtin-long-cwd", "argv":["cd", long_cwd], "caps":["effect:mutate"]},
    {"id":"after-builtin-long-cwd", "argv":["pwd"], "caps":["effect:read"]},
    {"id":"after-builtin-long-env", "argv":["env", "--json"], "caps":["effect:read"]},
    {"id":"after-builtin-long-kernel", "argv":["/bin/pwd", "-P"], "caps":["effect:exec"]},
])
assert refused["rc"] == 1 and "error" not in refused and "outcome" not in refused, refused
assert os.strerror(errno.ENAMETOOLONG) in refused["stderr"] and not refused["stdout"], refused
assert os.path.realpath(refused["cwd"]) == physical_root, refused
assert after["rc"] == 0 and os.path.realpath(after["cwd"]) == physical_root, after
assert kernel["rc"] == 0 and kernel["stdout"] == physical_root + "\n", kernel
assert before["rc"] == environment["rc"] == 0, (before, environment)
prior, current = json.loads(before["stdout"]), json.loads(environment["stdout"])
for key in ("PWD", "OLDPWD"):
    assert (key in prior, prior.get(key)) == (key in current, current.get(key)), (key, prior, current)

link = os.path.join(build, "cwd-short-link")
assert len(os.fsencode(link)) < 512, link
os.symlink(long_cwd, link)
target_before = os.stat(long_cwd)
# The strict profile deliberately excludes eval/source. A real recipe runs
# its steps in the daemon's current state and publishes each observed result.
# Its commands' stdout goes to stderr, separate from the structured receipts.
commands = ["cd -- " + shlex.quote(link), "/bin/pwd -P", "/bin/rm -- " + shlex.quote(link)]
recipe_name = "strict-cwd-guard"
recipe_directory = os.path.join(home, ".jash", "recipes")
os.makedirs(recipe_directory, mode=0o700, exist_ok=True)
with open(os.path.join(recipe_directory, recipe_name + ".jashrc"), "w") as recipe:
    recipe.write("name " + recipe_name + "\ncreated 1700000000\nsession fixture\nrange 1 3\n")
    recipe.writelines("C " + command + "\n" for command in commands)
guarded, after, environment, kernel = run([
    {"id":"post-eval-long-cwd", "argv":["recipe", "--json", "run", recipe_name],
     "caps":["effect:exec"]},
    {"id":"after-post-eval-long-cwd", "argv":["pwd"], "caps":["effect:read"]},
    {"id":"after-post-eval-long-env", "argv":["env", "--json"], "caps":["effect:read"]},
    {"id":"after-post-eval-long-kernel", "argv":["/bin/pwd", "-P"], "caps":["effect:exec"]},
])
assert guarded["rc"] == 125 and guarded["outcome"] == "indeterminate", guarded
assert guarded["error"] == "cwd exceeds runtime state limit", guarded
assert guarded["stderr"] == os.path.realpath(long_cwd) + "\n", guarded
receipts = [json.loads(line) for line in guarded["stdout"].splitlines() if line.strip()]
assert [row["row_kind"] for row in receipts] == ["checkpoint", "step", "step", "step", "result"], receipts
assert receipts[0]["checkpoint"]["applied"] and receipts[0]["checkpoint"]["durable"], receipts[0]
assert os.path.isfile(receipts[0]["checkpoint"]["path"]), receipts[0]
for index, (command, receipt) in enumerate(zip(commands, receipts[1:4]), 1):
    assert receipt["index"] == index and receipt["command"] == command, receipt
    assert receipt["attempted"] and receipt["command_status"] == 0 and receipt["error"] is None, receipt
assert receipts[-1]["completed"] and receipts[-1]["steps_completed"] == 3, receipts[-1]
assert receipts[-1]["failed_commands"] == receipts[-1]["command_status"] == 0, receipts[-1]
assert not os.path.lexists(link), link
target_after = os.stat(long_cwd)
assert (target_before.st_dev, target_before.st_ino) == (target_after.st_dev, target_after.st_ino)
for row in (guarded, after, environment, kernel):
    assert os.path.realpath(row["cwd"]) == physical_root, row
assert after["rc"] == environment["rc"] == kernel["rc"] == 0, (after, environment, kernel)
assert os.path.realpath(json.loads(environment["stdout"])["PWD"]) == physical_root, environment
assert kernel["stdout"] == physical_root + "\n", kernel
PY

# A rejection discovered after reserving an id is not a completed operation and
# must not persist as an unsigned replay record. A corrected retry may execute.
retry_rows="$(printf '%s\n' \
    '{"id":"retry-after-reject","argv":["echo","BAD"],"cwd":"/definitely/not/here","caps":["effect:read"]}' \
    '{"id":"retry-after-reject","argv":["echo","GOOD"],"caps":["effect:read"]}' \
    | HOME="$IDEM_HOME" "$BIN")"
printf '%s\n' "$retry_rows" | "$PYTHON_BIN" -c '
import json, sys
rows = [json.loads(line) for line in sys.stdin if line.strip()][1:]
first, second = rows
assert first["id"] == "retry-after-reject" and "error" in first, first
assert "receipt_ref" not in first, first
assert second["rc"] == 0 and second["stdout"] == "GOOD\n", second
assert second.get("replayed") is not True and second["receipt_ref"], second
'

# timeout_ms supervises explicit exec authority through the native timeout
# process-group implementation. It preserves partial output, kills descendants,
# and leaves the daemon usable; non-exec operations reject the field visibly.
# Allow shell startup under a loaded master run before requiring PARTIAL output;
# the 30-second descendant still must be killed after a one-second budget.
TIMEOUT_HOME="$BUILD/timeout-home"
TIMEOUT_PID="$BUILD/timeout-child.pid"
mkdir -p "$TIMEOUT_HOME"
t0="$(date +%s)"
timeout_responses="$(printf '%s\n' \
    '{"id":"fast","argv":["sh","-c","printf FAST"],"caps":["effect:exec"],"timeout_ms":2000}' \
    '{"id":"natural-124","argv":["sh","-c","exit 124"],"caps":["effect:exec"],"timeout_ms":2000}' \
    '{"id":"slow","argv":["sh","-c","printf PARTIAL; sleep 30 & echo $! > '"$TIMEOUT_PID"'; wait"],"caps":["effect:exec"],"timeout_ms":1000}' \
    '{"id":"wrong-effect","argv":["echo","no"],"caps":["effect:read"],"timeout_ms":200}' \
    '{"id":"after-timeout","argv":["echo","STILL_ALIVE"],"caps":["effect:read"]}' \
    | HOME="$TIMEOUT_HOME" "$BIN")"
t1="$(date +%s)"
test "$((t1 - t0))" -lt 5
printf '%s\n' "$timeout_responses" | "$PYTHON_BIN" -c '
import json, sys
rows = [json.loads(line) for line in sys.stdin if line.strip()]
by_id = {row.get("id"): row for row in rows[1:]}
assert by_id["fast"].get("rc") == 0 and "FAST" in by_id["fast"].get("stdout", ""), by_id
assert by_id["fast"].get("timed_out") is False, by_id["fast"]
assert by_id["natural-124"].get("rc") == 124, by_id["natural-124"]
assert by_id["natural-124"].get("timed_out") is False, by_id["natural-124"]
assert by_id["slow"].get("rc") == 124, by_id["slow"]
assert by_id["slow"].get("timed_out") is True, by_id["slow"]
assert "PARTIAL" in by_id["slow"].get("stdout", ""), by_id["slow"]
assert by_id["wrong-effect"].get("rc") == 125, by_id["wrong-effect"]
assert "effect:exec" in by_id["wrong-effect"].get("error", ""), by_id["wrong-effect"]
assert by_id["after-timeout"].get("rc") == 0, by_id["after-timeout"]
assert "STILL_ALIVE" in by_id["after-timeout"].get("stdout", ""), by_id["after-timeout"]
'
if test -s "$TIMEOUT_PID" && kill -0 "$(cat "$TIMEOUT_PID")" 2>/dev/null; then
    echo "timeout left a descendant running" >&2
    kill -KILL "$(cat "$TIMEOUT_PID")" 2>/dev/null || true
    exit 1
fi

# Explicit cancellation uses a separate inherited socket because the JSONL
# request loop is synchronous. A wrong ID must not cancel; the matching ID kills
# the whole process group, preserves partial output, persists the terminal
# verdict for replay, and leaves the daemon usable.
"$PYTHON_BIN" - "$BIN" "$BUILD" <<'PY'
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time

binary = sys.argv[1]
root = Path(sys.argv[2])
home = root / "cancel-home"
def wait_pid(path):
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        try:
            text = path.read_text()
        except FileNotFoundError:
            text = ""
        if text.endswith("\n") and text[:-1].isdigit() and int(text) > 0:
            return int(text)
        time.sleep(0.02)
    raise AssertionError(f"descendant did not publish a complete PID: {path}")

home.mkdir()
pid_file = root / "cancel-child.pid"
parent, child = socket.socketpair()
env = dict(os.environ, HOME=str(home), JASH_AGENT_CANCEL_FD=str(child.fileno()))
proc = subprocess.Popen(
    [binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    stderr=subprocess.PIPE, text=True, env=env, pass_fds=(child.fileno(),)
)
child.close()
assert proc.stdin and proc.stdout
handshake = json.loads(proc.stdout.readline())
assert handshake["cancel"]["transport"] == "fd-jsonl", handshake
assert handshake["cancel"]["activation_ack"] is True, handshake

# A frame queued before its request is stale, not authorization to cancel a
# future operation that happens to reuse the same ID.
parent.sendall(b'{"cancel":"future-id"}\n')
proc.stdin.write(json.dumps({
    "id": "future-id", "argv": ["sleep", "0.05"],
    "caps": ["effect:exec"], "timeout_ms": 1000, "cancelable": True,
}, separators=(",", ":")) + "\n")
proc.stdin.flush()
active = json.loads(proc.stdout.readline())
assert active == {"id": "future-id", "active": True, "cancelable": True}, active
future = json.loads(proc.stdout.readline())
assert future["rc"] == 0 and future["cancelled"] is False, future

request = json.dumps({
    "id": "cancel-one",
    "argv": ["sh", "-c", f"printf PARTIAL; (trap \'\' TERM; sleep 30) & echo $! > {pid_file}; wait"],
    "caps": ["effect:exec"], "timeout_ms": 10000, "cancelable": True,
}, separators=(",", ":"))
proc.stdin.write(request + "\n")
proc.stdin.flush()
active = json.loads(proc.stdout.readline())
assert active == {"id": "cancel-one", "active": True, "cancelable": True}, active
descendant = wait_pid(pid_file)

parent.sendall(b'{"cancel":"wrong-id"}\n')
wrong = json.loads(parent.recv(4096))
assert wrong == {"cancel": "wrong-id", "accepted": False,
                 "error": "request is not active"}, wrong
parent.sendall(b'{"cancel":"cancel-one"}\n')
accepted = json.loads(parent.recv(4096))
assert accepted == {"cancel": "cancel-one", "accepted": True}, accepted

cancelled = json.loads(proc.stdout.readline())
assert cancelled["id"] == "cancel-one" and cancelled["rc"] == 130, cancelled
assert cancelled["cancelled"] is True and cancelled["timed_out"] is False, cancelled
assert "PARTIAL" in cancelled.get("stdout", ""), cancelled
try:
    os.kill(descendant, 0)
except ProcessLookupError:
    pass
else:
    raise AssertionError(f"receipt published before descendant {descendant} died")

proc.stdin.write(json.dumps({"id": "after-cancel", "argv": ["echo", "ALIVE"],
                             "caps": ["effect:read"]}) + "\n")
proc.stdin.flush()
after = json.loads(proc.stdout.readline())
assert after["rc"] == 0 and "ALIVE" in after.get("stdout", ""), after

proc.stdin.write(request + "\n")
proc.stdin.flush()
replayed = json.loads(proc.stdout.readline())
assert replayed["rc"] == 130 and replayed["cancelled"] is True, replayed
assert replayed == cancelled, (replayed, cancelled)

proc.stdin.write(json.dumps({"id": "bad-cancel", "argv": ["sh", "-c", "true"],
                             "caps": ["effect:exec"], "cancelable": True}) + "\n")
proc.stdin.flush()
bad = json.loads(proc.stdout.readline())
assert bad["rc"] == 125 and "cancelable" in bad["error"], bad

proc.stdin.close()
assert proc.wait(timeout=5) == 0, proc.stderr.read()
parent.close()
try:
    os.kill(descendant, 0)
except ProcessLookupError:
    pass
else:
    raise AssertionError(f"cancel left descendant {descendant} alive")

# Closing the peer after sending a valid frame must not SIGPIPE the daemon
# before it kills the workload.
parent, child = socket.socketpair()
home2 = root / "cancel-close-home"
home2.mkdir()
pid_file2 = root / "cancel-close-child.pid"
env = dict(os.environ, HOME=str(home2), JASH_AGENT_CANCEL_FD=str(child.fileno()))
proc = subprocess.Popen(
    [binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    stderr=subprocess.PIPE, text=True, env=env, pass_fds=(child.fileno(),)
)
child.close()
assert proc.stdin and proc.stdout
json.loads(proc.stdout.readline())
request2 = json.dumps({
    "id": "cancel-close",
    # An existing empty PID file is deliberately observable before publication.
    # Cancelling on mere existence would kill the writer before echo completes.
    "argv": ["sh", "-c", f'sleep 30 & child=$!; : > {pid_file2}; sleep 0.1; echo "$child" > {pid_file2}; wait'],
    "caps": ["effect:exec"], "timeout_ms": 10000, "cancelable": True,
}, separators=(",", ":"))
proc.stdin.write(request2 + "\n")
proc.stdin.flush()
active = json.loads(proc.stdout.readline())
assert active == {"id": "cancel-close", "active": True, "cancelable": True}, active
descendant2 = wait_pid(pid_file2)
parent.sendall(b'{"cancel":"cancel-close"}\n')
parent.close()
closed_peer = json.loads(proc.stdout.readline())
assert closed_peer["rc"] == 130 and closed_peer["cancelled"] is True, closed_peer
proc.stdin.close()
assert proc.wait(timeout=5) == 0, proc.stderr.read()
try:
    os.kill(descendant2, 0)
except ProcessLookupError:
    pass
else:
    raise AssertionError("closed cancel peer left descendant alive")
PY

# Every completed request publishes a signed DSSE receipt before its idempotency
# record can become completed. The standalone verifier pins the handshake key;
# changing the artifact must fail while the untouched receipt still verifies.
SEED_RACE_HOME="$BUILD/seed-race-home"
mkdir -p "$SEED_RACE_HOME"
"$PYTHON_BIN" - "$BIN" "$SEED_RACE_HOME" <<'PY'
import json
import os
from pathlib import Path
import subprocess
import sys

binary, home = sys.argv[1:]
env = dict(os.environ, HOME=home)
procs = [subprocess.Popen([binary], stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, env=env) for _ in range(16)]
keys = set()
for proc in procs:
    stdout, stderr = proc.communicate(timeout=10)
    assert proc.returncode == 0, (proc.returncode, stderr)
    keys.add(json.loads(stdout.splitlines()[0])["receipts"]["pubkey"])
assert len(keys) == 1, keys
seed = Path(home) / ".jash" / "keys" / "ed25519.seed"
assert seed.stat().st_size == 32
PY

RECEIPT_HOME="$BUILD/receipt-home"
mkdir -p "$RECEIPT_HOME"
RECEIPT_REQUEST='{"id":"signed-one","argv":["echo","SIGNED"],"execution_spec_sha256":"sha256:1111111111111111111111111111111111111111111111111111111111111111","caps":["effect:read"]}'
printf '%s\n' "$RECEIPT_REQUEST" > "$BUILD/receipt-request.json"
receipt_rows="$(printf '%s\n' "$RECEIPT_REQUEST" | HOME="$RECEIPT_HOME" "$BIN")"
receipt_response="$BUILD/receipt-response.json"
receipt_meta="$(printf '%s\n' "$receipt_rows" | "$PYTHON_BIN" -c '
import json, sys
lines = [line for line in sys.stdin if line.strip()]
handshake, response = [json.loads(line) for line in lines]
assert handshake["receipts"]["required"] is True, handshake
assert handshake["receipts"]["schema"] == "jash.agent-runtime-receipt.v2", handshake
assert response["rc"] == 0 and response["receipt_ref"], response
assert response["receipt_pubkey"] == handshake["receipts"]["pubkey"], response
open(sys.argv[1], "w", encoding="utf-8").write(lines[1].rstrip("\n") + "\n")
print(response["receipt_pubkey"], response["receipt_ref"])
' "$receipt_response")"
receipt_pubkey="${receipt_meta%% *}"
receipt_ref="${receipt_meta#* }"
receipt_path="$RECEIPT_HOME/.jash/agent_receipts/$receipt_ref"
test -f "$receipt_path"
"$BUILD/jef_verify" proof-pinned "$receipt_pubkey" "$receipt_path" >/dev/null
"$BUILD/jef_verify" runtime-receipt "$receipt_pubkey" \
    "$BUILD/receipt-request.json" "$receipt_response" "$receipt_path" >/dev/null
cp "$receipt_path" "$BUILD/sealed-receipt-snapshot"
if "$BUILD/jef_verify" runtime-receipt "$receipt_pubkey" \
    "$BUILD/receipt-request.json" "$receipt_response" \
    "$BUILD/sealed-receipt-snapshot" >/dev/null 2>&1; then
    echo "runtime receipt accepted a renamed snapshot without an explicit ref" >&2
    exit 1
fi
"$BUILD/jef_verify" runtime-receipt "$receipt_pubkey" \
    "$BUILD/receipt-request.json" "$receipt_response" \
    "$BUILD/sealed-receipt-snapshot" "$receipt_ref" >/dev/null
replay_rows="$(printf '%s\n' "$RECEIPT_REQUEST" | HOME="$RECEIPT_HOME" "$BIN")"
printf '%s\n' "$replay_rows" | "$PYTHON_BIN" -c '
import sys
lines = [line.rstrip("\n") for line in sys.stdin if line.strip()]
stored = open(sys.argv[1], encoding="utf-8").read().rstrip("\n")
assert lines[1] == stored, (lines[1], stored)
open(sys.argv[2], "w", encoding="utf-8").write(lines[1] + "\n")
' "$receipt_response" "$BUILD/receipt-replay-response.json"
"$BUILD/jef_verify" runtime-receipt "$receipt_pubkey" \
    "$BUILD/receipt-request.json" "$BUILD/receipt-replay-response.json" \
    "$receipt_path" >/dev/null

# The agent contract declares background=false. A leader that exits after
# spawning a descendant must not publish completion while that process lives.
QUIET_REQUEST='{"id":"quiescent","argv":["sh","-c","sleep 30 & echo $!"],"caps":["effect:exec"],"timeout_ms":2000}'
quiet_rows="$(printf '%s\n' "$QUIET_REQUEST" | HOME="$RECEIPT_HOME" "$BIN")"
quiet_pid="$(printf '%s\n' "$quiet_rows" | "$PYTHON_BIN" -c '
import json, sys
response = [json.loads(line) for line in sys.stdin if line.strip()][1]
assert response["rc"] == 0 and response["receipt_ref"], response
print(response["stdout"].strip())
')"
if kill -0 "$quiet_pid" 2>/dev/null; then
    echo "agent published completion with a live background descendant" >&2
    kill -KILL "$quiet_pid" 2>/dev/null || true
    exit 1
fi

OVERFLOW_REQUEST='{"id":"signed-overflow","argv":["head","-c","67108865","/dev/zero"],"caps":["effect:exec"]}'
printf '%s\n' "$OVERFLOW_REQUEST" > "$BUILD/overflow-request.json"
overflow_rows="$(printf '%s\n' "$OVERFLOW_REQUEST" | HOME="$RECEIPT_HOME" "$BIN")"
overflow_meta="$(printf '%s\n' "$overflow_rows" | "$PYTHON_BIN" -c '
import json, sys
lines = [line for line in sys.stdin if line.strip()]
response = json.loads(lines[1])
assert response["rc"] == 0 and response["stdout_bytes"] == 67108865, response
assert response["stdout_ref"] and response["receipt_ref"], response
open(sys.argv[1], "w", encoding="utf-8").write(lines[1].rstrip("\n") + "\n")
print(response["receipt_ref"], response["stdout_ref"])
' "$BUILD/overflow-response.json")"
overflow_receipt_ref="${overflow_meta%% *}"
overflow_stdout_ref="${overflow_meta#* }"
"$BUILD/jef_verify" runtime-receipt "$receipt_pubkey" \
    "$BUILD/overflow-request.json" "$BUILD/overflow-response.json" \
    "$RECEIPT_HOME/.jash/agent_receipts/$overflow_receipt_ref" >/dev/null
printf X >> "$RECEIPT_HOME/.jash/overflow/$overflow_stdout_ref"
if "$BUILD/jef_verify" runtime-receipt "$receipt_pubkey" \
    "$BUILD/overflow-request.json" "$BUILD/overflow-response.json" \
    "$RECEIPT_HOME/.jash/agent_receipts/$overflow_receipt_ref" \
    >/dev/null 2>&1; then
    echo "runtime receipt accepted a tampered overflow artifact" >&2
    exit 1
fi
rm -f "$RECEIPT_HOME/.jash/overflow/$overflow_stdout_ref"
mkfifo "$RECEIPT_HOME/.jash/overflow/$overflow_stdout_ref"
"$PYTHON_BIN" - "$BUILD/jef_verify" "$receipt_pubkey" \
    "$BUILD/overflow-request.json" "$BUILD/overflow-response.json" \
    "$RECEIPT_HOME/.jash/agent_receipts/$overflow_receipt_ref" <<'PY'
import subprocess
import sys
try:
    result = subprocess.run(
        [sys.argv[1], "runtime-receipt", *sys.argv[2:]],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=2,
    )
except subprocess.TimeoutExpired as exc:
    raise AssertionError("runtime receipt verifier hung on an overflow FIFO") from exc
assert result.returncode != 0, "runtime receipt verifier accepted an overflow FIFO"
PY
rm -f "$RECEIPT_HOME/.jash/overflow/$overflow_stdout_ref"

FOREIGN_HOME="$BUILD/foreign-receipt-home"
mkdir -p "$FOREIGN_HOME"
foreign_rows="$(printf '%s\n' \
    '{"id":"foreign","argv":["echo","FOREIGN"],"caps":["effect:read"]}' \
    | HOME="$FOREIGN_HOME" "$BIN")"
foreign_meta="$(printf '%s\n' "$foreign_rows" | "$PYTHON_BIN" -c '
import json, sys
rows = [json.loads(line) for line in sys.stdin if line.strip()]
print(rows[1]["receipt_pubkey"], rows[1]["receipt_ref"])
')"
foreign_pubkey="${foreign_meta%% *}"
foreign_ref="${foreign_meta#* }"
test "$foreign_pubkey" != "$receipt_pubkey"
if "$BUILD/jef_verify" proof-pinned "$receipt_pubkey" \
    "$FOREIGN_HOME/.jash/agent_receipts/$foreign_ref" >/dev/null 2>&1; then
    echo "foreign self-signed runtime receipt passed pinned verification" >&2
    exit 1
fi
cp "$receipt_path" "$BUILD/tampered-receipt.dsse"
"$PYTHON_BIN" - "$BUILD/tampered-receipt.dsse" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
data = p.read_text()
needle = '"payload":"'
i = data.index(needle) + len(needle)
replacement = "A" if data[i] != "A" else "B"
p.write_text(data[:i] + replacement + data[i + 1:])
PY
if "$BUILD/jef_verify" proof-pinned "$receipt_pubkey" \
    "$BUILD/tampered-receipt.dsse" >/dev/null 2>&1; then
    echo "tampered runtime receipt verified" >&2
    exit 1
fi
"$BUILD/jef_verify" proof-pinned "$receipt_pubkey" "$receipt_path" >/dev/null
cp "$receipt_response" "$BUILD/tampered-response.json"
"$PYTHON_BIN" - "$BUILD/tampered-response.json" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
data = p.read_text()
assert '"rc":0' in data
p.write_text(data.replace('"rc":0', '"rc":9', 1))
PY
if "$BUILD/jef_verify" runtime-receipt "$receipt_pubkey" \
    "$BUILD/receipt-request.json" "$BUILD/tampered-response.json" \
    "$receipt_path" >/dev/null 2>&1; then
    echo "runtime receipt accepted a tampered response" >&2
    exit 1
fi
cp "$receipt_response" "$BUILD/tampered-tail.json"
"$PYTHON_BIN" - "$BUILD/tampered-tail.json" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
data = p.read_text()
assert data.endswith("}\n")
p.write_text(data[:-2] + ',"rc":9}\n')
PY
if "$BUILD/jef_verify" runtime-receipt "$receipt_pubkey" \
    "$BUILD/receipt-request.json" "$BUILD/tampered-tail.json" \
    "$receipt_path" >/dev/null 2>&1; then
    echo "runtime receipt accepted unauthenticated response tail" >&2
    exit 1
fi
cp "$receipt_response" "$BUILD/tampered-ref.json"
"$PYTHON_BIN" - "$BUILD/tampered-ref.json" <<'PY'
from pathlib import Path
import re
import sys
p = Path(sys.argv[1])
data = p.read_text()
match = re.search(r'"receipt_ref":"([0-9a-f])', data)
assert match
replacement = "0" if match.group(1) != "0" else "1"
p.write_text(data[:match.start(1)] + replacement + data[match.end(1):])
PY
if "$BUILD/jef_verify" runtime-receipt "$receipt_pubkey" \
    "$BUILD/receipt-request.json" "$BUILD/tampered-ref.json" \
    "$receipt_path" >/dev/null 2>&1; then
    echo "runtime receipt accepted a foreign receipt_ref" >&2
    exit 1
fi
"$BUILD/jef_verify" runtime-receipt "$receipt_pubkey" \
    "$BUILD/receipt-request.json" "$receipt_response" "$receipt_path" >/dev/null
cp "$BUILD/receipt-request.json" "$BUILD/tampered-request.json"
sed 's/sha256:1111/sha256:2111/' "$BUILD/tampered-request.json" > "$BUILD/tampered-request-2.json"
if "$BUILD/jef_verify" runtime-receipt "$receipt_pubkey" \
    "$BUILD/tampered-request-2.json" "$receipt_response" \
    "$receipt_path" >/dev/null 2>&1; then
    echo "runtime receipt accepted a tampered request" >&2
    exit 1
fi

# The surgery is target-specific: the human shell must still build with its
# interactive and extension surfaces intact.
cmake --build "$BUILD" --target jash >/dev/null
test -x "$BUILD/jash"
nm "$BUILD/jash-dev" | grep -E 'jash_plugin_lookup_builtin|jash_readline' >/dev/null

echo "PASS: jash-agentd agent-strict source profile"
