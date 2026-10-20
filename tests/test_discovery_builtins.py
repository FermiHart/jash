#!/usr/bin/env python3
"""Semantic discovery providers: JSON and native JTV2 share observations.

The small independent JDV/JTV decoder below checks native kinds and terminal
state. The actual operator importer additionally verifies schema/hash framing.
Every filesystem/environment fixture is private and synthetic.
"""
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("JASH_BUILD_DIR", ROOT / "build")).resolve()
JASH = Path(os.environ.get("JASH", BUILD / "jash")).resolve()
AGENT = Path(os.environ.get("JASH_AGENTD_BIN", BUILD / "jash-agentd")).resolve()


def native_value(data, offset=0):
    kind = data[offset]
    offset += 1
    if kind == 0:
        return (kind, None), offset
    if kind == 1:
        assert data[offset] in (0, 1)
        return (kind, bool(data[offset])), offset + 1
    if kind in (2, 3, 4, 5, 6):
        value = int.from_bytes(data[offset:offset + 8], "big", signed=kind in (2, 5, 6))
        return (kind, value), offset + 8
    if kind in (7, 8, 9, 10):
        length = int.from_bytes(data[offset:offset + 8], "big")
        offset += 8
        assert offset + length <= len(data)
        return (kind, data[offset:offset + length]), offset + length
    if kind == 11:
        count = int.from_bytes(data[offset:offset + 8], "big")
        offset += 8
        result = []
        for _ in range(count):
            value, offset = native_value(data, offset)
            result.append(value)
        return (kind, result), offset
    if kind == 12:
        count = int.from_bytes(data[offset:offset + 8], "big")
        offset += 8
        result = {}
        previous = None
        for _ in range(count):
            length = int.from_bytes(data[offset:offset + 8], "big")
            offset += 8
            key = data[offset:offset + length]
            assert previous is None or previous < key
            previous = key
            offset += length
            value, offset = native_value(data, offset)
            result[key.decode()] = value
        return (kind, result), offset
    raise AssertionError("unexpected native kind: " + str(kind))


def project(value):
    kind, value = value
    if kind == 12:
        return {key: project(item) for key, item in value.items()}
    if kind == 11:
        return [project(item) for item in value]
    if kind in (7, 10):
        return value.decode("utf-8")
    if kind == 9:
        try:
            return value.decode("utf-8")
        except UnicodeDecodeError:
            pass
    if kind in (8, 9):
        return {"encoding": "base64", "data": base64.b64encode(value).decode()}
    return value


def native_stream(data):
    assert data[:4] == b"JTV2"
    offset, rows, terminal, tags = 4, [], None, []
    while offset < len(data):
        assert offset + 9 <= len(data)
        tag = data[offset:offset + 1]
        size = int.from_bytes(data[offset + 1:offset + 9], "big")
        offset += 9
        payload = data[offset:offset + size]
        assert len(payload) == size
        offset += size
        tags.append(tag)
        if tag == b"I":
            assert payload[:8] == b"\0" * 8  # no fabricated logical credit
            assert payload[8:12] == b"JDV1"
            row, consumed = native_value(payload, 12)
            assert consumed == len(payload) and row[0] == 12
            rows.append(row)
        elif tag in (b"D", b"G", b"F"):
            assert payload[:4] == b"JSC1"
        elif tag == b"T":
            assert terminal is None and offset == len(data) and len(payload) >= 96
            terminal = dict(state=payload[0], completeness=payload[1],
                            truncated=payload[2], hash_present=payload[3],
                            items=int.from_bytes(payload[8:16], "big"),
                            logical_bytes=int.from_bytes(payload[16:24], "big"),
                            errors=int.from_bytes(payload[24:32], "big"),
                            status=int.from_bytes(payload[32:40], "big", signed=True),
                            reason=payload[96:].decode())
            assert int.from_bytes(payload[88:96], "big") == len(payload) - 96
        else:
            raise AssertionError("unexpected JTV2 frame")
    assert tags[:2] == [b"D", b"G"] and tags[-1:] == [b"T"]
    assert terminal is not None and terminal["items"] == len(rows)
    return rows, terminal


