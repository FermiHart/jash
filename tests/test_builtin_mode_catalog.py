#!/usr/bin/env python3
"""Name catalogue and scoped output contracts must describe the running binary.

This checks the closed Linux builtin catalog and its executable descriptors.
Provider semantics and failure behavior are exercised by separate family gates.
Install tests/requirements-agent-json.txt; a missing independent validator is a
failure, never a successful skipped schema check. Fixtures use a private HOME
and an allowlisted environment; no caller credentials or real state are read.
"""
import base64
from collections import Counter
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from jsonschema import Draft202012Validator
from referencing import Registry, Resource


ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("JASH_BUILD_DIR", ROOT / "build")).resolve()
JASH = Path(os.environ.get("JASH", BUILD / "jash")).resolve()
AGENT = Path(os.environ.get("JASH_AGENTD_BIN", BUILD / "jash-agentd")).resolve()


class BuiltinModeCatalog(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = json.loads((ROOT / 'schemas/BUILTIN_CONTRACTS.json').read_text())
        cls.contracts = {row['name']: row for row in cls.manifest['builtins']}
        cls.schemas = {}
        for path in (ROOT / "schemas").glob("*.schema.json"):
            document = json.loads(path.read_text())
            if "$id" in document:
                if document["$id"] in cls.schemas:
                    raise AssertionError("duplicate schema identifier: " + document["$id"])
                cls.schemas[document["$id"]] = document
        cls.registry = Registry().with_resources(
            (name, Resource.from_contents(schema)) for name, schema in cls.schemas.items())

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="jash-mode-contract-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.env = dict(HOME=str(self.directory), PATH=os.defpath, LC_ALL="C.UTF-8",
                        JASH_SIGIL="0", JASH_ADOPT="0", JASH_NUDGE="0",
                        JASH_MEMO="0", JASH_LOOPGUARD="off")

    def run_cli(self, binary, *args, status=0):
        completed = subprocess.run([str(binary), *args], cwd=self.directory,
                                   env=self.env, capture_output=True, timeout=20)
        self.assertEqual(completed.returncode, status, completed.stderr)
        return completed

    def inventory(self, binary):
        return json.loads(self.run_cli(binary, "--contract-json").stdout)

    def validator(self, schema):
        Draft202012Validator.check_schema(schema)
        return Draft202012Validator(schema, registry=self.registry)

    def validate(self, schema, document):
        self.validator(schema).validate(document)

    def mode(self, name, mode, variant):
        operations = self.inventory(JASH)["operations"]
        builtin = next(row for row in operations if row["id"] == "builtin:" + name)
        return next(row for row in builtin["structured_modes"][mode]["variants"]
                    if row["id"] == variant)

    def execute(self, argv, status=0):
        request = json.dumps({"argv": argv}, separators=(",", ":"))
        return self.run_cli(JASH, "--exec-json", request, status=status)

    def daemon(self, requests):
        wire = "".join(json.dumps(row) + "\n" for row in requests).encode()
        completed = subprocess.run([str(AGENT)], cwd=self.directory, env=self.env,
                                   input=wire, capture_output=True, timeout=30)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        rows = [json.loads(line) for line in completed.stdout.splitlines()]
        self.assertEqual(len(rows), len(requests) + 1)
        return rows[1:]

    def test_both_profiles_validate_and_every_name_is_classified(self):
        for binary in (JASH, AGENT):
            with self.subTest(binary=binary.name):
                document = self.inventory(binary)
                self.validate({"$ref": "urn:jash:agent-contract:inventory:v1"}, document)
                rows = [row for row in document["operations"] if row["kind"] == "builtin"]
                counts = Counter(row["structured_relevance"] for row in rows)
                coverage = document["coverage"]
                self.assertEqual(coverage["classified_builtin_names"], len(rows))
                self.assertTrue(coverage["builtin_name_catalog_complete"])
                self.assertEqual(coverage["structured_candidates"], counts["structured"])
                self.assertEqual(coverage["execution_envelope_names"], counts["execution-envelope"])
                self.assertEqual(coverage["interactive_names"], counts["interactive"])
                self.assertNotIn("unclassified", counts)
                if binary == JASH:
                    self.assertEqual(counts, {"structured": 132, "execution-envelope": 29,
                                              "interactive": 10})
                # Builtin closure does not claim the entire runtime/language.
                self.assertFalse(document["complete"])
                self.assertEqual(coverage["denominator_frozen"], self.manifest['denominator_frozen'])
                qualified = 0
                for row in rows:
                    contract = self.contracts[row['name']]
                    self.assertTrue((ROOT / row["implementation_source"]).is_file(), row)
                    expected_modes = {}
                    for kind in ('json', 'data'):
                        variants = [v for v in contract['modes'] if v['mode'] == kind]
                        expected_modes[kind] = (contract['operations_complete'] and bool(variants)
                            and all(v['qualified'] and v['options_complete'] for v in variants))
                    expected_complete = all(expected_modes.values())
                    self.assertEqual(row["complete"], expected_complete)
                    self.assertEqual(row['operations_complete'], contract['operations_complete'])
                    self.assertEqual(row['operation_exclusions'], contract.get('operation_exclusions', []))
                    self.assertEqual(row['legacy_output_compatibility'], contract.get('legacy_output_compatibility', []))
                    qualified += expected_complete
                    self.assertIsNone(row["options"])
                    for kind, mode in row["structured_modes"].items():
                        self.assertEqual(mode["complete"], expected_modes[kind])
                        self.assertEqual(mode["variants_complete"], contract['operations_complete'])
                        self.assertEqual(mode["qualification"], "qualified" if expected_modes[kind] else "pending")
                        if row["structured_relevance"] == "execution-envelope":
                            self.assertEqual(mode["status"], "not-applicable")
                            self.assertEqual(mode["variants"], [])
                        if mode["variants"]:
                            self.assertEqual(mode["status"], "qualified" if expected_modes[kind] else "partial")
                            self.assertNotEqual(mode["implementation"], "none")
                self.assertEqual(coverage['qualified_complete'], qualified)

    def test_generated_catalog_is_current(self):
        completed = subprocess.run([os.environ.get('PYTHON', 'python3'),
            str(ROOT / 'tools/gen_builtin_contracts.py'), '--check'], cwd=ROOT,
            capture_output=True, timeout=20)
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_reviewed_builtin_denominator_is_closed(self):
        self.assertTrue(self.manifest['denominator_frozen'])
        candidates = [row for row in self.contracts.values() if row['role'] == 'structured']
        self.assertEqual(len(candidates), 132)
        for row in candidates:
            with self.subTest(name=row['name']):
                self.assertTrue(row['operations_complete'])
                self.assertEqual({mode['mode'] for mode in row['modes']}, {'json', 'data'})
                for mode in row['modes']:
                    self.assertTrue(mode['options_complete'])
                    self.assertTrue(mode['qualified'])
                    self.assertEqual(mode['qualification_scope'], ['linux'])
                    self.assertTrue(mode['operations'])
                    self.assertTrue(mode['semantic_tests'])

    def test_mode_references_and_scoped_inputs_are_valid_and_resolve_locally(self):
        inventory = self.inventory(JASH)
        names = {row["name"] for row in inventory["operations"] if row["kind"] == "builtin"}
        for row in inventory["operations"]:
            if row["kind"] != "builtin":
                continue
            if row["alias_of"]:
                # A semantic alias can fix a subcommand/flag (risk -> explain --risk).
                self.assertIn(row["alias_of"].split()[0], names)
            for mode in row["structured_modes"].values():
                ids = [variant["id"] for variant in mode["variants"]]
                self.assertEqual(len(ids), len(set(ids)))
                for variant in mode["variants"]:
                    with self.subTest(name=row["name"], variant=variant["id"]):
                        self.assertTrue(variant["semantic_validation_required"])
                        declared = next(v for v in self.contracts[row['name']]['modes']
                                        if v['id'] == variant['id'] and v['mode'] ==
                                        ('data' if mode is row['structured_modes']['data'] else 'json'))
                        self.assertEqual(variant["options_complete"], declared['options_complete'])
                        self.assertEqual(variant['qualified'], declared['qualified'])
                        for field in ('operations', 'semantic_tests', 'qualification_scope', 'options', 'resource_limits'):
                            self.assertEqual(variant[field], declared[field])
                        validator = self.validator(variant["input_schema"])
                        argv = [row["name"]]
                        if variant["selector"]:
                            argv.append(variant["selector"])
                        validator.validate(argv)
                        self.assertFalse(validator.is_valid(["unrelated-builtin", *argv[1:]]))
                        if variant["selector"]:
                            self.assertFalse(validator.is_valid([row["name"]]))
                        for field in ("result_schema", "error_schema"):
                            if variant[field] is not None:
                                ref = variant[field]["$ref"]
                                self.assertIn(ref, self.schemas)
                                self.validator(self.schemas[ref])
                        projection = variant['resource_limits'].get('projection_schema')
                        if projection is not None:
                            # Resolve the binary's published URI with an
                            # independent library, including its JSON Pointer.
                            projected = self.registry.resolver().lookup(projection).contents
                            Draft202012Validator.check_schema(projected)
                        if variant["framing"] in ("jtv1", "jtv2"):
                            self.assertIsNone(variant["result_schema"])

    def test_management_modes_publish_scoped_truthful_descriptors(self):
        cases = [('alias', 'list-lookup', [], 0), ('unalias', 'remove', ['missing_alias'], 1),
                 ('enable', 'list', [], 0), ('hash', 'list', [], 0),
                 ('plugin', 'list', ['list'], 0), ('module', 'list-info', ['list'], 0),
                 ('jash_state', 'persistence', ['clear'], 0),
                 ('config', 'query', ['show'], 0), ('doctor', 'checks', [], 0)]
        for name, variant, operands, status in cases:
            with self.subTest(name=name):
                descriptor = self.mode(name, 'json', variant + '-json')
                self.assertEqual(descriptor['framing'], 'json')
                self.assertEqual(descriptor['input_framing'], 'none')
                self.assertEqual(descriptor['selector_scope'], 'before-double-dash')
                self.assertEqual(descriptor['error_schema'], {'$ref': 'urn:jash:builtin-error:v1'})
                self.assertTrue(descriptor['complete'])
                document = json.loads(self.execute([name, '--json', *operands], status=status).stdout)
                self.validate(descriptor['result_schema'], document)
                for suffix, selector in [('data', '--data'), ('data-v2', '--data-v2')]:
                    data = self.mode(name, 'data', variant + '-' + suffix)
                    self.assertEqual((data['selector'], data['framing'], data['input_framing']), (selector, 'jtv2', 'none'))
                    self.assertIsNone(data['result_schema'])
                    self.assertEqual(data['error_policy'], 'typed-terminal')
                    self.assertEqual(data['resource_limits']['rows'], 4096)
                    self.assertTrue(data['complete'])

    def test_data_names_preserve_their_actual_framing(self):
        ls_modes = {self.mode("ls", "data", name)["framing"] for name in ("jtv1", "jtv2")}
        self.assertEqual(ls_modes, {"jtv1", "jtv2"})
        page = self.mode("cat", "data", "byte-page")
        self.assertEqual(page["framing"], "json")
        self.assertEqual(page["result_schema"], {"$ref": "urn:jash:file-page:v1"})
        checkpoint = self.mode("checkpoint", "json", "list-json")
        self.assertEqual({option["name"] for option in checkpoint["options"]},
                         {"--json", "--list", "-l", "--"})
        self.assertEqual(checkpoint["selector_scope"], "before-double-dash")
        self.assertEqual(checkpoint['operations'],
                         [{'grammar': '[list|--list|-l]', 'effect': 'read', 'reason': ''}])
        diff = self.mode('checkpoint', 'json', 'diff-json')
        self.assertNotIn('--scope', {option['name'] for option in diff['options']})

    def test_new_filesystem_success_and_error_documents_match_mode_schemas(self):
        fixture = self.directory / 'space " quote.txt'
        fixture.write_bytes(b"first\nsecond\x00\xff\n")
        cases = [
            ("cat", "json", "metadata-array", ["cat", "--json", str(fixture)]),
            ("cat", "data", "byte-page", ["cat", "--data", "--offset", "0", "--limit", "16", str(fixture)]),
            ("glob", "json", "match-page", ["glob", "--json", str(self.directory / "*.txt")]),
            ("find", "json", "query-page", ["find", "--json", str(fixture)]),
            ("ls", "json", "json-rows", ["ls", "--json", str(fixture)]),
        ]
        for name, mode, identifier, argv in cases:
            with self.subTest(name=name, mode=mode):
                descriptor = self.mode(name, mode, identifier)
                self.validate(descriptor["input_schema"], argv)
                raw = self.execute(argv).stdout
                rows = [json.loads(line) for line in raw.splitlines()] if descriptor["framing"] == "jsonl" else [json.loads(raw)]
                self.assertTrue(rows)
                for document in rows:
                    self.validate(descriptor["result_schema"], document)
                if mode == "data":
                    self.assertEqual(base64.b64decode(rows[0]["data"]["data"]), fixture.read_bytes()[:16])
                    self.assertIsNone(rows[0]["text"])
        missing = str(self.directory / "missing")
        document = json.loads(self.execute(["cat", "--json", missing], status=1).stdout)
        self.validate(self.mode("cat", "json", "metadata-array")["result_schema"], document)
        self.assertEqual(document[0]["error"], "read_failed")
        document = json.loads(self.execute(["find", "--json", missing], status=1).stdout)
        self.validate(self.mode("find", "json", "query-page")["result_schema"], document)
        self.assertFalse(document["complete"])
        self.assertGreater(document["errors_total"], 0)

    def test_env_pwd_success_and_explicit_errors_have_distinct_schemas(self):
        for name, identifier in (("env", "environment-map"), ("pwd", "working-directory")):
            descriptor = self.mode(name, "json", identifier)
            argv = [name, "--json"]
            self.validate(descriptor["input_schema"], argv)
            self.validate(descriptor["result_schema"], json.loads(self.execute(argv).stdout))
            failure = json.loads(self.execute([name, "--json", "--unknown"], status=2).stdout)
            self.validate(descriptor["error_schema"], failure)
            self.assertEqual(failure["command"], name)
            self.assertFalse(failure["complete"])
        env = self.mode("env", "json", "environment-map")
        self.assertFalse(self.validator(env["input_schema"]).is_valid(["env", "--json", "operand"]))

    def test_runtime_maintenance_schema_selection_matches_real_results(self):
        operations = {row["name"]: row for row in self.inventory(AGENT)["operations"]
                      if row["kind"] == "runtime_operation"}
        requests = []
        for name, fields in (("store.history", {}), ("store.inspect", {}),
                             ("store.inspect", {"target_id": "synthetic-absent"}),
                             ("store.resolve", {"target_id": "synthetic-absent", "dry_run": True})):
            requests.append(dict(id="catalog-" + str(len(requests)), protocol_version=2,
                                 op=name, caps=["store:read", "store:manage"], **fields))
        documents = self.daemon(requests)
        expected = ["jash.agent-maintenance-history.v1", "jash.agent-store.v1",
                    "jash.agent-maintenance-inspection.v1", "jash.agent-maintenance-resolution.v1"]
        for request, document, schema_name in zip(requests, documents, expected):
            with self.subTest(operation=request["op"], target=request.get("target_id")):
                descriptor = operations[request["op"]]
                self.validate(descriptor["input_schema"], request)
                self.validate(descriptor["result_schema"], document)
                self.assertEqual(document["schema"], schema_name)
                if request["op"] == "store.inspect":
                    matching = [variant for variant in descriptor["result_variants"]
                                if self.validator(variant["when"]).is_valid(request)]
                    self.assertEqual(len(matching), 1)
                    self.validate(matching[0]["result_schema"], document)
        self.assertFalse(documents[-1]["published"])
        self.assertFalse(documents[-1]["ok"])

    def test_runtime_admission_errors_are_not_claimed_as_operation_documents(self):
        operations = {row["name"]: row for row in self.inventory(AGENT)["operations"]
                      if row["kind"] == "runtime_operation"}
        bad = dict(id="invalid", protocol_version=2, op="store.history",
                   caps=["store:read"], wait_ms=1)
        good = dict(id="next", protocol_version=2, op="store.history", caps=["store:read"])
        error, document = self.daemon([bad, good])
        descriptor = operations["store.history"]
        self.assertFalse(self.validator(descriptor["input_schema"]).is_valid(bad))
        self.assertIn("error", error)
        self.assertFalse(self.validator(descriptor["result_schema"]).is_valid(error))
        self.assertIn("admission/protocol errors", descriptor["result_schema_scope"])
        self.validate(descriptor["result_schema"], document)


if __name__ == "__main__":
    unittest.main()
