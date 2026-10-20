#!/usr/bin/env python3
"""ADR-0056 W01: live inventory and pre-effect structural enforcement.

This gate covers the implemented discovery slice, not W01 completion or the
future all-builtin option/schema denominator. All runtime tests use private HOME.
"""
import base64
import json
import os
from pathlib import Path
import random
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("JASH_BUILD_DIR", ROOT / "build")).resolve()
JASH = Path(os.environ.get("JASH", BUILD / "jash")).resolve()
AGENT = Path(os.environ.get("JASH_AGENTD_BIN", BUILD / "jash-agentd")).resolve()


class Inventory(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="jash-contract-")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("JASH_")}
        self.env.update(HOME=str(self.root), JASH_SIGIL="0", JASH_NUDGE="0",
                        JASH_ADOPT="0", JASH_MEMO="0", JASH_LOOPGUARD="off")

    def cli(self, binary, *args, good=True):
        p = subprocess.run([str(binary), *args], env=self.env, cwd=self.root,
                           capture_output=True, timeout=15)
        if good:
            self.assertEqual(p.returncode, 0, p.stderr)
        else:
            self.assertNotEqual(p.returncode, 0)
        return p

    def inventory(self, binary, *args):
        out = self.cli(binary, "--contract-json", *args).stdout
        self.assertEqual(out.count(b"\n"), 1)
        return json.loads(out)

    def daemon(self, requests):
        wire = "".join(json.dumps(r, separators=(",", ":")) + "\n" for r in requests)
        p = subprocess.run([str(AGENT)], input=wire.encode(), env=self.env,
                           cwd=self.root, capture_output=True, timeout=30)
        self.assertEqual(p.returncode, 0, p.stderr)
        return [json.loads(row) for row in p.stdout.splitlines()]

    def test_general_inventory_is_exact_compiled_registry(self):
        d = self.inventory(JASH)
        legacy = json.loads(self.cli(JASH, "--builtins-json").stdout)
        by_name = {b["name"]: b for b in d["operations"] if b["kind"] == "builtin"}
        self.assertEqual(d["profile"], "general")
        self.assertEqual(set(by_name), {b["name"] for b in legacy["builtins"]})
        self.assertEqual(d["coverage"]["compiled_builtins"], legacy["count"])
        for b in legacy["builtins"]:
            row = by_name[b["name"]]
            self.assertEqual(row["id"], "builtin:" + b["name"])
            self.assertEqual(row["summary"], b["help"])
            self.assertEqual(row["argv_native"], b["argv_native"])
            self.assertEqual(row["declared_effect"], b["effect"])
            self.assertIsNone(row["options"])
            self.assertEqual(row["complete"], row['operations_complete'] and
                all(mode['complete'] for mode in row['structured_modes'].values()))
        ids = [r["id"] for r in d["operations"]]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertFalse(d["complete"])
        manifest = json.loads((ROOT / 'schemas/BUILTIN_CONTRACTS.json').read_text())
        self.assertEqual(d["coverage"]["denominator_frozen"], manifest['denominator_frozen'])

    def test_agent_inventory_matches_runtime_handshake(self):
        d = self.inventory(AGENT)
        # Discovery is before runtime initialization/signing key creation.
        self.assertEqual(list(self.root.iterdir()), [])
        handshake = self.daemon([])[0]
        builtins = [r for r in d["operations"] if r["kind"] == "builtin"]
        self.assertEqual(d["profile"], "agent-strict")
        self.assertEqual(len(builtins), handshake["inventory"]["builtins"])
        self.assertEqual({r["name"] for r in builtins}, set(handshake["effect_map"]))
        self.assertNotIn("module", {r["name"] for r in builtins})
        for r in d["operations"]:
            self.assertEqual(r["availability"], "compiled")

    def test_documented_inventory_counts_match_both_live_profiles(self):
        general = self.inventory(JASH)
        strict = self.inventory(AGENT)
        self.assertEqual((sum(r["kind"] == "builtin" for r in general["operations"]),
                          sum(r["kind"] == "runtime_operation" for r in general["operations"])),
                         (171, 30))
        self.assertEqual((sum(r["kind"] == "builtin" for r in strict["operations"]),
                          sum(r["kind"] == "runtime_operation" for r in strict["operations"])),
                         (152, 30))
        if (ROOT / "Docs/AGENT_CONTRACT.md").exists():
            docs = " ".join((ROOT / "Docs/AGENT_CONTRACT.md").read_text(encoding="utf-8").split())
            self.assertIn("201 general / 182 agent-strict", docs)
            self.assertIn("9 job operations, 8 store operations, 12 budget/data operations, and one exec operation", docs)

    def test_selector_does_not_resolve_external_or_execute(self):
        for binary in (JASH, AGENT):
            for selector in ("builtin:; touch marker", "runtime:job.missing", "builtin:/usr/bin/true"):
                row = json.loads(self.cli(binary, "--contract-json", selector, good=False).stdout)
                self.assertEqual(row["error"]["operation"], selector)
                self.assertFalse(row["executed"])
            self.cli(binary, "--contract-json", "builtin:cp", "touch", "marker", good=False)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_lookup_matches_full_document(self):
        for binary in (JASH, AGENT):
            full = self.inventory(binary)
            for name in ("builtin::", "builtin:[", "builtin:cp", "runtime:job.start", "runtime:job.read"):
                row = self.inventory(binary, name)["operations"]
                self.assertEqual(row, [r for r in full["operations"] if r["id"] == name])

    def test_declared_forbidden_fields_fail_before_effect(self):
        commands = [r for r in self.inventory(AGENT)["operations"] if r["kind"] == "runtime_operation"]
        requests = []
        for i, row in enumerate(commands):
            req = {"id": f"bad-{i}", "protocol_version": 2, "op": row["name"],
                   "caps": ["effect:exec", "job:create", "job:read", "job:signal", "job:pty:resize", "job:stdin:write"]}
            values = {"job_id": "missing", "wait_ms": 1, "rows": 2, "cols": 2,
                      "stream": "stdout", "offset": 0, "limit": 1,
                       "data_b64": "eA==", "background": True, "max_bytes": 0,
                        "root_namespace": "agent_receipts", "root_name": "missing", "root_blake3": "0" * 64,
                        "target_id": "missing", "revision": "0" * 64, "fd_enabled": True,
                         "artifact_ref": "v-missing", "transport": "inline",
                         "stream_ref": "s-" + "0" * 32 + "-" + "0" * 16,
                         "credit_bytes": 1, "ack_offset": 0}
            for field in row["required_fields"]:
                if field not in req: req[field] = values[field]
            if row["name"] in ("exec", "job.start"):
                req["argv"] = ["/usr/bin/touch", str(self.root / "marker")]
            # A recognized field from another operation, not a parser-unknown key.
            forbidden = "job_id" if "job_id" not in row["allowed_fields"] else "background"
            req[forbidden] = values[forbidden]
            requests.append(req)
        rows = self.daemon(requests)
        self.assertEqual(len(rows), len(requests) + 1)
        for row in rows[1:]:
            self.assertIn("field is not valid", row.get("error", ""), row)
        self.assertFalse((self.root / "marker").exists())

    def test_required_fields_rejected_and_next_request_still_works(self):
        requests = [
            {"id": "missing-wait", "protocol_version": 2, "op": "job.wait",
             "job_id": "missing", "caps": ["job:read"]},
            {"id": "ok", "protocol_version": 2, "op": "exec",
             "argv": ["/usr/bin/printf", "%s", "exact \\\" bytes"],
             "caps": ["effect:exec", "job:create"]},
        ]
        rows = self.daemon(requests)
        self.assertIn("wait_ms", rows[1]["error"])
        self.assertEqual(rows[-1]["rc"], 0)
        self.assertTrue(rows[-1]["terminal"])
        self.assertIn("receipt_ref", rows[-1])

    def test_request_identity_is_not_truncated_by_json_escaping(self):
        ids = ['"' * 47, '\\' * 47, '\x01' * 47]
        requests = [{"id": value, "protocol_version": 2, "op": "exec",
                     "argv": ["/usr/bin/true"], "caps": ["effect:exec", "job:create"]}
                    for value in ids]
        rows = self.daemon(requests)[1:]
        self.assertEqual([r["id"] for r in rows], ids)
        self.assertTrue(all(r["rc"] == 0 for r in rows))
        # The synchronous argv route uses the same decoded ID bound.
        ids = ["s" + value[1:] for value in ids]
        requests = [{"id": value, "argv": ["true"], "caps": ["effect:read"]} for value in ids]
        rows = self.daemon(requests)[1:]
        self.assertEqual([r["id"] for r in rows], ids)
        self.assertTrue(all(r["rc"] == 0 for r in rows))

    def test_schemas_and_metadata_share_required_and_allowed_fields(self):
        for row in self.inventory(AGENT)["operations"]:
            if row["kind"] != "runtime_operation": continue
            schema = row["input_schema"]
            self.assertEqual(set(schema["properties"]), set(row["allowed_fields"]) | {"request_version"})
            self.assertEqual(set(schema["required"]), set(row["required_fields"]) - {"protocol_version"})
            self.assertFalse(schema["additionalProperties"])
            self.assertEqual(schema["properties"]["op"]["const"], row["name"])
            self.assertEqual(schema["properties"]["id"]["x-maxDecodedBytes"], 47)
            self.assertEqual(len(schema["oneOf"]), 2)

    def test_caps_escape_environment_without_truncation(self):
        value = '"\\\n\t' * 3000 + "fim"
        self.env["JASH_SIGIL"] = value
        row = json.loads(self.cli(JASH, "--capabilities-json").stdout)
        self.assertEqual(row["attest"]["sigil_env"], value)

    def test_closed_output_is_not_success(self):
        for binary in (JASH, AGENT):
            rd, wr = os.pipe()
            os.close(rd)
            try:
                p = subprocess.run([str(binary), "--contract-json"], env=self.env,
                                   cwd=self.root, stdout=wr, stderr=subprocess.PIPE, timeout=10)
                self.assertNotEqual(p.returncode, 0)
            finally:
                os.close(wr)

    def test_document_schema_fields_and_generated_markdown(self):
        schema = json.loads((ROOT / "schemas/agent-contract-inventory-v1.schema.json").read_text())
        self.assertEqual(schema["$schema"], "https://json-schema.org/draft/2020-12/schema")
        for binary in (JASH, AGENT):
            d = self.inventory(binary)
            self.assertEqual(set(d), set(schema["required"]))
            for row in d["operations"]:
                definition = schema["$defs"]["builtin" if row["kind"] == "builtin" else "runtime"]
                self.assertTrue(set(definition["required"]).issubset(row))
                self.assertTrue(set(row).issubset(definition["properties"]))
            p = subprocess.run(["python3", str(ROOT / "tools/agent_contract.py"),
                                "--binary", str(binary), "--operation", "runtime:job.read",
                                "--format", "markdown"], capture_output=True, timeout=15,
                               env=self.env, cwd=self.root)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertIn(b"runtime:job.read", p.stdout)
            self.assertIn(b"| offset | integer | yes |", p.stdout)
            self.assertNotIn(b"builtin:cp", p.stdout)

    def test_sync_env_overflow_never_executes_partial_request(self):
        request = {"argv": ["/usr/bin/touch", "marker"],
                   "env": {f"KEY{i}": str(i) for i in range(17)}}
        self.cli(JASH, "--exec-json", json.dumps(request), good=False)
        self.assertFalse((self.root / "marker").exists())

    def test_sync_duplicate_env_key_is_rejected_before_effect(self):
        wire = '{"argv":["/usr/bin/touch","marker"],"env":{"KEY":"one","KEY":"two"}}'
        self.cli(JASH, "--exec-json", wire, good=False)
        self.assertFalse((self.root / "marker").exists())

    def test_sync_argv_line_exclusive_even_when_argv_empty(self):
        self.cli(JASH, "--exec-json", '{"argv":[],"line":"/usr/bin/touch marker"}', good=False)
        self.assertFalse((self.root / "marker").exists())

    def test_general_serve_rejects_partial_requests_and_recovers(self):
        overflow = json.dumps({"argv": ["/usr/bin/touch", "marker"],
                               "env": {f"KEY{i}": str(i) for i in range(17)}})
        wires = [overflow,
                 '{"argv":["/usr/bin/touch","marker"],"env":{"KEY":"one","KEY":"two"}}',
                 '{"argv":[],"line":"/usr/bin/touch marker"}',
                 '{"id":"next","argv":["/usr/bin/printf","ok"]}']
        p = subprocess.run([str(JASH), "--serve"], input=("\n".join(wires) + "\n").encode(),
                           env=self.env, cwd=self.root, capture_output=True, timeout=15)
        self.assertEqual(p.returncode, 0, p.stderr)
        rows = [json.loads(row) for row in p.stdout.splitlines()]
        self.assertEqual(len(rows), 5)
        for row in rows[1:4]: self.assertIn("error", row)
        self.assertEqual(rows[-1]["rc"], 0)
        self.assertEqual(rows[-1]["stdout"], "ok")
        self.assertFalse((self.root / "marker").exists())

    def test_malformed_json_values_never_execute(self):
        invalid = [('"id":oops'), ('"id":01'), ('"id":1e'),
                   ('"timeout_ms":{"x":[1,,2]}'), ('"timeout_ms":"\\q"'),
                   ('"timeout_ms":' + '[' * 128 + '0' + ']' * 128)]
        for i, field in enumerate(invalid):
            with self.subTest(field=field[:64]):
                marker = f"malformed-{i}"
                wire = '{"argv":["/usr/bin/touch","' + marker + '"],' + field + '}'
                p = self.cli(JASH, "--exec-json", wire, good=False)
                self.assertEqual(p.returncode, 2, p.stderr)
                self.assertFalse((self.root / marker).exists())

    def test_surrogate_pairs_roundtrip_to_utf8(self):
        text = "é∞😀𐀀"
        request = {"argv": ["/usr/bin/printf", "%s", text]}
        p = self.cli(JASH, "--exec-json", json.dumps(request, ensure_ascii=True))
        self.assertEqual(p.stdout, text.encode("utf-8"))
        p = self.cli(JASH, "--exec-json", json.dumps(request, ensure_ascii=False))
        self.assertEqual(p.stdout, text.encode("utf-8"))

    def test_invalid_utf8_and_lone_surrogates_are_refused(self):
        for value in (b'\xc0\xaf', b'\xf4\x90\x80\x80', b'\\ud800', b'\\udc00', b'\\ud800\\u0041'):
            with self.subTest(value=value):
                wire = b'{"argv":["/usr/bin/touch","marker"],"timeout_ms":"' + value + b'"}'
                self.assertEqual(self.cli(JASH, "--exec-json", wire, good=False).returncode, 2)
                self.assertFalse((self.root / "marker").exists())
                wire = b'{"argv":["/usr/bin/touch","' + value + b'"]}'
                self.assertEqual(self.cli(JASH, "--exec-json", wire, good=False).returncode, 2)

    def test_general_ids_remain_opaque_valid_json_values(self):
        ids = [None, True, False, 0, -1, 1.5, [1, 2], {"nested": [1]}]
        requests = [{"id": value, "argv": ["true"]} for value in ids]
        p = subprocess.run([str(JASH), "--serve"],
                           input=("\n".join(json.dumps(r) for r in requests) + "\n").encode(),
                           env=self.env, cwd=self.root, capture_output=True, timeout=15)
        self.assertEqual(p.returncode, 0, p.stderr)
        rows = [json.loads(row) for row in p.stdout.splitlines()][1:]
        self.assertEqual([r["id"] for r in rows], ids)
        self.assertTrue(all(r["rc"] == 0 for r in rows))

    def test_invocation_contract_distinguishes_profiles_and_transports(self):
        schema = json.loads((ROOT / "schemas/agent-contract-invocation-v1.schema.json").read_text())
        for binary, profile in ((JASH, "general"), (AGENT, "agent-strict")):
            for name in ("serve", "exec-json"):
                row = self.inventory(binary, "invocation:" + name)
                self.assertEqual(set(row), set(schema["required"]))
                self.assertEqual(row["schema"], "jash.agent-contract.invocation.v1")
                self.assertEqual(row["profile"], profile)
                self.assertEqual(row["input_transport"], "stdio-jsonl" if name == "serve" else "argv-json")
                strict = profile == "agent-strict"
                self.assertEqual("caps" in row["allowed_fields"], strict)
                self.assertEqual("cancelable" in row["allowed_fields"], strict)
                self.assertEqual(row["traits"]["capabilities_required"], strict)
                self.assertEqual(row["traits"]["timeout_ms"], "effect-exec-only" if strict else "reserved")
                self.assertIn(["argv", "line"], row["exclusive_fields"])
                self.assertIn(["stdin", "stdin_b64"], row["exclusive_fields"])
                self.assertEqual(row["availability"], "requires_general_profile" if strict and name == "exec-json" else "compiled")
                self.assertFalse(row["complete"])
        self.assertEqual(list(self.root.iterdir()), [])

    def test_profile_builtin_exclusions_and_listener_availability_are_exact(self):
        general = self.inventory(JASH)
        strict = self.inventory(AGENT)
        general_names = {r["name"] for r in general["operations"] if r["kind"] == "builtin"}
        strict_names = {r["name"] for r in strict["operations"] if r["kind"] == "builtin"}
        excluded = {"clear", "alias", "unalias", "history", "fc", "source", ".",
                    "eval", "compgen", "complete", "abbr", "juxconfig", "plugin",
                    "module", "sigil", "theme", "statusbar", "spinner", "brain"}
        self.assertEqual(len(general_names), 171)
        self.assertEqual(len(strict_names), 152)
        self.assertEqual(general_names - strict_names, excluded)
        self.assertEqual(strict_names - general_names, set())
        if (ROOT / "Docs/AGENT_CONTRACT.md").exists():
            docs = (ROOT / "Docs/AGENT_CONTRACT.md").read_text(encoding="utf-8")
            normalized_docs = " ".join(docs.split())
            exclusion_prefix = "- Agent-strict builtin exclusions (19): "
            self.assertIn(exclusion_prefix, normalized_docs)
            exclusion_text = normalized_docs.split(exclusion_prefix, 1)[1].split(". The general", 1)[0]
            documented_exclusions = {item.strip().strip("`") for item in exclusion_text.split(",")}
            self.assertEqual(documented_exclusions, excluded)
            self.assertIn("`invocation:listen` is `requires_agent_profile` for general and `compiled` for agent-strict.", normalized_docs)
        general_listen = self.inventory(JASH, "invocation:listen")
        strict_listen = self.inventory(AGENT, "invocation:listen")
        self.assertEqual(general_listen["availability"], "requires_agent_profile")
        self.assertEqual(strict_listen["availability"], "compiled")
        self.assertEqual(general_listen["input_transport"], "unix-jsonl")
        self.assertEqual(strict_listen["input_transport"], "unix-jsonl")
        self.assertEqual(strict_listen["traits"]["authentication"], "canonical-scope-token-v1")
        self.assertEqual(list(self.root.iterdir()), [])

    def test_every_discovered_sync_field_reaches_the_real_parser(self):
        values = {"line": "/usr/bin/true", "stdin": "input", "stdin_b64": "eA==",
                  "cwd": str(self.root), "env": {"W01_FIELD_TEST": "value"},
                  "timeout_ms": 1000, "cancelable": False,
                  "caps": ["effect:exec"], "execution_spec_sha256": "sha256:" + "a" * 64}
        for binary in (JASH, AGENT):
            description = self.inventory(binary, "invocation:serve")
            requests = []
            denied = set()
            for i, field in enumerate(description["allowed_fields"]):
                req = {"id": f"{description['profile']}-field-{i}", "protocol_version": 2,
                       "argv": ["/usr/bin/true"]}
                if description["traits"]["capabilities_required"]: req["caps"] = ["effect:exec"]
                if field in values: req[field] = values[field]
                if field == "line": del req["argv"]
                if field == "line" and not description["traits"]["text_line_authorized"]:
                    denied.add(req["id"])
                    req["line"] = "/usr/bin/touch denied-marker"
                requests.append(req)
            alias = dict(requests[0], id=description["profile"] + "-alias")
            alias["request_version"] = alias.pop("protocol_version")
            requests.append(alias)
            p = subprocess.run([str(binary)] + (["--serve"] if binary == JASH else []),
                               input=("\n".join(json.dumps(r) for r in requests) + "\n").encode(),
                               env=self.env, cwd=self.root, capture_output=True, timeout=30)
            self.assertEqual(p.returncode, 0, p.stderr)
            rows = [json.loads(line) for line in p.stdout.splitlines()][1:]
            errors = [r for r in rows if "error" in r]
            self.assertEqual({r["id"] for r in errors}, denied)
            self.assertTrue(all("text line" in r["error"] for r in errors))
            self.assertTrue(all(r.get("rc", 1) != 0 for r in errors))
            terminal = [r for r in rows if "rc" in r and "error" not in r]
            self.assertEqual([r["id"] for r in terminal], [r["id"] for r in requests if r["id"] not in denied])
            self.assertTrue(all(r["rc"] == 0 for r in terminal))
            self.assertFalse((self.root / "denied-marker").exists())


