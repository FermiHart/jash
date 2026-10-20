#!/usr/bin/env bash
# tests/test_json_adapter.sh — ADR-0026 Tier 1: structured CLI adapters.
set -e

JASH="${JASH:-./build/jash}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cd "$(dirname "$0")/.."
JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"

pass=0
fail=0

ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

# 1. git status emits valid JSON.
out="$TMP/git_status.json"
$JASH -c 'json git status' > "$out" 2>/dev/null
if python3 -m json.tool "$out" >/dev/null 2>&1 && grep -q '"branch"' "$out"; then
    ok "json git status emits valid JSON"
else
    err "json git status invalid"
fi

# Git's quoted text path is not the filesystem path. The adapter must preserve
# newline/non-UTF-8 names as tagged path bytes for both status and diff.
if python3 - "$JASH" <<'PY'
import json, os, subprocess, sys, tempfile

jash = sys.argv[1]
with tempfile.TemporaryDirectory(prefix="jash-json-git-bytes-") as tmp:
    root = os.fsencode(tmp)
    env = {k: v for k, v in os.environ.items() if not k.startswith("JASH_")}
    env.update(HOME=tmp, PATH="/usr/bin:/bin", JASH_SIGIL="0",
               JASH_NUDGE="0", JASH_BRIEF="0", GIT_CONFIG_NOSYSTEM="1",
               GIT_CONFIG_GLOBAL="/dev/null")
    def git(*args):
        return subprocess.run([b"/usr/bin/git", *args], cwd=root, env=env,
                              capture_output=True, check=True)
    git(b"init", b"--quiet")
    git(b"config", b"user.name", b"Fixture")
    git(b"config", b"user.email", b"fixture@example.invalid")
    name = b"odd-\xff\nname.txt"
    path = root + b"/" + name
    with open(path, "wb") as file:
        file.write(b"old\n")
    git(b"add", b"--", name)
    git(b"commit", b"--quiet", b"-m", b"fixture")
    with open(path, "wb") as file:
        file.write(b"new\n")

    def adapter(command):
        result = subprocess.run([jash, "-c", command], cwd=root, env=env,
                                capture_output=True, timeout=15)
        assert result.returncode == 0, (result.stdout, result.stderr)
        return json.loads(result.stdout)

    def path_bytes(value):
        if isinstance(value, str):
            return value.encode("utf-8")
        assert value["encoding"] == "base64", value
        import base64
        return base64.b64decode(value["data"], validate=True)

    status = adapter("json git status")
    assert any(path_bytes(row["path"]) == name for row in status["files"]), status
    diff = adapter("json git diff")
    assert any(path_bytes(row["path"]) == name for row in diff["files"]), diff
PY
then
    ok "json git status/diff preserve newline and non-UTF-8 path bytes"
else
    err "json git adapters changed or lost raw pathname bytes"
fi

# 2. git log emits valid JSON.
out="$TMP/git_log.json"
$JASH -c 'json git log 3' > "$out" 2>/dev/null
if python3 -m json.tool "$out" >/dev/null 2>&1 && grep -q '"commits"' "$out"; then
    ok "json git log emits valid JSON"
else
    err "json git log invalid"
fi

# 3. Run a real, private Makefile. Running the project's default target here
# would rebuild the executable being qualified by every later gate.
if python3 - "$JASH" "$TMP" <<'PY'
import json, os, pathlib, subprocess, sys

jash, temporary = sys.argv[1:]
work = pathlib.Path(temporary) / "make-fixture"
work.mkdir()
(work / "Makefile").write_text(
    ".PHONY: all fail\n"
    "all:\n\t@printf 'fixture built\\n' > artifact.txt\n"
    "\t@printf 'fixture.c:7:3: warning: fixture diagnostic\\n' >&2\n"
    "fail:\n\t@printf 'fixture.c:9:2: error: expected failure\\n' >&2\n"
    "\t@exit 2\n")
env = dict(os.environ, JASH_SIGIL="0", JASH_NUDGE="0", JASH_BRIEF="0")
for command, severity, line, column, message in (
        ("json make", "warning", 7, 3, "fixture diagnostic"),
        ("json make fail", "error", 9, 2, "expected failure")):
    response = subprocess.run([jash, "-c", command], cwd=work, env=env,
                              capture_output=True, timeout=15)
    report = json.loads(response.stdout)
    assert report["schema"] == "jash.build-result.v1", report
    assert report["adapter_status"] == 0, report
    assert report["rc"] == report["command_status"] == response.returncode, report
    assert (response.returncode == 0) == (severity == "warning"), report
    assert report["ok"] == (response.returncode == 0), report
    assert report["diagnostics_complete"] is True and report["truncated"] is False, report
    assert report["diagnostics"] == [{
        "severity": severity, "file": "fixture.c", "line": line,
        "column": column, "message": message,
        "raw_line": f"fixture.c:{line}:{column}: {severity}: {message}"}], report
    assert (work / "artifact.txt").read_bytes() == b"fixture built\n"
PY
then
    ok "json make emits valid JSON with real private effects and failure diagnostics"
else
    err "json make lost private effects, command status or diagnostics"
fi

# 4. Unknown command returns non-zero.
if $JASH -c 'json unknown cmd' >/dev/null 2>&1; then
    err "unknown json command should fail"
else
    ok "unknown json command fails"
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  json adapter RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
if [ "$fail" -eq 0 ]; then
    echo "  ✅ json adapter tests PASSED"
    exit 0
else
    echo "  ✗ json adapter tests FAILED"
    exit 1
fi
