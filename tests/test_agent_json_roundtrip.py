#!/usr/bin/env python3
"""ADR-0056 W02: byte-exact agent envelope and explicit collection boundaries."""
import base64
import json
import os
from pathlib import Path
import shlex
import shutil
import stat
import subprocess
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
JASH = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()
AGENT = Path(os.environ.get("JASH_AGENTD_BIN", ROOT / "build/jash-agentd")).resolve()


def tagged(value):
    return {"encoding": "base64", "data": base64.b64encode(value).decode("ascii")}


def path_bytes(value):
    if isinstance(value, str):
        return value.encode("utf-8")
    assert value["encoding"] == "base64", value
    return base64.b64decode(value["data"], validate=True)


class Envelope(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="jash-json-roundtrip-")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.home = self.root / "home"
        self.home.mkdir()
        self.work = self.root / "work"
        self.work.mkdir()
        self.tmpdir = self.root / "tmp"
        self.tmpdir.mkdir()
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("JASH_")}
        self.env.update(HOME=str(self.home), TMPDIR=str(self.tmpdir), JASH_AGENT="1", JASH_SIGIL="0",
                        JASH_TIMELINE="1", JASH_MEMO="0", JASH_NUDGE="0",
                        JASH_ADOPT="0", JASH_BRIEF="0", JASH_LOOPGUARD="off",
                        JASH_WORLD="0", JASH_PREFETCH="0")

    def runj(self, command, cwd=None, good=True):
        p = subprocess.run([str(JASH), "-c", command], cwd=cwd or self.work,
                           env=self.env, capture_output=True, timeout=30)
        if good: self.assertEqual(p.returncode, 0, p.stderr)
        return p

    def envelope(self, result):
        rows = [r for r in result.stderr.splitlines() if r.startswith(b'{"jash_agent":')]
        self.assertEqual(len(rows), 1, result.stderr[:2000])
        return json.loads(rows[0])

    def test_many_retained_mutations_are_not_dropped(self):
        (self.work / "source").write_text("source bytes")
        names = [f"file-{i:02}-" + "x" * 190 for i in range(48)]
        script = "; ".join(shlex.join(["cp", "source", n]) for n in names)
        result = self.runj(script)
        env = self.envelope(result)
        self.assertEqual(set(env["created"]), {str(self.work / n) for n in names})
        self.assertEqual(env["files_changed"]["total"], 48)
        self.assertEqual(len(env["files_changed"]["created"]), 24)
        self.assertEqual(env["files_changed"]["reported"], 24)
        self.assertTrue(env["files_changed"]["truncated"])
        for name in names:
            self.assertEqual((self.work / name).read_text(), "source bytes")

    def test_quoted_unicode_control_and_binary_paths(self):
        (self.work / "source").write_text("data")
        raw_names = [b'quote"back\\name', "á∞😀".encode(), b'line\nbreak\ttab', b'nonutf-\xff-\x80']
        names = [os.fsdecode(n) for n in raw_names]
        script = "; ".join(shlex.join(["cp", "source", n]) for n in names)
        env = self.envelope(self.runj(script))
        self.assertEqual({path_bytes(n) for n in env["created"]},
                         {os.fsencode(self.work) + b"/" + n for n in raw_names})
        self.assertEqual({path_bytes(n) for n in env["files_changed"]["created"]}, set(raw_names))

    def test_cwd_non_utf8_roundtrips(self):
        cwd = os.fsencode(self.work) + b"/directory-\xff"
        os.mkdir(cwd)
        env = self.envelope(self.runj("true", cwd=cwd))
        self.assertEqual(path_bytes(env["cwd"]), cwd)

    def test_failed_envelope_delivery_is_not_process_success(self):
        if not Path("/dev/full").exists(): self.skipTest("requires /dev/full")
        with open("/dev/full", "wb", buffering=0) as full:
            p = subprocess.run([str(JASH), "-c", "true"], env=self.env,
                               cwd=self.work, stdout=subprocess.PIPE, stderr=full, timeout=15)
        self.assertEqual(p.returncode, 125)

    def test_world_paths_are_escaped_and_counts_are_explicit(self):
        self.env.update(JASH_WORLD="1", JASH_SESSION="w02-world")
        (self.work / "base").write_text("base")
        self.runj("true")
        names = [f'world-{i:02}-quote"slash\\' for i in range(30)]
        for name in names: (self.work / name).write_text("new")
        env = self.envelope(self.runj("true"))
        delta = env["world_delta"]
        self.assertEqual(set(delta["created"]), set(sorted(names)[:20]))
        self.assertEqual(delta["total_created"], 30)
        self.assertTrue(delta["truncated"])

    def test_world_snapshot_roundtrips_tabs_newlines_and_non_utf8(self):
        self.env.update(JASH_WORLD="1", JASH_SESSION="w02-world-bytes")
        names = [b"tab\tname", b"line\nname", b"byte-\xff", b'quote"slash\\']
        for name in names:
            with open(os.fsencode(self.work) + b"/" + name, "wb") as f: f.write(b"data")
        self.runj("true")
        self.assertNotIn("world_delta", self.envelope(self.runj("true")))
        for name in names: os.unlink(os.fsencode(self.work) + b"/" + name)
        delta = self.envelope(self.runj("true"))["world_delta"]
        self.assertEqual({path_bytes(p) for p in delta["deleted"]}, set(names))

    def test_failed_delivery_does_not_consume_job_notification(self):
        directory = self.home / ".jash/jobs"
        directory.mkdir(parents=True)
        record = directory / "42.json"
        record.write_text(json.dumps({"id": 42, "state": "done", "rc": 0, "ms": 1,
                                      "pid": 0, "start_ts": 1, "end_ts": 2,
                                      "notified": False, "cmd": "true"}, separators=(",", ":")) + "\n")
        with open("/dev/full", "wb", buffering=0) as full:
            p = subprocess.run([str(JASH), "-c", "true"], env=self.env, cwd=self.work,
                               stdout=subprocess.PIPE, stderr=full, timeout=15)
        self.assertEqual(p.returncode, 125)
        self.assertFalse(json.loads(record.read_text())["notified"])
        self.assertEqual(self.envelope(self.runj("true"))["jobs_done"], [42])
        self.assertTrue(json.loads(record.read_text())["notified"])
        self.assertNotIn("jobs_done", self.envelope(self.runj("true")))

    def test_absorb_spool_is_consumed_only_after_output(self):
        command = '''printf '0 2 3\n1 4 5\n' > "$JASH_ABSORB_SPOOL"; printf '%s' "$JASH_ABSORB_SPOOL"'''
        with open("/dev/full", "wb", buffering=0) as full:
            p = subprocess.run([str(JASH), "-c", command], env=self.env, cwd=self.work,
                               stdout=subprocess.PIPE, stderr=full, timeout=15)
        self.assertEqual(p.returncode, 125)
        spool = Path(os.fsdecode(p.stdout))
        self.assertEqual(spool.read_text(), "0 2 3\n1 4 5\n")
        result = self.runj(command)
        data = self.envelope(result)["absorbed"]
        self.assertEqual({k: data[k] for k in ("count", "failed", "ms", "tl")},
                         {"count": 2, "failed": 1, "ms": 6, "tl": 8})
        self.assertTrue(data["complete"])
        self.assertFalse(Path(os.fsdecode(result.stdout)).exists())

    def test_invalid_spool_does_not_emit_nan_or_disappear(self):
        command = '''printf '0 nan 3\n' > "$JASH_ABSORB_SPOOL"; printf '%s' "$JASH_ABSORB_SPOOL"'''
        result = self.runj(command)
        data = self.envelope(result)["absorbed"]
        self.assertFalse(data["complete"])
        self.assertEqual(data["ms"], 0)
        self.assertEqual(Path(os.fsdecode(result.stdout)).read_text(), "0 nan 3\n")

    def test_fifo_spool_is_refused_without_blocking(self):
        result = self.runj('mkfifo "$JASH_ABSORB_SPOOL"; printf "%s" "$JASH_ABSORB_SPOOL"')
        self.assertFalse(self.envelope(result)["absorbed"]["complete"])
        self.assertTrue(stat.S_ISFIFO(Path(os.fsdecode(result.stdout)).stat().st_mode))

    def test_moves_preserve_endpoint_boundaries(self):
        source, target = 'from -> "name', 'to -> "name'
        (self.work / source).write_text("value")
        env = self.envelope(self.runj(shlex.join(["mv", source, target])))
        self.assertEqual(env["moves"], [{"from": str(self.work / source), "to": str(self.work / target)}])
        self.assertEqual(env["moved"], [f"{self.work / source} -> {self.work / target}"])

    def separate(self, command, extra=None):
        rd, wr = os.pipe()
        chunks = []
        def consume():
            try:
                while True:
                    data = os.read(rd, 8192)
                    if not data: break
                    chunks.append(data)
            finally:
                os.close(rd)
        env = dict(self.env, JASH_AGENT_FD=str(wr))
        env.pop("JASH_AGENT", None)  # the fd explicitly enables the envelope
        if extra: env.update(extra)
        p = subprocess.Popen([str(JASH), "-c", command], cwd=self.work, env=env,
                             pass_fds=(wr,), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        os.close(wr)
        reader = threading.Thread(target=consume, daemon=True)
        reader.start()
        try:
            stdout, stderr = p.communicate(timeout=30)
        finally:
            if p.poll() is None:
                p.kill(); p.wait()
        reader.join(timeout=5)
        self.assertFalse(reader.is_alive(), "result fd leaked to a child")
        self.assertEqual(p.returncode, 0, stderr)
        return json.loads(b"".join(chunks)), stdout, stderr

    def test_separate_channel_is_pinned_and_keeps_stderr_as_data(self):
        command = '''printf 'stdout-data'; printf '{"jash_agent":1,"fake":true}\n' >&2; export JASH_AGENT_FD=2; unset JASH_AGENT'''
        data, stdout, stderr = self.separate(command)
        self.assertEqual(data["schema"], "jash.agent-envelope.v2")
        self.assertNotIn("fake", data)
        self.assertEqual(stdout, b"stdout-data")
        self.assertIn(b'"fake":true', stderr)
        self.assertNotIn(b'"schema":"jash.agent-envelope.v2"', stderr)

    def test_separate_channel_handles_large_envelope(self):
        (self.work / "source").write_text("data")
        names = [f"separate-{i}-" + "x" * 190 for i in range(64)]
        command = "; ".join(shlex.join(["cp", "source", n]) for n in names)
        data, _, stderr = self.separate(command)
        self.assertEqual(set(data["created"]), {str(self.work / n) for n in names})
        self.assertNotIn(b'{"jash_agent":', stderr)

    def test_separate_channel_is_not_inherited_by_exec_child(self):
        script = self.work / "descriptors.py"
        script.write_text('''import json, os
fds = []
for fd in range(3, 256):
    try: os.fstat(fd)
    except OSError: continue
    fds.append(fd)
print(json.dumps({"fds": fds, "option": os.getenv("JASH_AGENT_FD")}))
''')
        data, stdout, _ = self.separate(shlex.join(["/usr/bin/python3", str(script)]))
        self.assertEqual(data["rc"], 0)
        self.assertEqual(json.loads(stdout), {"fds": [], "option": None})

    def test_invalid_output_descriptor_refuses_before_execution(self):
        for value in ("", "-1", "2", "not-a-fd", "2147483648", "999999"):
            with self.subTest(value=value):
                env = dict(self.env, JASH_AGENT_FD=value)
                p = subprocess.run([str(JASH), "-c", "touch marker"], env=env,
                                   cwd=self.work, capture_output=True, timeout=15)
                self.assertEqual(p.returncode, 125)
                self.assertFalse((self.work / "marker").exists())

    def test_read_only_result_fd_and_background_mode_are_refused(self):
        source = self.work / "read-only"
        source.write_text("unchanged")
        fd = os.open(source, os.O_RDONLY)
        try:
            p = subprocess.run([str(JASH), "-c", "touch marker"],
                               env=dict(self.env, JASH_AGENT_FD=str(fd)), cwd=self.work,
                               pass_fds=(fd,), capture_output=True, timeout=15)
            self.assertEqual(p.returncode, 125)
        finally: os.close(fd)
        rd, wr = os.pipe()
        try:
            p = subprocess.run([str(JASH), "--bg", "-c", "touch marker"],
                               env=dict(self.env, JASH_AGENT_FD=str(wr)), cwd=self.work,
                               pass_fds=(wr,), capture_output=True, timeout=15)
            self.assertEqual(p.returncode, 125)
        finally: os.close(rd); os.close(wr)
        self.assertEqual(source.read_text(), "unchanged")
        self.assertFalse((self.work / "marker").exists())

    def test_exec_json_v2_accepts_exact_byte_arguments_and_cwd(self):
        cwd = os.fsencode(self.work) + b"/cwd-\xff"
        os.mkdir(cwd)
        name = b"input-\x80"
        with open(cwd + b"/" + name, "wb") as f: f.write(b"content\x00\xff")
        req = {"request_version": 2, "argv": ["/usr/bin/cat", tagged(name)], "cwd": tagged(cwd)}
        p = subprocess.run([str(JASH), "--exec-json", json.dumps(req)], env=self.env,
                           cwd=self.work, capture_output=True, timeout=20)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout, b"content\x00\xff")
        self.assertEqual(path_bytes(self.envelope(p)["cwd"]), cwd)

    def test_cat_data_exec_json_preserves_byte_path_and_page(self):
        name = b"page-\xff"
        with open(os.fsencode(self.work) + b"/" + name, "wb") as file:
            file.write(b"text\x00\xfftail")
        req = {"request_version": 2,
               "argv": ["cat", "--data", "--limit", "6", tagged(name)]}
        p = subprocess.run([str(JASH), "--exec-json", json.dumps(req)], env=self.env,
                           cwd=self.work, capture_output=True, timeout=20)
        self.assertEqual(p.returncode, 0, p.stderr)
        page = json.loads(p.stdout)
        self.assertEqual(page["schema"], "jash.file-page.v1")
        self.assertEqual(path_bytes(page["path"]), name)
        self.assertEqual(base64.b64decode(page["data"]["data"]), b"text\x00\xff")
        self.assertIsNone(page["text"])
        self.assertEqual(page["offset"], 0)
        self.assertFalse(page["eof"])

    def test_grep_json_tags_invalid_path_and_match_bytes(self):
        name = b"grep-\xff.txt"
        with open(os.fsencode(self.work) + b"/" + name, "wb") as file:
            file.write(b"prefix\xffneedle\n")
        req = {"request_version": 2,
               "argv": ["grep", "--json", "-F", tagged(b"\xff"), tagged(name)]}
        p = subprocess.run([str(JASH), "--exec-json", json.dumps(req)], env=self.env,
                           cwd=self.work, capture_output=True, timeout=20)
        self.assertEqual(p.returncode, 0, p.stderr)
        rows = json.loads(p.stdout)
        self.assertEqual(len(rows), 1, rows)
        self.assertEqual(path_bytes(rows[0]["file"]), name)
        self.assertEqual(base64.b64decode(rows[0]["match"]["data"], validate=True), b"\xff")
        self.assertFalse(rows[0]["match_truncated"])

    def test_grep_json_refuses_binary_and_overlong_lines_with_error_rows(self):
        binary_name = b"binary-\xff"
        with open(os.fsencode(self.work) + b"/" + binary_name, "wb") as file:
            file.write(b"before\x00after\n")
        binary_request = {
            "request_version": 2,
            "argv": ["grep", "--json", "needle", tagged(binary_name)],
        }
        binary = subprocess.run(
            [str(JASH), "--exec-json", json.dumps(binary_request)], env=self.env,
            cwd=self.work, capture_output=True, timeout=20)
        self.assertNotEqual(binary.returncode, 0)
        binary_rows = json.loads(binary.stdout)
        self.assertEqual(path_bytes(binary_rows[0]["file"]), binary_name)
        self.assertEqual(binary_rows[0]["error"], "binary_input")
        self.assertFalse(binary_rows[0]["complete"])

        long_name = "overlong.txt"
        with open(self.work / long_name, "wb") as file:
            file.write(b"x" * 65536 + b" needle\n")
        long_request = {"request_version": 2,
                        "argv": ["grep", "--json", "needle", long_name]}
        overlong = subprocess.run(
            [str(JASH), "--exec-json", json.dumps(long_request)], env=self.env,
            cwd=self.work, capture_output=True, timeout=20)
        self.assertNotEqual(overlong.returncode, 0)
        long_rows = json.loads(overlong.stdout)
        self.assertEqual(long_rows[0]["error"], "line_exceeds_json_limit")
        self.assertFalse(long_rows[0]["complete"])

    def test_find_json_preserves_non_utf8_path_bytes(self):
        name = b"found-\xff.c"
        with open(os.fsencode(self.work) + b"/" + name, "wb") as file:
            file.write(b"int fixture;\n")
        req = {"request_version": 2,
               "argv": ["find", "--json", ".", "-maxdepth", "1",
                        "-type", "f", "-name", "found-*.c"]}
        p = subprocess.run([str(JASH), "--exec-json", json.dumps(req)], env=self.env,
                           cwd=self.work, capture_output=True, timeout=20)
        self.assertEqual(p.returncode, 0, p.stderr)
        result = json.loads(p.stdout)
        self.assertEqual(result["schema"], "jash.query.find.v1")
        self.assertEqual(len(result["items"]), 1, result)
        self.assertEqual(path_bytes(result["items"][0]["path"]), b"./" + name)

    def test_grep_json_declares_large_match_field_truncation(self):
        line = b"x" * 5000
        (self.work / "long-match.txt").write_bytes(line + b"\n")
        req = {"request_version": 2,
               "argv": ["grep", "--json", "-F", line.decode("ascii"), "long-match.txt"]}
        p = subprocess.run([str(JASH), "--exec-json", json.dumps(req)], env=self.env,
                           cwd=self.work, capture_output=True, timeout=20)
        self.assertEqual(p.returncode, 0, p.stderr)
        result = json.loads(p.stdout)
        self.assertEqual(len(result), 1, result)
        self.assertEqual(len(result[0]["match"]), 4000)
        self.assertEqual(result[0]["span"], {"start": 0, "end": 5000})
        self.assertTrue(result[0]["match_truncated"])

    def test_glob_json_preserves_non_utf8_pattern_and_path_bytes(self):
        name = b"glob-\xff.c"
        with open(os.fsencode(self.work) + b"/" + name, "wb") as file:
            file.write(b"fixture\n")
        req = {"request_version": 2,
               "argv": ["glob", "--json", tagged(b"glob-\xff*")]}
        p = subprocess.run([str(JASH), "--exec-json", json.dumps(req)], env=self.env,
                           cwd=self.work, capture_output=True, timeout=20)
        self.assertEqual(p.returncode, 0, p.stderr)
        result = json.loads(p.stdout)
        self.assertEqual(path_bytes(result["pattern"]), b"glob-\xff*")
        self.assertEqual(result["total_matches"], 1, result)
        self.assertEqual(path_bytes(result["matches"][0]["path"]), name)

    def test_byte_arguments_require_v2_and_reject_nul_and_noncanonical_base64(self):
        for version, value in ((1, tagged(b"marker")), (2, tagged(b"marker\x00other")),
                               (2, {"encoding": "base64", "data": "Zh=="}),
                               (2, {"encoding": "base64", "data": "Zg==AAAA"})):
            req = {"request_version": version, "argv": ["/usr/bin/touch", value]}
            p = subprocess.run([str(JASH), "--exec-json", json.dumps(req)], env=self.env,
                               cwd=self.work, capture_output=True, timeout=20)
            self.assertEqual(p.returncode, 2, p.stderr)
            self.assertEqual(list(self.work.iterdir()), [])

    def test_byte_argument_shell_operators_remain_literal(self):
        value = b"literal'; touch should-not-exist; #"
        req = {"request_version": 2, "argv": ["/usr/bin/printf", "%s", tagged(value)]}
        p = subprocess.run([str(JASH), "--exec-json", json.dumps(req)], env=self.env,
                           cwd=self.work, capture_output=True, timeout=20)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout, value)
        self.assertFalse((self.work / "should-not-exist").exists())

    def test_job_ir_uses_byte_cwd_argv_and_redirection_paths(self):
        cwd = os.fsencode(self.work) + b"/job-\xff"
        os.mkdir(cwd)
        source, dest = b"source-\x80", b"dest-\xff"
        with open(cwd + b"/" + source, "wb") as f: f.write(b"raw\x00\xffdata")
        req = {"id": "byte-ir", "protocol_version": 2, "op": "exec", "cwd": tagged(cwd),
               "caps": ["effect:exec", "job:create"], "ir": {"version": 1, "pipeline": [
                   {"argv": ["/usr/bin/cat", tagged(source)],
                    "redirs": [{"fd": 1, "kind": "truncate", "path": tagged(dest)}]}]}}
        p = subprocess.run([str(AGENT)], input=(json.dumps(req) + "\n").encode(),
                           env=self.env, cwd=self.work, capture_output=True, timeout=20)
        self.assertEqual(p.returncode, 0, p.stderr)
        rows = [json.loads(line) for line in p.stdout.splitlines()]
        self.assertEqual(rows[0]["schema_version"], 18)
        self.assertTrue(rows[0]["protocol_features"]["2"]["byte_paths"])
        self.assertEqual(rows[-1]["rc"], 0, rows[-1])
        with open(cwd + b"/" + dest, "rb") as f: self.assertEqual(f.read(), b"raw\x00\xffdata")