class Writer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory(prefix="jash-json-writer-")
        cls.addClassCleanup(tmp.cleanup)
        cls.binary = str(Path(tmp.name) / "writer")
        cmd = [os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-I" + str(ROOT / "c"), str(ROOT / "tests/test_json_writer.c"),
               str(ROOT / "c/core/json_writer.c"), "-o", cls.binary]
        if os.environ.get("SANITIZE") == "1":
            cmd += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run(cmd, check=True, capture_output=True)
        cls.parser_binary = str(Path(tmp.name) / "parser")
        parser_cmd = [os.environ.get("CC", "cc"), "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g",
                      "-ffunction-sections", "-fdata-sections", "-I" + str(ROOT / "c"),
                      "-I" + str(ROOT / "libjef"), str(ROOT / "tests/test_agent_json_parser.c"),
                      str(ROOT / "c/core/agent_json.c"), str(ROOT / "c/core/json_writer.c"),
                      str(ROOT / "c/core/agent_contract.c"),
                      "-Wl,--gc-sections", "-o", cls.parser_binary]
        if os.environ.get("SANITIZE") == "1":
            parser_cmd += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run(parser_cmd, check=True, capture_output=True)

    def emit(self, mode, value):
        p = subprocess.run([self.binary, mode], input=value, capture_output=True, timeout=10)
        self.assertEqual(p.returncode, 0, p.stderr)
        return json.loads(p.stdout)

    def test_faults_short_writes_eintr_and_invalid_utf8(self):
        p = subprocess.run([self.binary], capture_output=True, timeout=10)
        self.assertEqual(p.returncode, 0, p.stderr)

    def test_text_crosses_buffers_without_loss(self):
        for n in (0, 1, 1000, 1023, 1024, 1025, 40000):
            text = "á∞😀\\\"\n\t" * n
            self.assertEqual(self.emit("text", text.encode()), text)

    def test_binary_all_octets_and_padding(self):
        rng = random.Random(56)
        for n in (0, 1, 2, 3, 4, 255, 256, 1023, 1024, 100001):
            data = bytes(range(256)) if n == 256 else bytes(rng.randrange(256) for _ in range(n))
            row = self.emit("bytes", data)
            self.assertEqual(row["encoding"], "base64")
            self.assertEqual(base64.b64decode(row["data"], validate=True), data)

    def test_unsigned_boundary(self):
        self.assertEqual(self.emit("u64", b""), 2**64 - 1)

    def test_protocol_parser_bounds_and_seeded_fuzz(self):
        p = subprocess.run([self.parser_binary], capture_output=True, timeout=30)
        self.assertEqual(p.returncode, 0, p.stderr)


if __name__ == "__main__":
    unittest.main()