class DiscoveryBuiltins(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="jash-discovery-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.env = dict(HOME=str(self.directory), PATH=os.defpath, LC_ALL="C.UTF-8",
                        JASH_SIGIL="0", JASH_NUDGE="0", JASH_ADOPT="0",
                        JASH_MEMO="0", JASH_BRIEF="0", JASH_LOOPGUARD="off")
        self.command = self.directory / "executable with spaces"
        self.command.write_text("#!/bin/sh\nexit 0\n")
        self.command.chmod(0o700)

    def run_cli(self, *args, status=0, input=b"", stdout=subprocess.PIPE):
        result = subprocess.run([str(JASH), *args], cwd=self.directory, env=self.env,
                                input=input, stdout=stdout, stderr=subprocess.PIPE, timeout=25)
        self.assertEqual(result.returncode, status, (args, result.stderr, result.stdout))
        return result

    def execute(self, argv, status=0, stdout=subprocess.PIPE):
        encoded = [{"encoding": "base64", "data": base64.b64encode(value).decode()}
                   if isinstance(value, bytes) else value for value in argv]
        request = json.dumps({"protocol_version": 2, "argv": encoded})
        return self.run_cli("--exec-json", request, status=status, stdout=stdout)

    def report(self, name, *args, status=0):
        document = json.loads(self.execute([name, "--json", *args], status=status).stdout)
        schema = json.loads((ROOT / "schemas" / ("builtin-discovery-" + name + "-v1.schema.json")).read_text())
        Draft202012Validator.check_schema(schema)
        Draft202012Validator(schema).validate(document)
        return document

    def test_help_names_and_details_match_real_registry(self):
        registry = json.loads(self.run_cli("--builtins-json").stdout)
        report = self.report("help")
        self.assertEqual({row["name"] for row in report["items"]}, {row["name"] for row in registry["builtins"]})
        by_name = {row["name"]: row for row in registry["builtins"]}
        for row in report["items"]:
            expected = by_name[row["name"]]
            self.assertEqual((row["summary"], row["declared_effect"], row["argv_native"]),
                             (expected["help"], expected["effect"], expected["argv_native"]))
        report = self.report("help", "ls", "synthetic-missing", status=1)
        self.assertTrue(report["items"][0]["found"])
        self.assertFalse(report["items"][1]["found"])
        self.assertIsNone(report["items"][1]["contract_id"])

    def test_type_and_which_report_exact_names_paths_and_absence(self):
        rows = self.report("type", "ls", str(self.command), "synthetic-missing", status=1)["items"]
        self.assertEqual([row["kind"] for row in rows], ["builtin", "file", "missing"])
        self.assertEqual(rows[1]["path"], str(self.command))
        self.assertTrue(rows[1]["executable"])
        self.assertEqual(rows[2]["error"], "not_found")
        rows = self.report("which", str(self.command), "synthetic-missing", status=1)["items"]
        self.assertEqual(rows[0]["path"], str(self.command))
        self.assertFalse(rows[1]["found"])
        # Very long names must not become a successful lookup of a prefix.
        rows = self.report("which", "a" * 5000, status=1)["items"]
        self.assertEqual(rows[0]["name"], "a" * 5000)
        self.assertEqual(rows[0]["error"], "name_too_long")

    def test_type_follows_alias_and_function_precedence(self):
        text = self.run_cli("-c", "alias audit_alias='echo safe'; type --json audit_alias").stdout
        self.assertEqual(json.loads(text)["items"][0]["kind"], "alias")
        text = self.run_cli("-c", "ls() { true; }; type --json ls").stdout
        self.assertEqual(json.loads(text)["items"][0]["kind"], "function")
        text = self.run_cli("-c", "ls() { true; }; type --json -a ls").stdout
        kinds = [row["kind"] for row in json.loads(text)["items"]]
        self.assertEqual(kinds[:2], ["function", "builtin"])

    def test_version_hash_and_uname_are_actual_observations(self):
        version = self.report("version")["items"][0]
        self.assertEqual(version["sha256"], hashlib.sha256(JASH.read_bytes()).hexdigest())
        self.assertEqual(Path(version["binary_path"]), JASH.resolve())
        self.assertIn(version["version"].encode(), self.run_cli("--version").stdout)
        identity = self.report("uname")["items"][0]
        for field, actual in zip(("sysname", "nodename", "release", "version", "machine"), os.uname()):
            self.assertEqual(identity[field], actual)
        selected = self.report("uname", "-sr")["items"][0]
        self.assertEqual(selected["sysname"], os.uname().sysname)
        self.assertEqual(selected["release"], os.uname().release)
        self.assertIsNone(selected["nodename"])

    def test_osif_caps_are_declared_without_running_probes(self):
        report = self.report("osif", "caps")
        self.assertEqual(len(report["items"]), 8)
        self.assertEqual(len({row["bit"] for row in report["items"]}), 8)
        human = re.sub(rb"\x1b\[[0-9;]*m", b"", self.run_cli("-c", "osif caps").stdout).decode()
        for row in report["items"]:
            self.assertIn(("+" if row["advertised"] else "-") + row["capability"], human)
            self.assertEqual(row["status"], "advertised" if row["advertised"] else "unavailable")
            self.assertIsNone(row["detail"])
        self.assertFalse((self.directory / "Docs").exists())

    def test_osif_checks_run_actual_probes_and_preserve_provider_failure(self):
        for operation in ("check", "score"):
            result = subprocess.run([str(JASH), '--exec-json', json.dumps({
                'protocol_version': 2, 'argv': ['osif', '--json', operation]})],
                cwd=self.directory, env=self.env, capture_output=True, timeout=25)
            self.assertIn(result.returncode, (0, 1), result.stderr)
            document = json.loads(result.stdout)
            schema = json.loads((ROOT / 'schemas/builtin-discovery-osif-v1.schema.json').read_text())
            Draft202012Validator(schema).validate(document)
            rows = document['items']
            self.assertEqual(len(rows), 8)
            self.assertEqual(result.returncode, int(any(row['status'] == 'fail' for row in rows)))
            for row in rows:
                self.assertEqual(row['operation'], operation)
                self.assertIn(row['status'], ('pass', 'fail') if row['advertised'] else ('skip',))
                self.assertIsInstance(row['detail'], str)
            sysid = next(row for row in rows if row['capability'] == 'SYSID')
            self.assertEqual(sysid['status'], 'pass')
            self.assertIn(os.uname().sysname, sysid['detail'])
        source, library = self.directory / 'sysid-fault.c', self.directory / 'sysid-fault.so'
        source.write_text('#include <errno.h>\n#include <sys/utsname.h>\nint uname(struct utsname *p) {(void)p; errno=EIO; return -1;}\n')
        compiled = subprocess.run(['cc', '-shared', '-fPIC', str(source), '-o', str(library)],
                                  env=self.env, capture_output=True, timeout=25)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        self.env['LD_PRELOAD'] = str(library)
        failed = self.report('osif', 'check', status=1)
        self.assertEqual(next(row for row in failed['items'] if row['capability'] == 'SYSID')['status'], 'fail')
        raw, terminal = native_stream(self.execute(['osif', '--data-v2', 'score'], status=1).stdout)
        rows = [project(row) for row in raw]
        self.assertEqual(next(row for row in rows if row['capability'] == 'SYSID')['status'], 'fail')
        # Every probe outcome was collected: a negative probe is a complete
        # diagnostic result, distinct from an incomplete provider collection.
        self.assertEqual((terminal['state'], terminal['status'], terminal['errors']), (0, 1, 0))
        self.assertFalse((self.directory / 'Docs').exists())

    def test_version_unavailable_binary_is_failed_source_terminal(self):
        source, library = self.directory / 'exe-fault.c', self.directory / 'exe-fault.so'
        source.write_text('#include <errno.h>\n#include <unistd.h>\nssize_t readlink(const char *p, char *b, size_t n) {(void)p;(void)b;(void)n;errno=EIO;return -1;}\n')
        compiled = subprocess.run(['cc', '-shared', '-fPIC', str(source), '-o', str(library)],
                                  env=self.env, capture_output=True, timeout=25)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        self.env['LD_PRELOAD'] = str(library)
        report = self.report('version', status=1)
        self.assertEqual(report['items'][0]['error'], 'binary_path_unavailable')
        self.assertIsNone(report['items'][0]['sha256'])
        for selector in ('--data', '--data-v2'):
            rows, terminal = native_stream(self.execute(['version', selector], status=1).stdout)
            self.assertEqual([project(row) for row in rows], report['items'])
            self.assertEqual((terminal['state'], terminal['completeness'], terminal['status']), (2, 2, 1))
            self.assertEqual(terminal['errors'], 1)

    def test_native_rows_match_json_and_retain_value_kinds(self):
        cases = [("help", ["ls"]), ("type", [str(self.command)]),
                 ("which", [str(self.command)]), ("version", []),
                 ("uname", []), ("osif", ["caps"])]
        for name, args in cases:
            with self.subTest(name=name):
                expected = self.report(name, *args)["items"]
                wire = self.execute([name, "--data", *args]).stdout
                rows, terminal = native_stream(wire)
                self.assertEqual([project(row) for row in rows], expected)
                self.assertEqual((terminal["state"], terminal["completeness"], terminal["status"]), (0, 1, 0))
                self.assertEqual(terminal["errors"], 0)
                self.assertEqual(terminal["hash_present"], 1)
                fields = ",".join(expected[0])
                imported = self.run_cli("-c", "pick --from-data " + fields, input=wire).stdout
                self.assertEqual([json.loads(line) for line in imported.splitlines()], expected)
                if name in ("type", "which"):
                    self.assertEqual(rows[0][1]["path"][0], 9)  # JDV_PATH, not JSON text
                    self.assertEqual(rows[0][1]["executable"][0], 1)
                if name == "osif":
                    self.assertEqual(rows[0][1]["bit"][0], 3)  # exact JDV_U64

    def test_binary_path_bytes_are_reversible_in_both_adapters(self):
        name = os.fsencode(self.directory) + b"/native \xff command"
        descriptor = os.open(name, os.O_CREAT | os.O_WRONLY, 0o700)
        os.write(descriptor, b"#!/bin/sh\nexit 0\n")
        os.close(descriptor)
        report = self.report("which", name)
        self.assertEqual(base64.b64decode(report["items"][0]["path"]["data"]), name)
        native, terminal = native_stream(self.execute(["which", "--data", name]).stdout)
        self.assertEqual(native[0][1]["path"], (9, name))
        self.assertEqual(project(native[0]), report["items"][0])
        self.assertEqual(terminal["state"], 0)

    def test_bad_arguments_use_json_error_or_failed_native_terminal(self):
        error_schema = json.loads((ROOT / "schemas/builtin-error-v1.schema.json").read_text())
        for name in ("help", "type", "which", "version", "uname", "osif"):
            with self.subTest(name=name):
                document = json.loads(self.execute([name, "--json", "--invalid"], status=2).stdout)
                Draft202012Validator(error_schema).validate(document)
                self.assertFalse(document["complete"])
                wire = self.execute([name, "--data", "--invalid"], status=2).stdout
                rows, terminal = native_stream(wire)
                self.assertEqual(rows, [])
                self.assertEqual((terminal["state"], terminal["completeness"], terminal["status"]), (2, 2, 2))
                self.assertEqual(terminal["errors"], 1)
                self.assertTrue(terminal["reason"])
                refused = self.run_cli("-c", "count --from-data", input=wire, status=2)
                self.assertEqual(refused.stdout, b"")
        self.execute(["osif", "--json", "report", "unexpected"], status=2)
        self.assertFalse((self.directory / "Docs").exists())
        self.execute(["help", "--json", "--data"], status=2)

    def test_flag_position_and_human_compatibility(self):
        document = json.loads(self.execute(["help", "ls", "--json"]).stdout)
        self.assertEqual(document["items"][0]["name"], "ls")
        document = self.report("help", "--", "--json", status=1)
        self.assertEqual(document["items"][0]["name"], "--json")
        self.assertIn(b"builtin", self.run_cli("-c", "type ls").stdout)
        self.assertEqual(self.execute(["which", str(self.command)]).stdout.rstrip(), str(self.command).encode())
        self.assertIn(str(self.command).encode(), self.execute(["type", str(self.command)]).stdout)
        self.assertIn(os.uname().release.encode(), self.run_cli("-c", "uname -a").stdout)
        self.assertIn(b"OSIF target=", self.run_cli("-c", "osif caps").stdout)
        summary = self.report("help", "ls")["items"][0]["summary"]
        self.assertIn(summary.encode(), self.run_cli("-c", "help ls").stdout)

    def test_document_budget_and_checked_delivery_never_claim_success(self):
        self.env["JASH_LLM_BUDGET"] = "1"
        document = json.loads(self.execute(["help", "--json"], status=75).stdout)
        self.assertEqual(document["error"]["code"], "output_budget_exceeded")
        self.assertFalse(document["complete"])
        # Explicit typed bytes have their own resource bounds, not prose caps.
        rows, terminal = native_stream(self.execute(["help", "--data", "ls"]).stdout)
        self.assertEqual(len(rows), 1)
        self.assertEqual(terminal["state"], 0)
        del self.env["JASH_LLM_BUDGET"]
        for mode in ("--json", "--data"):
            with open("/dev/full", "wb") as full:
                self.execute(["help", mode], status=125, stdout=full)
            read_fd, write_fd = os.pipe()
            os.close(read_fd)
            try:
                self.execute(["help", mode], status=125, stdout=write_fd)
            finally:
                os.close(write_fd)

    def test_strict_profile_help_uses_its_own_compiled_registry(self):
        request = dict(id="strict-help", argv=["help", "--json"], caps=["effect:read"])
        completed = subprocess.run([str(AGENT)], cwd=self.directory, env=self.env,
                                   input=(json.dumps(request) + "\n").encode(),
                                   capture_output=True, timeout=20)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        handshake, result = [json.loads(line) for line in completed.stdout.splitlines()]
        self.assertEqual(result["rc"], 0, result)
        names = {row["name"] for row in json.loads(result["stdout"])["items"]}
        self.assertEqual(names, set(handshake["effect_map"]))
        self.assertNotIn("module", names)


if __name__ == "__main__":
    unittest.main()