class Serializer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temp = tempfile.TemporaryDirectory(prefix="jash-envelope-unit-")
        cls.addClassCleanup(temp.cleanup)
        cls.binary = str(Path(temp.name) / "envelope")
        files = [ROOT / "tests/test_agent_envelope.c", ROOT / "c/core/agent_json.c",
                 ROOT / "c/core/json_writer.c", ROOT / "c/core/world.c", ROOT / "c/core/claims.c"]
        command = [os.environ.get("CC", "cc"), "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g",
                   "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                   "-I" + str(ROOT / "c"), *map(str, files), "-o", cls.binary]
        if os.environ.get("SANITIZE") == "1":
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        built = subprocess.run(command, capture_output=True)
        if built.returncode:
            raise RuntimeError(built.stderr.decode(errors="replace"))

    def result(self, *args):
        env = {k: v for k, v in os.environ.items() if not k.startswith("JASH_")}
        p = subprocess.run([self.binary, *args], capture_output=True, env=env, timeout=20)
        self.assertEqual(p.returncode, 0, p.stderr)
        return p.stdout

    def test_large_envelope_and_failed_sink_preserve_all_retained_records(self):
        raw = self.result()
        self.assertGreater(len(raw), 100000)
        data = json.loads(raw)
        for field in ("created", "modified", "deleted", "moved", "moves", "directories_created",
                      "directories_deleted", "links_created", "metadata_changed"):
            self.assertEqual(len(data[field]), 16, field)
        self.assertEqual(len(data["claim_conflicts"]), 64)
        self.assertTrue(data["claim_conflicts_truncated"])
        self.assertEqual(len(data["files_changed"]["stale_writes"]), 24)
        self.assertTrue(data["files_changed"]["truncated"])
        self.assertEqual(data["timeline_scope"]["lost_to_ring"], 5)
        self.assertEqual(data["world_delta"]["total_created"], 20)
        self.assertEqual(data["world_delta"]["total_modified"], 20)
        self.assertEqual(data["world_delta"]["total_deleted"], 20)
        self.assertIn(b"\xff", path_bytes(data["created"][0]))

    def test_integer_boundaries_are_lossless(self):
        data = json.loads(self.result("numbers"))
        self.assertEqual(data[0], str(-(2**63)))
        self.assertEqual(data[1], str(2**64 - 1))
        self.assertEqual(data[2], 2**53 - 1)
        self.assertIsInstance(data[3], str)
        self.assertEqual(len(data[3]), 309)

    def test_javascript_consumes_bytes_and_exact_numbers(self):
        node = shutil.which("node")
        if not node: self.skipTest("Node.js required for JavaScript qualification")
        script = '''const fs = require('fs'); const [e,n] = JSON.parse(fs.readFileSync(0,'utf8'));
if (e.schema !== 'jash.agent-envelope.v2' || e.created.length !== 16) process.exit(1);
if (e.created[0].encoding !== 'base64' || !Buffer.from(e.created[0].data,'base64').includes(255)) process.exit(2);
if (BigInt(n[0]) !== -(2n**63n) || BigInt(n[1]) !== 2n**64n-1n || n[2] !== Number.MAX_SAFE_INTEGER) process.exit(3);'''
        payload = json.dumps([json.loads(self.result()), json.loads(self.result("numbers"))]).encode()
        p = subprocess.run([node, "-e", script], input=payload, capture_output=True, timeout=20)
        self.assertEqual(p.returncode, 0, p.stderr)


if __name__ == "__main__":
    unittest.main()
