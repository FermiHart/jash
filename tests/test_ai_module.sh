#!/usr/bin/env bash
set -u

JASH=${JASH_BIN:-build/jash}
pass=0
fail=0

ok() { printf '  OK  %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf '  FAIL %s: %s\n' "$1" "$2"; fail=$((fail + 1)); }

echo "=== AI module ==="

missing=""
for sym in jash_ai_auto jash_ai_debug jash_ai_learn jash_ai_set_busy jash_eval_line; do
    grep -q "  $sym;" c/plugin_exports.list || missing="$missing elf:$sym"
    grep -q "_$sym" c/plugin_exports_macos.txt || missing="$missing macos:$sym"
done
if [ -z "$missing" ]; then
    ok "host exports the symbols required by ai.modjash"
else
    bad "module ABI" "missing:$missing"
fi

if [ ! -x "$JASH" ]; then
    bad "lazy load" "$JASH is not executable"
elif [ ! -f modules/ai/ai.modjash ]; then
    bad "lazy load" "modules/ai/ai.modjash is missing"
else
    out=$("$JASH" -c '? on' 2>&1)
    if printf '%s' "$out" | grep -q 'ON -- modulo carregado' &&
       ! printf '%s' "$out" | grep -q 'load failed'; then
        ok "? on loads ai.modjash"
    else
        bad "lazy load" "$out"
    fi
fi

if grep -q '#define POOLSIDE_MODEL "poolside/laguna-xs-2.1"' modules/ai/ai_module.c &&
   grep -q '#define POOLSIDE_URL   "https://inference.poolside.ai/v1/chat/completions"' modules/ai/ai_module.c; then
    ok "configured provider endpoint and model are pinned"
else
    bad "provider config" "model or endpoint differs"
fi

fake_home=$(mktemp -d "${TMPDIR:-/tmp}/jash-ai-response.XXXXXX")
mkdir -p "$fake_home/bin"
cat >"$fake_home/bin/curl" <<'PY'
#!/usr/bin/python3
import json
import os

marker = os.environ["AI_TEST_MARKER"]
content = """# Exemplo completo

```c
#include <stdio.h>

int main(void) {
    printf(\"Hello, World!\\n\");
    return 0;
}
```

```jash
printf executado > %s
```
""" % marker
print(json.dumps({"choices": [{"message": {"content": content}}]},
                 separators=(",", ":")))
PY
chmod 0700 "$fake_home/bin/curl"
marker="$fake_home/executed"
out=$(HOME="$fake_home" PATH="$fake_home/bin:/usr/bin:/bin" \
      JASH_SIGIL=0 POOLSIDE_API_KEY=test AI_TEST_MARKER="$marker" \
      "$JASH" -c 'module load ai; ? mostre um hello world completo em C' 2>&1)
if ! printf '%s' "$out" | grep -q '#include <stdio.h>'; then
    bad "code display" "fenced C source was hidden: $out"
elif [ -e "$marker" ]; then
    bad "normal mode execution" "a displayed command ran without ? auto"
elif printf '%s' "$out" | grep -Eq 'Poolside|Laguna|XS 2\.1'; then
    bad "provider privacy" "provider/model leaked in user-visible output"
elif ! printf '%s' "$out" | grep -q 'Pensando'; then
    bad "thinking status" "Pensando... was not shown"
else
    ok "normal mode shows code without execution or provider disclosure"
fi
rm -f "$marker"
auto_out=$(printf '? on auto\n? pedido-unico-xyz\n' | \
    env HOME="$fake_home" PATH="$fake_home/bin:/usr/bin:/bin" \
        JASH_SIGIL=0 POOLSIDE_API_KEY=test AI_TEST_MARKER="$marker" \
        "$JASH" 2>&1)
if [ -e "$marker" ]; then
    ok "? auto remains the explicit command-execution mode"
else
    bad "auto mode execution" "? auto did not execute the jash block: $auto_out"
fi
rm -rf "$fake_home"

if ! command -v python3 >/dev/null 2>&1; then
    bad "? history" "python3 is required for the PTY regression test"
else
    hist_home=$(mktemp -d "${TMPDIR:-/tmp}/jash-ai-history.XXXXXX")
    if python3 - "$JASH" "$hist_home" <<'PY'
import os
import pty
import select
import signal
import sys
import time

jash, home = sys.argv[1:]
pid, fd = pty.fork()
if pid == 0:
    os.environ.clear()
    os.environ.update({
        "HOME": home,
        "PATH": "/usr/bin:/bin",
        "TERM": "xterm-256color",
        "LC_ALL": "C.UTF-8",
        "JASH_SIGIL": "0",
        "JASH_STATUSBAR": "0",
    })
    os.execv(jash, ["jash"])

def read_until(needle, timeout=5.0):
    data = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        data.extend(chunk)
        if needle in data:
            return bytes(data)
    raise RuntimeError(f"timeout waiting for {needle!r}: {bytes(data)!r}")

def read_for(seconds):
    data = bytearray()
    deadline = time.time() + seconds
    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.05)
        if ready:
            try:
                data.extend(os.read(fd, 65536))
            except OSError:
                break
    return bytes(data)

try:
    read_until(b"$", 5.0)
    os.write(fd, b"? on\r")
    read_until(b"ON -- modulo carregado")
    ai_prompt = read_for(0.8)
    if b"\xe2\xa0" in ai_prompt:
        raise RuntimeError(f"AI prompt still animates a braille spinner: {ai_prompt!r}")
    os.write(fd, b"? status\r")
    read_until(b"module=loaded")
    time.sleep(0.2)
    os.write(fd, b"\x1b[A\r")
    recalled = read_until(b"module=loaded")
    if b"active=1" not in recalled:
        raise RuntimeError(f"Up did not recall '? status': {recalled!r}")
    os.write(fd, b"\x04")
    deadline = time.time() + 5.0
    while time.time() < deadline:
        waited, _ = os.waitpid(pid, os.WNOHANG)
        if waited == pid:
            break
        time.sleep(0.05)
    else:
        raise RuntimeError("interactive jash did not exit")
finally:
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    try:
        os.close(fd)
    except OSError:
        pass

history = os.path.join(home, ".jash_history")
with open(history, "r", encoding="utf-8") as f:
    entries = [line.rstrip("\n") for line in f]
if "? status" not in entries:
    raise RuntimeError(f"'? status' was not persisted: {entries!r}")
PY
    then
        ok "? commands participate in Up-arrow history"
    else
        bad "? history" "Up did not recall the previous AI command"
    fi
    rm -rf "$hist_home"
fi

echo "=== AI module: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
