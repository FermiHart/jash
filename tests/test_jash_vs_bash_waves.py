#!/usr/bin/env python3
"""Ten hermetic differential and metamorphic hardening waves."""

from __future__ import annotations

import hashlib
import json
import os
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
JASH = Path(os.environ.get("JASH", ROOT / "build" / "jash")).resolve()
BASH = Path("/usr/bin/bash")


@dataclass
class Result:
    rc: int
    out: bytes
    err: bytes
    timed_out: bool = False

    @property
    def healthy(self) -> bool:
        diagnostics = (
            b"AddressSanitizer", b"LeakSanitizer", b"UndefinedBehaviorSanitizer",
            b"MemorySanitizer", b"ThreadSanitizer", b"runtime error:",
            b"Sanitizer CHECK failed",
        )
        return (
            not self.timed_out
            and self.rc >= 0
            and not any(marker in self.out or marker in self.err for marker in diagnostics)
        )


class Waves:
    def __init__(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="jash-vs-bash-waves-"))
        self.passed = 0
        self.failed = 0
        self.skipped = 0
        self.wave = 0

    def close(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def section(self, number: int, name: str) -> None:
        self.wave = number
        print(f"\n-- Wave {number}: {name} --")

    def check(self, label: str, condition: bool, detail: str = "") -> None:
        if condition:
            self.passed += 1
            print(f"ok    W{self.wave} {label}")
        else:
            self.failed += 1
            suffix = f": {detail}" if detail else ""
            print(f"FAIL  W{self.wave} {label}{suffix}")

    def skip(self, label: str, detail: str) -> None:
        self.skipped += 1
        print(f"SKIP  W{self.wave} {label}: {detail}")

    def dirs(self, label: str) -> tuple[Path, Path, Path, Path]:
        base = self.tmp / f"w{self.wave}-{label.replace(' ', '-')}"
        bw = base / "bash-work"
        jw = base / "jash-work"
        bh = base / "bash-home"
        jh = base / "jash-home"
        for path in (bw, jw, bh, jh):
            path.mkdir(parents=True)
        (bh / ".jashrc").write_bytes(b"")
        (jh / ".jashrc").write_bytes(b"")
        return bw, jw, bh, jh

    @staticmethod
    def env(home: Path, extra: dict[str, str] | None = None) -> dict[str, str]:
        values = {
            "HOME": str(home),
            "TMPDIR": str(home / "tmp"),
            "PATH": "/usr/local/bin:/usr/bin:/bin",
            "LC_ALL": "C",
            "LANG": "C",
            "TERM": "dumb",
            "JASH_SIGIL": "0",
            "JASH_CHANGES": "0",
            "JASH_MEMO": "0",
            "JASH_BRIEF": "0",
            "JASH_NUDGE": "0",
            "JASH_LOOPGUARD": "off",
            "JASH_DRIFT": "0",
            "JASH_RECALL_GUARD": "0",
            "JASH_RISK_CHECKPOINT": "0",
            "JASH_GUARD": "0",
            "JASH_GUARD_STRICT": "0",
            "JASH_GUARD_BYPASS": "0",
            "JASH_GUARD_BYPASS_STRICT": "0",
            "JASH_SKIP_SELFCHECK": "1",
            # Existing process-global leaks are outside this parser battery.
            "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
            "UBSAN_OPTIONS": "halt_on_error=1",
        }
        (home / "tmp").mkdir(exist_ok=True)
        if extra:
            values.update(extra)
        return values

    def run(
        self,
        shell: str,
        code: str,
        cwd: Path,
        home: Path,
        *,
        mode: str = "c",
        args: tuple[str, ...] = (),
        extra: dict[str, str] | None = None,
        timeout: float = 12,
    ) -> Result:
        binary = JASH if shell == "jash" else BASH
        env = self.env(home, extra)
        stdin: bytes | None = None
        if mode in ("c", "lc"):
            argv = [str(binary)]
            if shell == "bash":
                argv += ["--noprofile", "--norc"]
            argv += [f"-{mode}", code, *args]
        elif mode == "script":
            script = cwd / "wave-script.sh"
            script.write_text(code, encoding="utf-8")
            argv = [str(binary), str(script), *args]
        elif mode == "stdin":
            argv = [str(binary)]
            stdin = code.encode()
        else:
            raise ValueError(mode)
        with subprocess.Popen(
            argv,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=cwd,
            env=env,
            umask=0o022,
            start_new_session=True,
        ) as proc:
            try:
                out, err = proc.communicate(stdin, timeout=timeout)
                result = Result(proc.returncode, out, err)
            except subprocess.TimeoutExpired:
                # Descendants can hold capture pipes open after the shell dies.
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                try:
                    out, err = proc.communicate(timeout=2)
                except subprocess.TimeoutExpired as exc:
                    out, err = exc.stdout or b"", exc.stderr or b""
                    proc.stdout.close()
                    proc.stderr.close()
                result = Result(124, out, err, timed_out=True)
        if not result.healthy:
            self.check(f"{shell} {mode} execution health", False, f"code={code[:120]!r} result={result}")
        return result

    @staticmethod
    def manifest(root: Path) -> list[tuple[str, str, int, str]]:
        rows: list[tuple[str, str, int, str]] = []
        for path in sorted(root.rglob("*")):
            rel = str(path.relative_to(root))
            if rel == "wave-script.sh":
                continue
            st = path.lstat()
            mode = stat.S_IMODE(st.st_mode)
            if path.is_symlink():
                rows.append((rel, "link", mode, os.readlink(path)))
            elif path.is_file():
                digest = hashlib.sha256(path.read_bytes()).hexdigest()
                rows.append((rel, "file", mode, digest))
            elif path.is_dir():
                rows.append((rel, "dir", mode, ""))
            elif stat.S_ISFIFO(st.st_mode):
                rows.append((rel, "fifo", mode, ""))
        return rows

    def differential(
        self,
        label: str,
        code: str,
        *,
        seed=None,
        compare_files: bool = False,
        compare_stderr: bool = False,
        mode: str = "c",
        args: tuple[str, ...] = (),
        extra: dict[str, str] | None = None,
        expected: tuple[int, bytes] | None = None,
        timeout: float = 12,
    ) -> tuple[Result, Result, Path, Path]:
        bw, jw, bh, jh = self.dirs(label)
        if seed:
            seed(bw)
            seed(jw)
        br = self.run("bash", code, bw, bh, mode=mode, args=args, extra=extra, timeout=timeout)
        jr = self.run("jash", code, jw, jh, mode=mode, args=args, extra=extra, timeout=timeout)
        same = br.healthy and jr.healthy and br.rc == jr.rc and br.out == jr.out
        if expected is not None:
            same = same and (br.rc, br.out) == expected
        if compare_stderr:
            same = same and br.err == jr.err
        if compare_files:
            same = same and self.manifest(bw) == self.manifest(jw)
        detail = (
            f"bash=(rc={br.rc},out={br.out!r},err={br.err[-160:]!r}) "
            f"jash=(rc={jr.rc},out={jr.out!r},err={jr.err[-160:]!r})"
        )
        if compare_files:
            detail += f" bash_files={self.manifest(bw)} jash_files={self.manifest(jw)}"
        if expected is not None:
            detail += f" expected={expected!r}"
        self.check(label, same, detail)
        return br, jr, bw, jw

    def metamorphic(self, label: str, code: str) -> None:
        base = self.tmp / f"w{self.wave}-{label.replace(' ', '-')}"
        results: list[Result] = []
        manifests = []
        for mode in ("c", "script", "stdin"):
            work = base / mode / "work"
            home = base / mode / "home"
            work.mkdir(parents=True)
            home.mkdir(parents=True)
            (home / ".jashrc").write_bytes(b"")
            results.append(self.run("jash", code, work, home, mode=mode))
            manifests.append(self.manifest(work))
        key = [(r.rc, r.out) for r in results]
        self.check(
            label,
            all(r.healthy for r in results)
            and key[0] == key[1] == key[2]
            and manifests[0] == manifests[1] == manifests[2],
            f"results={key} manifests={manifests}",
        )

    @staticmethod
    def envelope(err: bytes) -> dict | None:
        for line in reversed(err.splitlines()):
            try:
                value = json.loads(line)
            except (json.JSONDecodeError, UnicodeDecodeError):
                continue
            if isinstance(value, dict) and "rc" in value:
                return value
        return None

    def wave1(self) -> None:
        self.section(1, "hermetic oracle")
        self.check(
            "timeouts signals and sanitizer diagnostics cannot be parity",
            not any(r.healthy for r in (
                Result(124, b"", b"", timed_out=True),
                Result(-signal.SIGSEGV, b"", b""),
                Result(0, b"", b"ERROR: AddressSanitizer: heap-use-after-free"),
                Result(0, b"runtime error: signed integer overflow", b""),
            ))
            and Result(124, b"", b"").healthy,
        )
        self.differential("byte-exact stdout", "printf 'A\\nB'")
        self.differential("exit status survives capture", "printf RC; exit 37")
        self.differential(
            "agent variables are absent",
            "printf '%s:%s' \"${CLAUDECODE-unset}\" \"${JASH_SESSION-unset}\"",
        )
        self.metamorphic("entry points agree", "x=7; printf '<%s>\\n' \"$x\"; false")

    def wave2(self) -> None:
        self.section(2, "startup and fast paths")
        bw, jw, bh, jh = self.dirs("routes")
        fast = self.run("jash", "echo FAST", jw, jh, extra={"JASH_BENCH": "1"})
        login = self.run("jash", "echo FAST", jw, jh, mode="lc", extra={"JASH_BENCH": "1"})
        full = self.run("jash", "echo FAST;", jw, jh, extra={"JASH_BENCH": "1"})
        self.check(
            "ultra/login/full output parity",
            all(r.rc == 0 and r.out == b"FAST\n" for r in (fast, login, full)),
            f"fast={fast} login={login} full={full}",
        )
        self.check(
            "route instrumentation is truthful",
            b"state_init=" not in fast.err
            and b"state_init=" in login.err
            and b"state_init=" in full.err,
            f"fast={fast.err!r} login={login.err!r} full={full.err!r}",
        )
        self.differential("unknown command status", "no_such_wave_command", timeout=5)
        self.differential(
            "-c positional arguments",
            "printf '%s|%s|%s\\n' \"$0\" \"$1\" \"$#\"",
            args=("zero", "one"),
        )
        positional_cases = {
            "empty argv zero": ("",),
            "quoted argv zero": ("zero with 'quotes' \" $var `printf BAD` ;|<>",),
            "empty and literal args": (
                "zero", "", "two words", "single'quote", 'double"quote',
                "$var", "`printf BAD`", "$(printf BAD)", "a;|&<>", "back\\slash",
            ),
        }
        for mode in ("c", "lc"):
            for label, args in positional_cases.items():
                expected = "".join(f"<{arg}>\n" for arg in (str(len(args) - 1), *args)).encode()
                self.differential(
                    f"-{mode} {label}",
                    "printf '<%s>\\n' \"$#\" \"$0\" \"$@\"",
                    mode=mode,
                    args=args,
                    expected=(0, expected),
                )
            args = positional_cases["empty and literal args"]
            expected = "".join(f"<{arg}>\n" for arg in (str(len(args) - 1), *args)).encode()
            self.differential(
                f"-{mode} full evaluator args",
                "printf '<%s>\\n' \"$#\" \"$0\" \"$@\";",
                mode=mode,
                args=args,
                expected=(0, expected),
            )
        self.metamorphic("multiline mutation and status", "printf 'one\\n' > artifact\nfalse\n")

    def wave3(self) -> None:
        self.section(3, "lexer quoting and separators")
        cases = {
            "escaped quote shields operators": "printf '<%s>\\n' \"a\\\"|b>c;d\"",
            "comment cannot poison quote state": "x=ok # it's a \" quote and a trailing \\\nprintf '<%s>\\n' \"$x\"",
            "nested separators are data": "printf '<%s>\\n' \"$(printf '%s' 'a;&&||b|>#c')\"",
            "escaped operators are argv": "Z=z; printf '<%s>\\n' \"$Z\" \\| \\> \\& \\;",
            "short circuit continuation": "false &&\nprintf BAD ||\nprintf '%s\\n' good",
        }
        for label, code in cases.items():
            self.differential(label, code)
        self.metamorphic("compound lexical parity", "if true; then printf A; else printf B; fi\nprintf Z")

    def wave4(self) -> None:
        self.section(4, "expansions IFS glob and cmdsub")
        self.differential(
            "parameter operators",
            "v=a.b.a.b; printf '<%s>|<%s>|<%s>\\n' \"${v#*.}\" \"${v%%.*}\" \"${v//a/X}\"",
        )
        self.differential(
            "IFS quoted versus unquoted",
            "P=a::b; IFS=:; for x in literal:pair \"$P\" $P; do printf '<%s>' \"$x\"; done; printf '\\n'",
        )
        self.differential(
            "cmdsub assignment preserves lines",
            "v=$(printf 'a\\nb\\n\\n'); printf '<%s>\\n' \"$v\"",
        )
        self.differential("nested cmdsub", "printf '<%s>\\n' \"$(echo $(echo deep))\"")

        def seed_glob(work: Path) -> None:
            (work / "a one.txt").write_text("x")
            (work / "b.txt").write_text("y")

        self.differential(
            "glob keeps path boundaries",
            "for f in *.txt; do printf '[%s]' \"$f\"; done; printf '\\n'",
            seed=seed_glob,
        )

    def wave5(self) -> None:
        self.section(5, "redirects heredocs and process substitution")
        self.differential(
            "fd ordering",
            "/bin/sh -c 'printf O; printf E >&2' >both 2>&1; cat both",
            compare_files=True,
        )
        self.differential(
            "failed builtin redirect",
            "printf BAD > missing/out",
            compare_files=True,
            expected=(1, b""),
        )
        failures = {
            "input": "< missing/in",
            "output": "> missing/out",
            "append": ">> missing/out",
            "clobber": ">| missing/out",
            "read-write": "<> missing/inout",
            "output dup": ">&9",
            "input dup": "<&9",
        }

        def seed_input(work: Path) -> None:
            (work / "original").write_text("original\n")
            (work / "alternate").write_text("alternate\n")

        for kind, command in (("builtin", "printf BAD"), ("function", "f")):
            prefix = "exec 9>&-; touched=no; f(){ touched=yes; printf BAD; }; "
            for label, redirect in failures.items():
                self.differential(
                    f"{kind} failed {label} does not execute",
                    prefix + f"{command} {redirect}; r=$?; "
                    "printf 'rc=%s touched=%s\\n' \"$r\" \"$touched\"; printf AFTER",
                    compare_files=True,
                    expected=(0, b"rc=1 touched=no\nAFTER"),
                )
            for label, redirect in (("open", "< missing/in"), ("dup", ">&9")):
                self.differential(
                    f"{kind} restores stdout after later {label} failure",
                    prefix + f"{command} > early {redirect}; r=$?; "
                    "printf 'rc=%s touched=%s\\n' \"$r\" \"$touched\"; printf AFTER",
                    compare_files=True,
                    expected=(0, b"rc=1 touched=no\nAFTER"),
                )

            self.differential(
                f"{kind} restores stdin after later open failure",
                prefix + f"exec 0< original; {command} < alternate > missing/out; r=$?; "
                "read line; printf 'rc=%s input=%s touched=%s\\n' \"$r\" \"$line\" \"$touched\"",
                seed=seed_input,
                compare_files=True,
                expected=(0, b"rc=1 input=original touched=no\n"),
            )
            br, jr, bw, jw = self.differential(
                f"{kind} restores stderr after later open failure",
                prefix + f"{command} 2> diagnostic < missing/in; r=$?; "
                "printf 'rc=%s touched=%s\\n' \"$r\" \"$touched\"; printf 'RESTORED\\n' >&2",
                expected=(0, b"rc=1 touched=no\n"),
            )
            diagnostic_paths = [work / "diagnostic" for work in (bw, jw)]
            diagnostics = [path.read_bytes() if path.is_file() else b"" for path in diagnostic_paths]
            self.check(
                f"{kind} stderr returns to capture and records redirect error",
                br.err == jr.err == b"RESTORED\n"
                and all(data and Result(0, b"", data).healthy for data in diagnostics),
                f"bash={br.err!r} jash={jr.err!r} diagnostics={diagnostics!r}",
            )

        for mask in (0o000, 0o077):
            code = f"umask {mask:03o}; f(){{ printf fn; }}; "
            for kind, command in (("builtin", "printf bi"), ("function", "f")):
                code += (
                    f"{command} > {kind}-out; {command} >> {kind}-append; "
                    f"{command} >| {kind}-clobber; {command} <> {kind}-rw; "
                )
            br, jr, bw, jw = self.differential(
                f"umask {mask:03o} redirect creation parity",
                code,
                compare_files=True,
                expected=(0, b"bifn"),
            )
            expected_mode = 0o666 & ~mask
            modes = [stat.S_IMODE(p.stat().st_mode) for work in (bw, jw) for p in work.iterdir()]
            self.check(
                f"umask {mask:03o} creates mode {expected_mode:03o}",
                len(modes) == 16 and all(mode == expected_mode for mode in modes),
                f"modes={[oct(mode) for mode in modes]}",
            )
        self.differential(
            "nested redirect restoration",
            "f(){ printf inner > inner; printf outer; }; f > outer; printf after; cat inner outer",
            compare_files=True,
        )
        heredoc = """V=expanded
cat <<U
$V $(printf sub) $((1+1))
U
cat <<'Q'
$V $(printf sub) $((1+1))
Q
printf AFTER
"""
        self.differential("heredoc expansion boundary", heredoc)
        temporary_input = """V=expanded
cat <<U
$V $(printf sub) `printf tick`
U
cat <<'Q'
$V $(printf sub) `printf tick`
Q
cat <<< 'literal $V `printf BAD` $(printf BAD) | > ;'
cat <<< "$V"
printf AFTER
"""
        expected_input = (
            b"expanded sub tick\n$V $(printf sub) `printf tick`\n"
            b"literal $V `printf BAD` $(printf BAD) | > ;\nexpanded\nAFTER"
        )
        tmp_names = {
            "spaces": "tmp with spaces",
            "quotes": "tmp'and\"quotes",
            "dollar": "tmp$WAVE_TMP_VAR",
            "backticks": "tmp`printf REEXPANDED`",
            "combined": "tmp space'\"$WAVE_TMP_VAR`printf REEXPANDED`",
        }
        for label, name in tmp_names.items():
            for mode in (("c", "script", "stdin") if label == "combined" else ("c",)):
                tmpdir = self.tmp / f"w5-tmpdir-{label}-{mode}" / name
                tmpdir.mkdir(parents=True)
                self.differential(
                    f"heredoc and here-string TMPDIR {label} {mode}",
                    temporary_input,
                    mode=mode,
                    extra={"TMPDIR": str(tmpdir), "WAVE_TMP_VAR": "REEXPANDED"},
                    compare_files=True,
                    expected=(0, expected_input),
                )
                self.check(f"TMPDIR {label} {mode} is cleaned", not list(tmpdir.iterdir()))
        self.differential(
            "input process substitution",
            "X=outer; cat <(X=inner; printf \"$X\"); printf ':%s' \"$X\"",
        )
        # Keep the companion resource probes in this harness: its env -i drops
        # sanitizer options, and its stderr suppression can hide diagnostics.
        bw, jw, bh, jh = self.dirs("procsub-resources")
        resource_cases = {
            "two process substitution consumers": (
                "diff <(printf hi) <(printf hi) >/dev/null && echo same", b"same\n",
            ),
            "process substitution releases fds": (
                "ulimit -n 32; " + "cat <(printf x); " * 40, b"x" * 40,
            ),
            "process substitution retains reassigned fd": (
                'f(){ n=${1##*/}; eval "exec $n>&-"; eval "exec $n>reused-fd.txt"; }; '
                'f <(true); eval "echo retained >&$n"; eval "exec $n>&-"; cat reused-fd.txt',
                b"retained\n",
            ),
        }
        for label, (code, expected_out) in resource_cases.items():
            result = self.run("jash", code, jw, jh)
            self.check(label, result.healthy and result.rc == 0 and result.out == expected_out, f"result={result}")

    def wave6(self) -> None:
        self.section(6, "functions scope and recursion")
        cases = {
            "function argv boundaries": "f(){ printf '<%s|%s|%s>\\n' \"$#\" \"$1\" \"$2\"; }; f 'a b' ''",
            "caller positionals restored": "set -- O1 '' O3; f(){ set -- I; printf 'in:%s:%s\\n' \"$#\" \"$1\"; }; f X; printf 'out:%s:%s:%s:%s\\n' \"$#\" \"$1\" \"$2\" \"$3\"",
            "redirected caller positionals restored": "set -- O1 '' O3; f(){ set -- I; printf 'in:%s:%s\\n' \"$#\" \"$1\"; }; f X > inner; cat inner; printf 'out:%s:%s:%s:%s\\n' \"$#\" \"$1\" \"$2\" \"$3\"",
            "nested local scope": "x=G; inner(){ local x=I; echo i:$x; }; outer(){ local x=O; inner; echo o:$x; }; outer; echo g:$x",
            "redirected local scope": "x=G; f(){ local x=L; :; }; f >/dev/null; echo $x",
            "recursive function": "fac(){ if [ \"$1\" -le 1 ]; then echo 1; else echo $(($1*$(fac $(($1-1))))); fi; }; fac 5",
        }
        for label, code in cases.items():
            self.differential(label, code)

        nested = r"""x=G
inner(){
    local x=I
    printf 'inner=%s x=%s arg=<%s>\n' "$FUNCNAME" "$x" "$1"
    return 7
    printf BAD
}
outer(){
    local x=O
    printf 'before=%s x=%s\n' "$FUNCNAME" "$x"
    inner "$1" > inner
    r=$?
    printf 'after=%s x=%s rc=%s\n' "$FUNCNAME" "$x" "$r"
    return 3
    printf BAD
}
outer 'a b' > outer
r=$?
printf 'global=%s name=<%s> rc=%s\n' "$x" "$FUNCNAME" "$r"
cat inner outer
"""
        for mode in ("c", "script", "stdin"):
            self.differential(
                f"redirected nested multiline scope FUNCNAME and return {mode}",
                nested,
                mode=mode,
                compare_files=True,
                expected=(0, b"global=G name=<> rc=3\ninner=inner x=I arg=<a b>\n"
                          b"before=outer x=O\nafter=outer x=O rc=7\n"),
            )

        literal_args = (
            "", "a b", "$var", "`printf REEXPANDED`", "$(printf REEXPANDED)",
            "a;|&<>#()", "single'quote", 'double"quote', "back\\slash",
        )
        argv_code = (
            "var=EXPANDED; set -- " + shlex.join(literal_args) + "; "
            "f(){ printf '<%s>\\n' \"$#\" \"$@\"; }; "
            "g(){ f \"$@\" > args; }; "
        )
        expected_args = ("".join(f"<{arg}>\n" for arg in (len(literal_args), *literal_args))
                         + "caller=EXPANDED\n").encode()
        for label, call in (("direct", 'f "$@" > args'), ("forwarded", 'g "$@" > outer')):
            self.differential(
                f"redirected function {label} argv expands only once",
                argv_code + call + "; cat args; printf 'caller=%s\\n' \"$var\"",
                compare_files=True,
                expected=(0, expected_args),
            )

    def wave7(self) -> None:
        self.section(7, "control nesting status and traps")
        cases = {
            "nested if and case": "x=b; if true; then if false; then echo BAD; else case \"$x\" in a) echo A;; b) echo B;; esac; fi; fi",
            "break two levels": "for i in 1 2; do for j in a b; do echo $i$j; break 2; done; echo BAD; done; echo end",
            "status 255 is data": "(exit 255); printf 's=%s\\n' \"$?\"",
            "list status survives": "false && echo BAD || { echo recover; false; }; s=$?; echo s=$s; exit $s",
            "DEBUG trap": "trap 'echo DBG' DEBUG\necho fn\necho aft",
        }
        for label, code in cases.items():
            self.differential(label, code)

        exit_cases = {
            "explicit status": ("printf 'trap=%s\\n' \"$?\"", "exit 7", 7, b"run\ntrap=7\n"),
            "implicit status": ("printf 'trap=%s\\n' \"$?\"", "/bin/sh -c 'exit 7'", 7, b"run\ntrap=7\n"),
            "multiple commands preserve status": (
                "printf 'trap=%s\\n' \"$?\"; printf 'tail\\n'; false", "exit 7", 7, b"run\ntrap=7\ntail\n",
            ),
            "explicit exit overrides status": (
                "printf 'trap=%s\\n' \"$?\"; printf 'tail\\n'; exit 23", "exit 7", 23, b"run\ntrap=7\ntail\n",
            ),
        }
        for label, (trap, ending, rc, out) in exit_cases.items():
            for mode in ("c", "script", "stdin"):
                self.differential(
                    f"EXIT trap {label} {mode}",
                    f"trap {shlex.quote(trap)} EXIT\nprintf 'run\\n'\n{ending}\n",
                    mode=mode,
                    expected=(rc, out),
                )

    def wave8(self) -> None:
        self.section(8, "pipelines jobs signals and resources")
        self.differential(
            "last pipeline status",
            "printf x | /bin/sh -c 'cat >/dev/null; exit 23'; r=$?; printf '%s\\n' \"$r\"",
        )
        self.differential(
            "binary pipeline",
            "python3 -c 'import os; os.write(1,b\"A\\0B\")' | python3 -c 'import os; os.write(1,os.read(0,9))'",
        )
        self.differential(
            "background wait status",
            "/bin/sh -c 'exit 7' & p=$!; wait $p; r=$?; printf '%s\\n' \"$r\"",
        )
        self.differential(
            "signaled child status",
            "python3 -c 'import os,signal; os.kill(os.getpid(),signal.SIGUSR1)'; printf '%s\\n' \"$?\"",
        )
        low_fd = "old=$(ulimit -n); (ulimit -n 32; i=0; while [ $i -lt 40 ]; do printf x | cat >/dev/null || exit 91; i=$((i+1)); done); r=$?; printf '%s:%s\\n' \"$r\" \"$(ulimit -n)\""
        self.differential("pipeline descriptors reclaimed", low_fd, timeout=20)

    def wave9(self) -> None:
        self.section(9, "agent sessions and filesystem observation")

        def seed(work: Path) -> None:
            (work / "modify.txt").write_text("old\n")
            (work / "delete.txt").write_text("gone\n")

        self.differential(
            "mutators preserve disk effect",
            "printf 'new\\n' > create.txt; printf 'changed\\n' > modify.txt; rm delete.txt",
            seed=seed,
            compare_files=True,
        )

        bw, jw, bh, jh = self.dirs("envelope")
        jr = self.run(
            "jash",
            "printf x > made.txt",
            jw,
            jh,
            extra={"JASH_AGENT": "1", "JASH_CHANGES": "1"},
        )
        envelope = self.envelope(jr.err)
        fc = envelope.get("files_changed", {}) if envelope else {}
        self.check(
            "agent envelope reports exact creation",
            jr.rc == 0
            and (jw / "made.txt").read_bytes() == b"x"
            and fc.get("created") == ["made.txt"]
            and fc.get("total") == 1,
            f"rc={jr.rc} err={jr.err!r}",
        )

        base = self.tmp / "w9-session"
        work, home = base / "work", base / "home"
        (work / "sub").mkdir(parents=True)
        home.mkdir(parents=True)
        (home / ".jashrc").write_bytes(b"")
        extra = {"JASH_SESSION": "wave9", "JASH_CHANGES": "0"}
        first = self.run("jash", "export W9='a b'; cd sub", work, home, extra=extra)
        second = self.run("jash", "printf '%s' \"$W9\" > value.txt", work, home, extra=extra)
        self.check(
            "session restores env and cwd before redirect",
            first.rc == 0
            and second.rc == 0
            and (work / "sub" / "value.txt").read_bytes() == b"a b"
            and not (work / "value.txt").exists(),
            f"first={first} second={second}",
        )

        target = work / "observed.txt"
        target.write_text("old")
        outside = base / "outside-link"
        outside.hardlink_to(target)
        code = f"printf longer > {outside}"
        observed = self.run(
            "jash",
            code,
            work,
            home,
            extra={"JASH_AGENT": "1", "JASH_CHANGES": "1"},
        )
        env = self.envelope(observed.err)
        changed = env.get("files_changed", {}).get("modified", []) if env else []
        self.check("external hardlink mutation is observed", "observed.txt" in changed, f"err={observed.err!r}")

    def wave10(self) -> None:
        self.section(10, "deterministic adversarial corpus and limits")
        corpus = [
            ":",
            "true; false; printf '%s' $?",
            "false && echo BAD || echo ok",
            "x='a b'; printf '<%s>' \"$x\"",
            "printf '%s' $((2+3*4))",
            "printf '<%s>' \"$(printf x)\"",
            "case abc in a*) echo yes;; *) echo no;; esac",
            "for i in 1 2 3; do printf '%s' $i; done",
            "i=0; while [ $i -lt 3 ]; do i=$((i+1)); done; echo $i",
            "f(){ local x=y; printf '%s' \"$x\"; }; f",
            "{ printf a; printf b; } | cat",
            "set -- 'a b' ''; printf '%s:%s:%s' \"$#\" \"$1\" \"$2\"",
            "v=abcabc; printf '%s' \"${v//a/X}\"",
            "printf '%s' 'literal | > ; && $()'",
            "echo \"$(echo \"$(echo deep)\")\"",
            "f(){ local x=L; printf '<%s>\\n' \"$x\" \"$@\"; return 7; }; "
            "f '' '$var' '`printf BAD`' '|><;&' > args; r=$?; cat args; exit $r",
            "trap 'printf \"trap=%s\\n\" \"$?\"; false' EXIT; exit 7",
            "cat <<'EOF'\n$var `printf BAD` | > ;\nEOF\ncat <<< 'literal $var `printf BAD`'\n",
        ]
        for index, code in enumerate(corpus, 1):
            self.differential(f"corpus seed {index:02d}", code, compare_files=True, compare_stderr=True)

        bw, jw, bh, jh = self.dirs("recursion-limit")
        recursive = self.run("jash", "r(){ r; }; r", jw, jh, timeout=8)
        self.check(
            "recursion limit refuses without crash",
            recursive.rc == 1 and b"maximum function recursion depth" in recursive.err,
            f"result={recursive}",
        )

        asan = os.environ.get("JASH_WAVES_ASAN")
        if asan and Path(asan).is_file():
            saved = globals()["JASH"]
            globals()["JASH"] = Path(asan).resolve()
            try:
                for index, code in enumerate(corpus, 1):
                    self.differential(
                        f"sanitized corpus seed {index:02d}", code,
                        compare_files=True, compare_stderr=True,
                    )
            finally:
                globals()["JASH"] = saved
        elif asan:
            self.check("sanitized corpus binary exists", False, asan)
        else:
            self.skip("sanitized corpus", "set JASH_WAVES_ASAN to enable")


def main() -> int:
    if not JASH.is_file():
        print(f"missing jash binary: {JASH}", file=sys.stderr)
        return 2
    waves = Waves()
    try:
        selected = [int(arg) for arg in sys.argv[1:]] if len(sys.argv) > 1 else list(range(1, 11))
        if any(number < 1 or number > 10 for number in selected):
            print("wave numbers must be between 1 and 10", file=sys.stderr)
            return 2
        for number in selected:
            getattr(waves, f"wave{number}")()
        print(
            f"\n=== 10 waves: {waves.passed} ok / {waves.failed} fail / "
            f"{waves.skipped} skip ==="
        )
        return 0 if waves.failed == 0 else 1
    finally:
        waves.close()


if __name__ == "__main__":
    raise SystemExit(main())
