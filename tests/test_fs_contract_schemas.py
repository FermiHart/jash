#!/usr/bin/env python3
"""Independent JSON Schema validation of actual filesystem and mesh results.

The binary and native semantic suites provide the behavioral oracles. This
gate additionally rejects missing fields, invented fields, incorrect units,
wrong scalar kinds and dishonest effect/callback projections. Dependency
absence is a failure, never a successful SKIP.
"""
import base64
import copy
import json
import os
import shlex
import subprocess
import unittest

from jsonschema import Draft202012Validator

import native_data_fixture as fixture


class FilesystemSchemas(fixture.NativeQueryCase):
    @classmethod
    def setUpClass(cls):
        cls.validators = {}
        for name in ("du-row", "grep", "diff", "tdiff", "tread", "awk", "stat-array",
                     "wc-array", "df-row", "mesh-query", "mesh-action", "glob",
                     "find", "file-page", "cat-metadata"):
            document = json.loads((fixture.ROOT / "schemas" / f"builtin-{name}-v1.schema.json").read_text())
            Draft202012Validator.check_schema(document)
            cls.validators[name] = Draft202012Validator(document)

    def valid(self, name, value):
        self.validators[name].validate(value)
        return value

    def invalid(self, name, value):
        self.assertTrue(list(self.validators[name].iter_errors(value)), (name, value))

    def document(self, schema, argv, rc=0, strict=False, caps=None):
        result = self.run_command(argv, strict=strict, caps=caps)
        self.assertEqual(result.returncode, rc, result.stderr)
        return self.valid(schema, json.loads(result.stdout))

    def lines(self, schema, argv, rc=0, strict=False):
        result = self.run_command(argv, strict=strict)
        self.assertEqual(result.returncode, rc, result.stderr)
        return [self.valid(schema, json.loads(line)) for line in result.stdout.splitlines() if line]

    def discovery_json_cases(self):
        payload = b"needle text\n"
        (self.work / "sample").write_bytes(payload)
        return (("glob", ["glob", "--json", "sample"]),
                ("find", ["find", "--json", "sample"]),
                ("cat-metadata", ["cat", "--json", "sample"]),
                ("file-page", ["cat", "--data", "sample"]))

    def test_discovery_json_budget_preserves_complete_documents(self):
        self.env["JASH_LLM_BUDGET"] = "1"
        for schema, argv in self.discovery_json_cases():
            for invocation in ("cli", "exec-json", "strict"):
                with self.subTest(command=argv, invocation=invocation):
                    if invocation == "cli":
                        result = subprocess.run([str(fixture.JASH), "-c", shlex.join(argv)],
                            cwd=self.work, env=self.env, capture_output=True, timeout=20)
                    else:
                        result = self.run_command(argv, strict=invocation == "strict")
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertNotIn(b"context-governor", result.stdout)
                    value = self.valid(schema, json.loads(result.stdout))
                    if schema == "glob":
                        self.assertEqual([row["path"] for row in value["matches"]], ["sample"])
                        self.assertTrue(value["complete"])
                    elif schema == "find":
                        self.assertEqual([row["path"] for row in value["items"]], ["sample"])
                        self.assertTrue(value["complete"])
                    elif schema == "cat-metadata":
                        self.assertEqual((value[0]["size_bytes"], value[0]["lines"]), (12, 1))
                    else:
                        self.assertEqual(base64.b64decode(value["data"]["data"], validate=True), b"needle text\n")
                        self.assertTrue(value["eof"])

    def test_discovery_json_broken_sinks_return_delivery_failure(self):
        self.env["JASH_LLM_BUDGET"] = "1"
        for _, argv in self.discovery_json_cases():
            request = json.dumps({"request_version": 2, "argv": argv, "cwd": str(self.work)})
            for invocation in (["-c", shlex.join(argv)], ["--exec-json", request]):
                for sink in ("full", "closed-pipe"):
                    with self.subTest(command=argv, invocation=invocation[0], sink=sink):
                        if sink == "full":
                            descriptor = os.open("/dev/full", os.O_WRONLY)
                        else:
                            read_end, descriptor = os.pipe()
                            os.close(read_end)
                        try:
                            result = subprocess.run([str(fixture.JASH), *invocation],
                                cwd=self.work, env=self.env, stdout=descriptor,
                                stderr=subprocess.PIPE, timeout=20)
                        finally:
                            os.close(descriptor)
                        self.assertEqual(result.returncode, 125, result.stderr)

    def test_discovery_json_short_writes_and_eintr_keep_all_bytes(self):
        source = self.root / "json_write_fault.c"
        source.write_text(r'''#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <unistd.h>
ssize_t write(int fd, const void* bytes, size_t count) {
    ssize_t (*next)(int, const void*, size_t) = dlsym(RTLD_NEXT, "write");
    static int interrupted;
    if (fd == STDOUT_FILENO && count) {
        if (!interrupted++) { errno = EINTR; return -1; }
        if (count > 3) count = 3;
    }
    return next(fd, bytes, count);
}
''')
        library = self.root / "json_write_fault.so"
        compiled = subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                                   str(source), "-o", str(library), "-ldl"],
                                  capture_output=True, timeout=20)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        for schema, argv in self.discovery_json_cases():
            control = self.run_command(argv)
            self.assertEqual(control.returncode, 0, control.stderr)
            expected = self.valid(schema, json.loads(control.stdout))
            with self.subTest(command=argv):
                injected = dict(self.env, LD_PRELOAD=str(library), JASH_LLM_BUDGET="1")
                request = json.dumps({"request_version": 2, "argv": argv, "cwd": str(self.work)})
                result = subprocess.run([str(fixture.JASH), "--exec-json", request],
                    cwd=self.work, env=injected, capture_output=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.valid(schema, json.loads(result.stdout)), expected)

    def memo_command(self, argv, *, env=None):
        # A compound command enters the real evaluator with SIGIL=0; this
        # avoids making the memo contract depend on an installed module.
        return subprocess.run([str(fixture.JASH), "-c", "true; " + shlex.join(argv)],
            cwd=self.work, env=env or self.env, capture_output=True, timeout=20)

    def memo_environment(self, observer, budget):
        env = dict(self.env, JASH_LLM_BUDGET=str(budget))
        for name in ("JASH_MEMO", "JASH_SESSION", "CLAUDE_CODE_SESSION_ID"):
            env.pop(name, None)
        if observer == "explicit":
            env.update(JASH_MEMO="1", JASH_SESSION=f"memo-explicit-{budget}")
        elif observer == "session":
            env["JASH_SESSION"] = f"memo-default-{budget}"
        else:
            env["CLAUDE_CODE_SESSION_ID"] = f"memo-claude-{budget}"
        return env

    def test_explicit_json_never_becomes_memo_prose_or_delta(self):
        sample = self.work / "sample"
        sample.write_bytes(b"needle text\n")
        cases = ((["cat", "--json", "sample"], False),
                 (["cat", "--data", "sample"], False),
                 (["stat", "--json", "sample"], False),
                 (["stat", "--json-report", "sample"], False),
                 (["wc", "--json", "sample"], False),
                 (["wc", "--json-report", "sample"], False),
                 (["grep", "--json", "needle", "sample"], False),
                 (["grep", "--json", "-c", "needle", "sample"], True),
                 (["grep", "-C", "--json", "needle", "sample"], True),
                 (["diff", "--json", "sample", "sample"], False),
                 (["ls", "--json", "sample"], True))
        for observer in ("explicit", "session", "claude"):
            for budget in (0, 1):
                env = self.memo_environment(observer, budget)
                for argv, measured_budget in cases:
                    for attempt in range(2):
                        with self.subTest(observer=observer, budget=budget, argv=argv, attempt=attempt):
                            result = self.memo_command(argv, env=env)
                            self.assertEqual(result.returncode, 75 if budget and measured_budget else 0, result.stderr)
                            self.assertNotIn(b"[jash memo", result.stdout)
                            self.assertNotIn(b"context-governor", result.stdout)
                            value = json.loads(result.stdout)
                            if budget and measured_budget:
                                self.assertEqual(value["error"]["code"], "output_budget_exceeded")
        # A changed input must still produce a fresh observation, not memo's
        # human diff of the old and new JSON documents.
        env = self.memo_environment("session", 0)
        sample.write_bytes(b"changed\n")
        result = self.memo_command(["stat", "--json", "sample"], env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)[0]["size"], 8)

    def test_explicit_native_streams_never_become_memo_prose(self):
        (self.work / "sample").write_bytes(b"needle text\n")
        cases = [("cat", "--data-v2", ["sample"], b"JTV2")]
        for command, operands in (("stat", ["sample"]), ("wc", ["sample"]),
                                  ("grep", ["needle", "sample"]),
                                  ("diff", ["sample", "sample"]), ("ls", ["sample"])):
            for selector in ("--data", "--data-v2"):
                cases.append((command, selector, operands,
                              b"JTV1" if command == "ls" and selector == "--data" else b"JTV2"))
        for budget in (0, 1):
            env = self.memo_environment("session", budget)
            for command, selector, operands, magic in cases:
                for attempt in range(2):
                    with self.subTest(command=command, selector=selector, budget=budget, attempt=attempt):
                        result = self.memo_command([command, selector, *operands], env=env)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        parsed = list(fixture.frames(result.stdout, magic))
                        self.assertEqual([tag for tag, _ in parsed[:2]], [b"D", b"G"])
                        self.assertEqual(parsed[-1][0], b"T")
                        self.assertEqual(parsed[-1][1][0], 0)

    def test_memo_preserves_human_output_and_literal_format_tokens(self):
        (self.work / "sample").write_bytes(b"needle text\n--json\n")
        (self.work / "--json").write_bytes(b"literal file\n")
        cases = (["cat", "sample"], ["cat", "--", "--json"],
                 ["cat", "sample", "--json"], ["ls", "--", "--json"],
                 ["stat", "-c", "--json", "sample"],
                 ["grep", "-e", "--json", "sample", "sample"])
        for observer in ("explicit", "session", "claude"):
            env = self.memo_environment(observer, 0)
            for argv in cases:
                with self.subTest(observer=observer, argv=argv):
                    first = self.memo_command(argv, env=env)
                    second = self.memo_command(argv, env=env)
                    self.assertEqual((first.returncode, second.returncode), (0, 0), first.stderr + second.stderr)
                    self.assertNotIn(b"[jash memo", first.stdout)
                    if argv[0] == "stat":
                        # This format delegates externally; nested dispatch
                        # invalidates the outer memo plan. Its oracle is the
                        # literal format value, not a cache hit it never stores.
                        self.assertEqual((first.stdout, second.stdout), (b"--json\n", b"--json\n"))
                    else:
                        self.assertIn(b"[jash memo] unchanged", second.stdout)

    def test_stat_wc_df_legacy_shapes_and_selected_units(self):
        raw = b"raw-\xff"
        path = os.fsencode(self.work) + b"/" + raw
        with open(path, "wb") as file:
            file.write(b"one two\n")
        for strict in (False, True):
            with self.subTest(strict=strict):
                stats = self.document("stat-array", ["stat", "--json", raw], strict=strict)
                self.assertEqual(stats[0]["size"], 8)
                self.assertEqual(base64.b64decode(stats[0]["name"]["data"]), raw)
                bad = copy.deepcopy(stats); bad[0]["mtime_nsec"] = 1000000000
                self.invalid("stat-array", bad)
                bad = copy.deepcopy(stats); bad[0]["inode"] = "not-a-number"
                self.invalid("stat-array", bad)
                counts = self.document("wc-array", ["wc", "--json", "-mL", raw], strict=strict)
                self.assertEqual((counts[0]["chars"], counts[0]["max_line_length"]), (8, 7))
                self.assertNotIn("bytes", counts[0])
                bad = copy.deepcopy(counts); bad[0]["bytes"] = 8
                self.invalid("wc-array", bad)
                for flags in (["--files0-from", "missing"], ["-Z"], ["--unknown"]):
                    refused = self.run_command(["wc", "--json", *flags], strict=strict)
                    self.assertEqual(refused.returncode, 2, refused.stderr)
                    self.assertEqual(json.loads(refused.stdout)["error"]["code"], "unsupported_option")
                disks = self.lines("df-row", ["df", "--json", "-i", "."], strict=strict)
                self.assertTrue(disks)
                bad = copy.deepcopy(disks[0]); bad["allocated"] = bad.pop("total_bytes")
                self.invalid("df-row", bad)

    def test_du_rows_failures_and_page_markers(self):
        (self.work / "one").write_bytes(b"a")
        (self.work / "two").write_bytes(b"bb")
        for strict in (False, True):
            rows = self.lines("du-row", ["du", "--json", "-a", "--total", "one", "two"], strict=strict)
            self.assertEqual(rows[-1]["name"], "total")
            self.assertEqual(rows[-1]["apparent_bytes"], 3)
            bad = copy.deepcopy(rows[-1]); bad["path"] = "total"
            self.invalid("du-row", bad)
            failed = self.lines("du-row", ["du", "--json", "missing"], rc=1, strict=strict)[0]
            self.assertIsNone(failed["allocated_bytes"])
            bad = copy.deepcopy(failed); bad["complete"] = True
            self.invalid("du-row", bad)
            page = self.lines("du-row", ["du", "--json", "--limit", "1", "one", "two"], strict=strict)
            self.assertEqual(page[-1]["error"], "page_limit")
            bad = copy.deepcopy(page[-1]); bad["next_offset"] = -1
            self.invalid("du-row", bad)

    def test_grep_matches_raw_bytes_caps_and_errors(self):
        (self.work / "long").write_bytes(b"x" * 5000 + b"\n")
        (self.work / "raw").write_bytes(b"a\xffb\n")
        for strict in (False, True):
            matches = self.document("grep", ["grep", "--json", "x+", "-E", "long"], strict=strict)
            self.assertEqual(matches[0]["span"], {"start": 0, "end": 5000})
            self.assertEqual(len(matches[0]["match"]), 4000)
            self.assertTrue(matches[0]["match_truncated"])
            bad = copy.deepcopy(matches); bad[0]["span"]["start"] = -1
            self.invalid("grep", bad)
            raw = self.document("grep", ["grep", "--json", "-F", b"\xff", "raw"], strict=strict)
            self.assertEqual(base64.b64decode(raw[0]["match"]["data"]), b"\xff")
            failure = self.document("grep", ["grep", "--json", "x", "missing"], rc=2, strict=strict)
            self.assertFalse(failure[0]["complete"])
            bad = copy.deepcopy(failure); bad[0]["complete"] = True
            self.invalid("grep", bad)
            self.assertEqual(self.document("grep", ["grep", "--json", "z", "long"], rc=1, strict=strict), [])

    def test_diff_and_tread_preserve_line_and_page_bytes(self):
        (self.work / "a").write_bytes(b"a\xff\nlast\n")
        (self.work / "b").write_bytes(b"b\xff\nlast")
        for strict in (False, True):
            difference = self.document("diff", ["diff", "--json", "a", "b"], rc=1, strict=strict)
            self.assertEqual(base64.b64decode(difference[0]["a_lines"][0]["data"]), b"a\xff")
            self.assertFalse(difference[-1]["b_final_newline"])
            bad = copy.deepcopy(difference); bad[0]["a_final_newline"] = "true"
            self.invalid("diff", bad)
            bad = copy.deepcopy(difference); bad[0]["op"] = "delete"; bad[0]["b"]["count"] = 1
            self.invalid("diff", bad)
            page = self.document("tread", ["tread", "--json", "--limit", "2", "a"], strict=strict)
            self.assertEqual(base64.b64decode(page["data"]["data"]), b"a\xff")
            self.assertFalse(page["eof"])
            bad = copy.deepcopy(page); bad["page_blake3"]["data"] = "AA=="
            self.invalid("tread", bad)
            bad = copy.deepcopy(page); bad["checkpoint"] = "invented"
            self.invalid("tread", bad)

    def test_checkpoint_projections_use_actual_historical_bytes(self):
        (self.work / "old").write_bytes(b"old\n")
        (self.work / "new").write_bytes(b"new")
        for command, schema, rc in (("tdiff --json current @base", "tdiff", 1),
                                    ("tread --json current @base", "tread", 0)):
            script = "cp old current >/dev/null; checkpoint base >/dev/null; cp new current >/dev/null; " + command
            completed = subprocess.run([str(fixture.JASH), "-c", script], cwd=self.work,
                                       env=dict(self.env, JASH_TIMELINE="1"), capture_output=True, timeout=20)
            self.assertEqual(completed.returncode, rc, completed.stderr)
            observed = self.valid(schema, json.loads(completed.stdout))
            if schema == "tdiff":
                self.assertEqual(observed[0]["checkpoint"], "base")
                self.assertIn("old", observed[0]["a_lines"])
                bad = copy.deepcopy(observed); del bad[0]["checkpoint"]
            else:
                self.assertTrue(observed["snapshot"])
                self.assertEqual(base64.b64decode(observed["data"]["data"]), b"old\n")
                bad = copy.deepcopy(observed); bad["checkpoint"] = None
            self.invalid(schema, bad)

    def test_awk_fields_aggregate_printf_pages_and_failed_aggregate(self):
        (self.work / "input").write_bytes(b"1 raw-\xff\n2 value\n")
        for strict in (False, True):
            fields = self.document("awk", ["awk", "--json", "{print $2}", "input"], strict=strict)
            self.assertEqual(base64.b64decode(fields[0]["fields"][0]["data"]), b"raw-\xff")
            result = self.document("awk", ["awk", "--json", "{sum += $1} END {print sum}", "input"], strict=strict)
            self.assertEqual(result, {"result": 3})
            self.document("awk", ["awk", "--json", 'BEGIN {printf "hello"}', "input"], strict=strict)
            page = self.document("awk", ["awk", "--json", "--limit", "1", "{print}", "input"], strict=strict)
            self.assertFalse(page["complete"])
            bad = copy.deepcopy(page); bad["numeric_model"] = "integer64"
            self.invalid("awk", bad)
            failure = self.document("awk", ["awk", "--json", "{sum += $1} END {print sum}", "missing"], rc=2, strict=strict)
            self.assertTrue(failure["errors"])
            self.assertFalse(any("result" in row for row in failure["results"]))
            bad = copy.deepcopy(failure); bad["complete"] = True
            self.invalid("awk", bad)

    def test_mesh_query_and_action_schemas_cover_real_store_operations(self):
        for strict in (False, True):
            ident = "strict" if strict else "general"
            def action(args, rc=0, caps=None):
                return self.document("mesh-action", ["mesh", "--json", *args], rc=rc,
                                     strict=strict, caps=caps or ["effect:mutate"])
            made = action(["register", "--id", ident, "--model", "model"])
            self.assertTrue(made["receipts"][-1]["applied"])
            bad = copy.deepcopy(made); bad["receipts"][-1]["transaction"] = "callback-invocation"
            self.invalid("mesh-action", bad)
            action(["heartbeat", "--id", ident, "--task", "something"])
            action(["init", "goal"])
            action(["add", "task", "--tag", "build"])
            action(["next", "--id", ident])
            action(["done", "1", "--id", "other"], rc=1)
            action(["done", "1", "--id", ident])
            action(["fail", "1", "--id", ident])
            action(["post", "message", "--id", ident])
            action(["claim", "claim-path", "--id", ident, "--ttl", "9007199254740993"])
            for args in (["workers"], ["board"], ["board", "--messages"], ["rep"], ["claims"],
                         ["workers", "--offset", "1", "--limit", "1"]):
                rows = self.document("mesh-query", ["mesh", "--json", *args], strict=strict)
                if args == ["claims"]:
                    self.assertEqual(rows[0]["ttl"], 9007199254740993)
                    self.assertIs(type(rows[0]["ttl"]), int)
                    bad = copy.deepcopy(rows); bad[0]["ttl"] = "9007199254740993"
                    self.invalid("mesh-query", bad)
            action(["unclaim", "claim-path", "--id", ident])
            action(["sweep"])
            self.assertIsNone(action(["next", "--id", ident], rc=1))


if __name__ == "__main__":
    unittest.main()
