#!/usr/bin/env python3
"""W04: native df quantities, live pagination and output boundaries."""
import json
import os
import unittest
import subprocess
import tempfile
import shlex
import test_agent_mutations_json as fixture


class Queries(unittest.TestCase):
    setUp = fixture.Mutations.setUp
    runj = fixture.Mutations.runj

    def query(self, args, rc=0):
        p = self.runj("df --json-report " + args)
        self.assertEqual(p.returncode, rc, p.stderr)
        d = json.loads(p.stdout)
        self.assertEqual(d["schema"], "jash.query.df.v1")
        self.assertEqual(d["exit_code"], rc)
        self.assertEqual(d["count"], len(d["items"]))
        return d

    def test_native_inode_values(self):
        item = self.query("-i .")["items"][0]
        st = os.statvfs(self.work)
        self.assertEqual(item["inodes"]["total"], st.f_files)
        self.assertGreaterEqual(item["inodes"]["used"], 0)
        self.assertEqual(item["total_bytes"], st.f_blocks * st.f_frsize)

    def test_live_pages_and_total(self):
        whole = self.query("--total . . .")
        self.assertEqual(whole["total"]["total_bytes"], sum(i["total_bytes"] for i in whole["items"]))
        page = self.query("--offset 1 --limit 1 . . .")
        self.assertEqual(page["count"], 1)
        self.assertEqual(page["next_offset"], 2)
        self.assertEqual(page["consistency"], "live")
        self.assertIsNone(self.query("--offset 3 --limit 1 . . .")["next_offset"])

    def test_missing_operand_is_partial(self):
        d = self.query("missing .", rc=1)
        self.assertEqual(d["count"], 1)

    def test_writer_budget_and_failed_sink(self):
        self.env["JASH_LLM_BUDGET"] = "64"
        self.query("-i .")
        p = self.runj("df --json-report . > /dev/full")
        self.assertEqual(p.returncode, 125, p.stderr)

    def test_jsonl_inode_total_and_invalid_pagination(self):
        p = self.runj("df --json -i --total .")
        self.assertEqual(p.returncode, 0, p.stderr)
        rows = [json.loads(line) for line in p.stdout.splitlines()]
        self.assertIn("inodes", rows[0])
        self.assertEqual(rows[-1]["device"], "total")
        for args in ("--limit 0", "--offset -1", "--limit 999999999999999999999999"):
            self.assertEqual(self.runj("df --json-report " + args).returncode, 2)

    def test_explicit_baseline_and_delta_arithmetic(self):
        baseline = self.query("--baseline baseline.bin .")
        data = (self.work / "baseline.bin").read_bytes()
        delta = self.query("--delta baseline.bin .")
        before, after = baseline["items"][0], delta["items"][0]
        for key in ("total_bytes", "used_bytes", "avail_bytes"):
            self.assertEqual(int(after["delta"][key]), int(after[key]) - int(before[key]))
        self.assertEqual((self.work / "baseline.bin").read_bytes(), data)
        self.assertEqual((self.work / "baseline.bin").stat().st_mode & 0o777, 0o600)
        self.query("--baseline baseline.bin .", rc=1)
        self.assertEqual((self.work / "baseline.bin").read_bytes(), data)

    def test_baselines_are_not_shared_and_corruption_is_rejected(self):
        self.query("--baseline first .")
        self.query("--baseline second .")
        (self.work / "first").write_bytes(b"corrupt")
        bad = self.query("--delta first .", rc=1)
        self.assertIsNone(bad["items"][0]["delta"])
        self.assertEqual(bad["baseline_error"], "invalid_baseline")
        self.assertIsNotNone(self.query("--delta second .")["items"][0]["delta"])

    def test_failed_output_prevents_baseline_creation(self):
        p = self.runj("df --json-report --baseline new . > /dev/full")
        self.assertEqual(p.returncode, 125)
        self.assertFalse((self.work / "new").exists())

    def test_strict_profile_requires_mutation_capability_for_baseline(self):
        agent = os.environ.get("JASH_AGENTD_BIN", str(fixture.ROOT / "build/jash-agentd"))
        request = {"id": "df-baseline-denied", "argv": ["df", "--json-report", "--baseline", "new", "."],
                   "cwd": str(self.work), "caps": ["effect:read"]}
        p = subprocess.run([str(agent)], input=(json.dumps(request) + "\n").encode(),
                           cwd=self.work, env=self.env, capture_output=True, timeout=15)
        result = json.loads(p.stdout.splitlines()[-1])
        self.assertNotEqual(result["rc"], 0, result)
        self.assertFalse((self.work / "new").exists())
        request.update(id="df-baseline-allowed", caps=["effect:mutate"])
        p = subprocess.run([str(agent)], input=(json.dumps(request) + "\n").encode(),
                           cwd=self.work, env=self.env, capture_output=True, timeout=15)
        self.assertEqual(json.loads(p.stdout.splitlines()[-1])["rc"], 0)
        self.assertTrue((self.work / "new").exists())

    def test_baseline_publication_uses_owned_descriptor(self):
        with tempfile.TemporaryDirectory(prefix="jash-df-fault-") as temp:
            hook = os.path.join(temp, "faults.so")
            subprocess.run(["cc", "-shared", "-fPIC", str(fixture.ROOT / "tests/mutation_metadata_faults.c"), "-ldl", "-o", hook],
                           check=True, capture_output=True, timeout=30)
            self.env.update(LD_PRELOAD=hook, JASH_TEST_MUTATION_FAULT="df_temp_swap")
            self.query("--baseline baseline .", rc=1)
            self.assertTrue((self.work / "owned-snapshot").exists())
            self.assertTrue(any(p.read_bytes() == b"raced" for p in self.work.glob(".jash-df-*.tmp")))
            self.env.pop("LD_PRELOAD")
            self.assertIsNotNone(self.query("--delta baseline .")["items"][0]["delta"])

    def test_stat_byte_paths_and_nanosecond_metadata(self):
        name = os.fsdecode(b"file-\xff")
        path = self.work / name; path.write_text("data")
        ns = 1700000000123456789; os.utime(path, ns=(ns, ns))
        p = self.runj(shlex.join(["stat", "--json", name]))
        self.assertEqual(p.returncode, 0, p.stderr)
        item = json.loads(p.stdout)[0]
        self.assertEqual(fixture.path_bytes(item["name"]), os.fsencode(name))
        self.assertEqual(int(item["mtime"]) * 10**9 + item["mtime_nsec"], ns)
        self.assertEqual(int(item["inode"]), path.stat().st_ino)

    def test_stat_pagination_and_errors_are_explicit(self):
        (self.work / "file").write_text("data")
        p = self.runj("stat --json-report --limit 2 missing file missing2")
        self.assertEqual(p.returncode, 1)
        report = json.loads(p.stdout)
        self.assertEqual(report["schema"], "jash.query.stat.v1")
        self.assertEqual(report["count"], 2)
        self.assertEqual(report["failed"], 1)
        self.assertEqual(report["next_offset"], 2)
        self.assertEqual(report["items"][0]["error"], "stat_failed")
        self.assertEqual(report["items"][1]["size"], 4)

    def test_stat_links_budget_and_output_failure(self):
        target = os.fsdecode(b"absent-\xfe"); (self.work / "link").symlink_to(target)
        self.env.update(JASH_LLM_BUDGET="32", JASH_MEMO="1")
        result = self.runj("true; stat --json-report link")
        item = json.loads(result.stdout)["items"][0]
        self.assertEqual(fixture.path_bytes(item["target"]), os.fsencode(target))
        self.assertEqual(self.runj("stat --json link > /dev/full").returncode, 125)

    def test_stat_format_value_is_not_json_switch(self):
        (self.work / "file").write_text("data")
        p = self.runj("stat -c --json file")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout, b"--json\n")

    def test_wc_reports_binary_counts_and_page_totals(self):
        (self.work / "first").write_bytes(b"one two\n\x00three\tlast")
        (self.work / "second").write_bytes(b"end\n")
        p = self.runj("wc --json-report --limit 2 first second missing")
        self.assertEqual(p.returncode, 0, p.stderr)
        d = json.loads(p.stdout)
        self.assertEqual(d["schema"], "jash.query.wc.v1")
        self.assertEqual(d["items"][0]["words"], 4)
        self.assertEqual(d["total"]["lines"], 2)
        self.assertEqual(d["total"]["bytes"], 23)
        self.assertEqual(d["next_offset"], 2)
        p = self.runj("wc --json-report --offset 2 first second missing")
        self.assertEqual(p.returncode, 1)
        d = json.loads(p.stdout)
        self.assertEqual(d["count"], 1)
        self.assertEqual(d["items"][0]["error"], "open_failed")
        self.assertFalse(d["total"]["complete"])

    def test_wc_bytes_paths_and_checked_legacy_array(self):
        name = os.fsdecode(b"raw-\xff"); (self.work / name).write_bytes("é\n".encode())
        for mode in ("--json", "--json-report"):
            self.env["JASH_LLM_BUDGET"] = "32"
            p = self.runj(shlex.join(["wc", mode, name]))
            self.assertEqual(p.returncode, 0, p.stderr)
            d = json.loads(p.stdout); item = d["items"][0] if mode == "--json-report" else d[0]
            self.assertEqual(fixture.path_bytes(item["name"]), os.fsencode(name))
            self.assertEqual(item["bytes"], 3)
        p = self.runj(shlex.join(["wc", "--json", "-m", name]))
        self.assertEqual(json.loads(p.stdout)[0]["chars"], 2)

    def test_wc_stdin_read_error_and_failed_output(self):
        p = self.runj("wc --json-report", input=b"a\nb\n")
        self.assertEqual(p.returncode, 0)
        self.assertEqual(json.loads(p.stdout)["total"]["lines"], 2)
        p = self.runj("wc --json-report .")
        self.assertEqual(p.returncode, 1)
        self.assertEqual(json.loads(p.stdout)["items"][0]["error"], "read_failed")
        for mode in ("--json", "--json-report"):
            # A FIFO would hang if opening the input preceded the output check.
            fifo = self.work / mode; os.mkfifo(fifo)
            self.assertEqual(self.runj(f"wc {mode} -- {mode} > /dev/full", timeout=5).returncode, 125)

    def test_wc_invalid_pagination_and_options_precede_input(self):
        os.mkfifo(self.work / "fifo")
        for args in ("--limit 0", "--limit", "--offset -1", "--offset 18446744073709551616", "-m", "--files0-from"):
            self.assertEqual(self.runj("wc --json-report " + args + " fifo", timeout=5).returncode, 2)

    def test_wc_report_is_read_capability_in_strict_profile(self):
        (self.work / "file").write_bytes(b"read only\n")
        agent = os.environ.get("JASH_AGENTD_BIN", str(fixture.ROOT / "build/jash-agentd"))
        request = {"id": "wc-read", "argv": ["wc", "--json-report", "file"], "cwd": str(self.work), "caps": ["effect:read"]}
        p = subprocess.run([agent], input=(json.dumps(request) + "\n").encode(), cwd=self.work,
                           env=self.env, capture_output=True, timeout=15)
        result = json.loads(p.stdout.splitlines()[-1])
        self.assertEqual(result["rc"], 0, result)
        self.assertEqual(json.loads(result["stdout"])["total"]["bytes"], 10)

    def test_stat_json_and_external_fallback_have_distinct_effect_caps(self):
        path = self.work / "file"
        path.write_text("four")
        agent = os.environ.get("JASH_AGENTD_BIN", str(fixture.ROOT / "build/jash-agentd"))

        def request(identifier, argv, caps):
            body = {"id": identifier, "argv": argv, "cwd": str(self.work), "caps": caps}
            p = subprocess.run([agent], input=(json.dumps(body) + "\n").encode(),
                               cwd=self.work, env=self.env, capture_output=True, timeout=15)
            self.assertEqual(p.returncode, 0, p.stderr)
            return json.loads(p.stdout.splitlines()[-1])

        native = request("stat-native-read", ["stat", "--json-report", "file"], ["effect:read"])
        self.assertEqual(native["rc"], 0, native)
        self.assertEqual(json.loads(native["stdout"])["schema"], "jash.query.stat.v1")

        denied = request("stat-external-denied", ["stat", "-c", "%s", "file"], ["effect:read"])
        self.assertEqual(denied["rc"], 125, denied)
        self.assertEqual(denied["error"], "capability denied", denied)

        external = request("stat-external-allowed", ["stat", "-c", "%s", "file"], ["effect:exec"])
        self.assertEqual(external["rc"], 0, external)
        self.assertEqual(external["stdout"], "4\n")


if __name__ == "__main__": unittest.main()
