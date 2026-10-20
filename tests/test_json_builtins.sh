#!/usr/bin/env bash
# test_json_builtins.sh — per-command structured output (ROADMAP Wave 7c).
# Every structured builtin gains an isolated --json path; the text path is
# unchanged. Verifies JSON shape + that plain output still works.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }
j(){ "$J" -c "$1" 2>/dev/null; }

TD="$(mktemp -d)"; trap 'rm -rf "$TD"' EXIT
printf x > "$TD/a"; printf xxxx > "$TD/b"; mkdir "$TD/d"
mkdir -p "$TD/globs/sub"; printf 'a\n' > "$TD/globs/a.c"
printf 'h\n' > "$TD/globs/b.h"; printf 'c\n' > "$TD/globs/sub/c.c"

echo "== per-command --json (Wave 7c) =="

o=$(j 'env --json');        case "$o" in '{'*'}'*) ok "env --json is an object";;   *) no env "$o";; esac
o=$(j 'pwd --json');        case "$o" in *'"cwd":"'*) ok "pwd --json has cwd";;      *) no pwd "$o";; esac
o=$(j 'echo --json hello world'); case "$o" in *'"hello","world"'*) ok "echo --json is array";; *) no echo "$o";; esac
o=$(j 'echo --json "" empty="test"'); case "$o" in *'""'*) ok "echo --json escapes empty arg";; *) no echo-escape "$o";; esac
o=$(j 'grep --json "root" /etc/passwd 2>/dev/null' | head -1); case "$o" in *'"span":{'*) ok "grep --json has span";; *) no grep "$o";; esac
o=$(j 'df --json' | head -1); case "$o" in *'"mountpoint"'*'"total_bytes":'*) ok "df --json has fs records";; *) no df "$o";; esac
o=$(j "du --json $TD" | head -1); case "$o" in *'"name":"'*'"size_kb":'*) ok "du --json has size records";; *) no du "$o";; esac
o=$(j 'ps --json')
if printf '%s\n' "$o" | python3 -c '
import json,sys
rows=[json.loads(line) for line in sys.stdin if line.strip()]
assert rows
assert all(type(row["pid"]) is int and row["pid"] > 0 and isinstance(row["comm"], str) for row in rows)
'; then ok "ps --json has process records"
else no ps "$o"; fi
o=$(j "ls --json $TD" | head -1); case "$o" in *'"name":"'*'"type":"'*) ok "ls --json has entry records";; *) no ls "$o";; esac

# LLM file reads need byte-exact pages, not the legacy cat --json size summary.
printf 'alpha\n\316\262eta\n' > "$TD/read.txt"
o=$(j "cat --data --offset 0 --limit 5 $TD/read.txt")
if printf '%s\n' "$o" | python3 -c '
import base64,json,sys
d=json.load(sys.stdin)
assert d["schema"] == "jash.file-page.v1"
assert d["path"].endswith("/read.txt")
assert (d["offset"],d["bytes_returned"],d["total_bytes"],d["eof"]) == (0,5,12,False)
assert d["text"] == "alpha" and base64.b64decode(d["data"]["data"]) == b"alpha"
assert len(d["page_blake3"]) == 64
'; then ok "cat --data returns a bounded typed byte page"
else no "cat --data page" "$o"; fi
o=$(j "cat --data --offset 6 --limit 6 $TD/read.txt")
if printf '%s\n' "$o" | python3 -c '
import base64,json,sys
d=json.load(sys.stdin)
assert d["text"] == "βeta\n" and d["eof"] is True
assert base64.b64decode(d["data"]["data"]) == "βeta\n".encode()
'; then ok "cat --data pages UTF-8 on byte offsets"
else no "cat --data UTF-8 page" "$o"; fi
printf '\000\377z' > "$TD/binary.bin"
o=$(j "cat --data $TD/binary.bin")
if printf '%s\n' "$o" | python3 -c '
import base64,json,sys
d=json.load(sys.stdin)
assert d["schema"] == "jash.file-page.v1" and d["text"] is None
assert base64.b64decode(d["data"]["data"]) == b"\0\xffz"
assert d["bytes_returned"] == 3 and d["eof"] is True
'; then ok "cat --data preserves binary bytes with tagged base64"
else no "cat --data binary page" "$o"; fi
o=$(j "cat --data --offset 13 $TD/read.txt"); rc=$?
[[ $rc -eq 2 && -z "$o" ]] && ok "cat --data refuses offsets beyond EOF without partial JSON" \
    || no "cat --data invalid offset" "rc=$rc out='$o'"
o=$(j "cat --json $TD/read.txt")
if printf '%s\n' "$o" | python3 -c '
import json,sys
d=json.load(sys.stdin)
assert len(d) == 1 and d[0]["size_bytes"] == 12 and d[0]["lines"] == 2
assert d[0]["path"].endswith("/read.txt")
'; then ok "cat --json metadata remains valid and escaped"
else no "cat --json metadata" "$o"; fi
o=$(j "cat --json $TD/missing.txt"); rc=$?
if [[ $rc -ne 0 ]] && printf '%s\n' "$o" | python3 -c '
import json,sys
d=json.load(sys.stdin)
assert len(d) == 1 and d[0]["error"] == "read_failed"
'; then ok "cat --json reports read failures as a valid error row"
else no "cat --json read error" "rc=$rc out='$o'"; fi
o=$(j "glob --json '$TD/globs/**/*.c'")
if printf '%s\n' "$o" | python3 -c '
import json,sys
d=json.load(sys.stdin)
assert d["schema"] == "jash.glob.v1" and d["total_matches"] == 2
assert d["matches"][0]["path"].endswith("/a.c")
assert d["matches"][1]["path"].endswith("/sub/c.c")
assert all(item["type"] == "file" and item["size_bytes"] == 2 for item in d["matches"])
'; then ok "glob --json returns deterministic typed recursive matches"
else no "glob recursive JSON" "$o"; fi
o=$(j "glob --json --offset 1 --limit 1 '$TD/globs/**/*.c'")
if printf '%s\n' "$o" | python3 -c '
import json,sys
d=json.load(sys.stdin)
assert d["total_matches"] == 2 and d["returned"] == 1
assert d["offset"] == 1 and d["next_offset"] is None and d["complete"] is True
assert d["matches"][0]["path"].endswith("/sub/c.c")
'; then ok "glob --json pages the match sequence"
else no "glob pagination" "$o"; fi
o=$(j "glob --json '$TD/globs/*.no-match'")
if printf '%s\n' "$o" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["matches"] == [] and d["complete"] and d["total_matches"] == 0'; then
    ok "glob --json returns a complete empty result"
else no "glob empty result" "$o"; fi
o=$(j "find --json --limit 2 $TD/globs -type f")
if printf '%s\n' "$o" | python3 -c '
import json,sys
d=json.load(sys.stdin)
assert d["schema"] == "jash.query.find.v1"
assert d["total_matches"] == 3 and d["count"] == 2 and d["next_offset"] == 2
assert [item["path"].rsplit("/",1)[-1] for item in d["items"]] == ["a.c","b.h"]
assert all(item["type"] == "file" for item in d["items"])
'; then ok "find --json returns typed bounded entries"
else no "find JSON page" "$o"; fi
o=$(j "find --json --offset 2 --limit 2 $TD/globs -type f")
if printf '%s\n' "$o" | python3 -c '
import json,sys
d=json.load(sys.stdin)
assert d["offset"] == 2 and d["count"] == 1 and d["complete"] is True
assert d["items"][0]["path"].endswith("/sub/c.c")
'; then ok "find --json pages deterministic results"
else no "find JSON pagination" "$o"; fi
find_marker="$TD/find-exec-must-not-run"
o=$("$J" -c "find --json $TD/globs -exec touch $find_marker" 2>/dev/null); rc=$?
if [[ $rc -eq 2 && ! -e "$find_marker" ]] && printf '%s\n' "$o" | python3 -c '
import json,sys
d=json.load(sys.stdin)
assert d["schema"] == "jash.builtin.error.v1" and d["command"] == "find"
assert d["complete"] is False and d["error"]["code"] == "unsupported_predicate"
'; then ok "find --json rejects external predicates before execution"
else no "find JSON external predicate" "rc=$rc out='$o' marker=$([[ -e "$find_marker" ]] && echo yes || echo no)"; fi

echo "== text paths intact =="
[ "$(j 'pwd')" = "$ROOT" ] || [ -n "$(j 'pwd')" ] && ok "pwd text" || no pwdtext ""
case "$(j 'df' | head -1)" in *Filesystem*) ok "df text header";; *) no dftext "";; esac
case "$(j "ls $TD")" in *a*) ok "ls text";; *) no lstext "";; esac

echo "== json-builtins: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
