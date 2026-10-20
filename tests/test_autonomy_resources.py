#!/usr/bin/env python3
"""Resource contract unit tests; real delegated-cgroup tests are separate below.

Run without rebuilding: python3 -B tests/test_autonomy_resources.py
"""
from argparse import Namespace
from contextlib import contextmanager
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools import jash_autonomy as workflow


LIMITS = dict(memory_max_bytes=128 * 1024 * 1024, pids_max=64,
              cpu_quota_us=50000, cpu_period_us=100000, wall_timeout_ms=2000)


def contract(resources=True):
    result = dict(schema="jash.autonomy.v1", campaign_id="1" * 32,
                  repo="/tmp/unit-source", target="target",
                  baseline=[dict(path="target", sha256=workflow.sha(b"base"),
                                 mode=0o644, bytes=4)],
                  generators=[dict(id="c0", argv=["/bin/true"])],
                  check=["/bin/true"], policy=dict(workflow.POLICY), timeout_ms=2000)
    if resources:
        result.update(schema="jash.autonomy.v2", resources=dict(LIMITS),
                      policy={**workflow.POLICY, "resources": "cgroup-v2"})
    return result


def response(terminal=True):
    row = dict(resources=dict(LIMITS), resource_backend="cgroup-v2", resource_enforced=True)
    if terminal:
        row.update(state="done", rc=0, signal=0, terminal=True, quiescent=True,
                   cancel_requested=False, output_limit=False, quiescence_forced=False,
                    kill_sent=False, resource_timed_out=False, resource_violation=False,
                    resource_empty=True, resource_memory_events=0, resource_pids_events=0,
                    descendants_forced=False)
    return row


def unit_sandbox(work, payload, writable):
    return ["/usr/bin/bwrap", "--unshare-all", "--unshare-user", "--unshare-cgroup",
            "--disable-userns", "--assert-userns-disabled", "--die-with-parent",
            "--new-session", "--as-pid-1", "--cap-drop", "ALL", "--clearenv",
            "--setenv", "PATH", "/usr/bin:/bin", "--setenv", "HOME", "/tmp",
            "--setenv", "LANG", "C", "--ro-bind", "/usr", "/usr", "--proc", "/proc",
            "--dev", "/dev", "--tmpfs", "/tmp", "--bind" if writable else "--ro-bind",
            str(work), "/work", "--chdir", "/work", "--", *payload]


def unit_capture(directory, campaign, cid="c0", phase="generate", spec=None):
    """Synthetic response files, NOT signed evidence. Mock libjef only in unit tests."""
    directory.mkdir(parents=True)
    (directory / "agent_receipts").mkdir()
    (directory / "overflow").mkdir()
    work = "/tmp/unit-workspace"
    payload = campaign["check"] if phase == "check" else campaign["generators"][int(cid[1])]["argv"]
    spec = spec or dict(schema=campaign["schema"], campaign=campaign, candidate=cid, phase=phase,
                        workspace=work, argv=unit_sandbox(work, payload, phase == "generate"))
    raw = workflow.wire(spec)
    ident = cid + "-" + phase
    request = dict(id=ident + "-start", protocol_version=2, op="job.start", argv=spec["argv"],
                   background=True, execution_spec_sha256="sha256:" + workflow.sha(raw),
                   caps=["effect:exec", "job:create"])
    start, terminal = response(False), response()
    if "resources" in campaign:
        request["resources"] = campaign["resources"]
        start["resources"] = terminal["resources"] = campaign["resources"]
    else:
        start, terminal = ({k: v for k, v in row.items()
                            if k != "resources" and not k.startswith("resource_")}
                           for row in (start, terminal))
    start.update(job_id="unit-job", receipt_ref="a.job-start.dsse")
    terminal.update(job_id="unit-job", receipt_ref="b.job-terminal.dsse",
                    stdout_ref="unit-stream", stderr_ref="unit-stream")
    (directory / "spec.json").write_bytes(raw)
    for label, req, res in (("start", request, start), ("terminal",
            dict(id=ident + "-wait", protocol_version=2, op="job.wait", job_id="unit-job",
                 wait_ms=campaign["timeout_ms"], caps=["job:read"]), terminal)):
        (directory / (label + "-request.json")).write_bytes(workflow.wire(req))
        (directory / (label + "-response.json")).write_bytes(workflow.wire(res))
        (directory / "agent_receipts" / res["receipt_ref"]).write_bytes(
            ("UNSIGNED UNIT FIXTURE " + ident + label).encode())
    (directory / "overflow/unit-stream").write_bytes(b"")
    return terminal


class ResourceUnitTests(unittest.TestCase):
    """Synthetic observations test Python policy, not actual quota enforcement."""

    def test_v1_stays_default_and_rejects_resource_contract_fields(self):
        self.assertEqual(workflow.SCHEMA, "jash.autonomy.v1")
        self.assertNotIn("resources", workflow.POLICY)
        workflow.validate_contract(contract(False))
        bad = contract(False)
        bad["resources"] = dict(LIMITS)
        with self.assertRaises(ValueError):
            workflow.validate_contract(bad)

    def test_v2_strict_contract_and_profile(self):
        workflow.validate_contract(contract())
        for key in ("resources", "schema", "policy", "check", "generators"):
            bad = contract()
            del bad[key]
            with self.subTest(missing=key), self.assertRaises(ValueError):
                workflow.validate_contract(bad)
        for changed in ({"unknown": 1}, {"schema": "jash.autonomy.v3"},
                        {"policy": workflow.POLICY}, {"resources": None}):
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                workflow.validate_contract({**contract(), **changed})

    def test_resource_types_bounds_and_exact_keys(self):
        workflow.validate_resources(LIMITS)
        ranges = dict(memory_max_bytes=(16 * 1024 * 1024, 1024 ** 4),
                      pids_max=(1, 4096), cpu_period_us=(1000, 1000000),
                      cpu_quota_us=(1000, LIMITS["cpu_period_us"]), wall_timeout_ms=(1, 30000))
        for name, (low, high) in ranges.items():
            for value in (True, False, None, "1000", 1000.0, 0, -1, low - 1, high + 1):
                with self.subTest(name=name, value=value), self.assertRaises(ValueError):
                    workflow.validate_resources({**LIMITS, name: value})
            for value in (low, high):
                valid = {**LIMITS, name: value}
                if name == "cpu_period_us":
                    valid["cpu_quota_us"] = value
                workflow.validate_resources(valid)
            bad = dict(LIMITS)
            del bad[name]
            with self.assertRaises(ValueError):
                workflow.validate_resources(bad)
        for bad in (None, [], {}, {**LIMITS, "root": "/tmp"},
                    {**LIMITS, "memory_swap_max_bytes": 0}, {**LIMITS, "memory_oom_group": 1}):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                workflow.validate_resources(bad)

    def test_bad_opt_in_refuses_before_output_or_daemon_effect(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "repo").mkdir()
            (root / "link").symlink_to(root, target_is_directory=True)
            (root / "file").write_text("not a directory")
            args = Namespace(repo=str(root / "repo"), bundle=str(root / "bundle"),
                             runtime_home=str(root / "home"), timeout_ms=2000)
            cases = [(None, workflow.wire(LIMITS).decode()), ("relative", None),
                     (str(root / "missing"), None), (str(root / "link"), None),
                     (str(root / "file"), None), (str(root) + "/../" + root.name, None),
                     (str(root), '{"pids_max":true}'), (str(root), '{"a":1,"a":2}')]
            for cgroup_root, resources in cases:
                args.cgroup_root, args.resources = cgroup_root, resources
                with self.subTest(root=cgroup_root, resources=resources), \
                     mock.patch.object(workflow, "snapshot") as snapshot, \
                     mock.patch.object(workflow, "Daemon") as daemon:
                    with self.assertRaises((ValueError, OSError)):
                        workflow.run(args)
                    snapshot.assert_not_called()
                    daemon.assert_not_called()
                    self.assertFalse((root / "home").exists())
                    self.assertFalse((root / "bundle").exists())

    def test_resource_eligibility_never_downgrades_missing_fields(self):
        req = {"id": "c0-check-wait"}
        good = response()
        self.assertTrue(workflow.job_passed(good, req, "c0-check", LIMITS))
        for key in [k for k in good if k in ("resources", "descendants_forced") or k.startswith("resource_")]:
            bad = dict(good)
            del bad[key]
            with self.subTest(missing=key):
                self.assertFalse(workflow.job_passed(bad, req, "c0-check", LIMITS))
        for key, value in (("resource_timed_out", True), ("resource_violation", True),
                           ("resource_empty", False), ("resource_enforced", False),
                           ("resource_backend", "none"), ("resource_memory_events", 1),
                           ("resource_pids_events", 1), ("resource_pids_events", True),
                            ("resource_memory_events", -1), ("resource_empty", 1),
                            ("descendants_forced", True), ("descendants_forced", 0)):
            with self.subTest(key=key, value=value):
                self.assertFalse(workflow.job_passed({**good, key: value}, req, "c0-check", LIMITS))
        legacy = {k: v for k, v in good.items() if k != "resources" and not k.startswith("resource_")}
        self.assertTrue(workflow.job_passed(legacy, req, "c0-check"))

    def test_daemon_environment_is_trusted_config_only(self):
        with tempfile.TemporaryDirectory() as temp, \
             mock.patch.dict(os.environ, {"JASH_AGENT_CGROUP_ROOT": "/untrusted", "SECRET": "secret"}), \
             mock.patch.object(workflow.subprocess, "Popen") as popen, \
             mock.patch.object(workflow.Daemon, "line", return_value=workflow.wire(
                 {"runtime": "jash-agentd", "receipts": {"pubkey": "1" * 64}})):
            for root in (None, Path(temp)):
                daemon = workflow.Daemon(sys.executable, Path(temp), trusted_cgroup_root=root)
                env = popen.call_args.kwargs["env"]
                self.assertEqual(env, {**workflow.ENV, "HOME": temp,
                                      **({"JASH_AGENT_CGROUP_ROOT": temp} if root else {})})
                daemon.close()
            popen.reset_mock()
            with self.assertRaises(ValueError):
                workflow.Daemon(sys.executable, Path(temp), trusted_cgroup_root="relative")
            popen.assert_not_called()

    def test_verify_phase_requires_resource_fields_and_request_binding(self):
        with tempfile.TemporaryDirectory() as temp, \
             mock.patch.object(workflow.subprocess, "run", return_value=Namespace(returncode=0)):
            directory = Path(temp) / "generate"
            unit_capture(directory, contract())
            verify = lambda: workflow.verify_phase(directory, "1" * 64, "unit-verifier",
                                                    contract(), "c0", "generate")
            self.assertTrue(verify()[1])
            for label in ("start", "terminal"):
                path = directory / (label + "-response.json")
                original = path.read_bytes()
                row = workflow.decode(original)
                mutations = [{k: v for k, v in row.items() if k != removed}
                             for removed in row if removed == "resources" or removed.startswith("resource_")]
                mutations += [{**row, "resources": {**LIMITS, "pids_max": True}},
                              {**row, "resources": {**LIMITS, "pids_max": 63}},
                              {**row, "resource_backend": "none"}, {**row, "resource_enforced": 1}]
                if label == "terminal":
                    mutations += [{**row, k: v} for k, v in
                                  (("resource_timed_out", 0), ("resource_violation", 0),
                                   ("resource_empty", 1), ("resource_memory_events", True),
                                   ("resource_pids_events", -1))]
                for changed in mutations:
                    with self.subTest(label=label, changed=changed), self.assertRaises(ValueError):
                        path.write_bytes(workflow.wire(changed))
                        verify()
                path.write_bytes(original)
            path = directory / "start-request.json"
            original = path.read_bytes()
            row = workflow.decode(original)
            for resources in (None, {**LIMITS, "pids_max": 63}, {**LIMITS, "pids_max": True},
                              {**LIMITS, "cgroup_root": "/elsewhere"}):
                changed = {**row, "resources": resources}
                if resources is None:
                    del changed["resources"]
                with self.subTest(resources=resources), self.assertRaises(ValueError):
                    path.write_bytes(workflow.wire(changed))
                    verify()
            path.write_bytes(original)
            terminal = directory / "terminal-response.json"
            original = workflow.decode(terminal.read_bytes())
            for changes in ({"resource_violation": True, "resource_pids_events": 1},
                            {"resource_memory_events": 1}, {"resource_timed_out": True},
                            {"resource_empty": False}):
                terminal.write_bytes(workflow.wire({**original, **changes}))
                self.assertFalse(verify()[1], "valid failure observations are not eligible")

    def test_legacy_phase_accepts_opaque_signed_response_but_not_resource_request(self):
        with tempfile.TemporaryDirectory() as temp, \
             mock.patch.object(workflow.subprocess, "run", return_value=Namespace(returncode=0)):
            directory = Path(temp) / "generate"
            unit_capture(directory, contract(False))
            verify = lambda: workflow.verify_phase(directory, "1" * 64, "unit-verifier",
                                                    contract(False), "c0", "generate")
            self.assertTrue(verify()[1])
            path = directory / "terminal-response.json"
            row = workflow.decode(path.read_bytes())
            path.write_bytes(workflow.wire({**row, "resources": {"opaque": True}}))
            self.assertTrue(verify()[1])
            path = directory / "start-request.json"
            row = workflow.decode(path.read_bytes())
            path.write_bytes(workflow.wire({**row, "resources": LIMITS}))
            with self.assertRaisesRegex(ValueError, "invalid schema fields"):
                verify()

    def test_daemon_job_sends_campaign_profile_only_in_v2(self):
        for mode in (False, True):
            with self.subTest(resources=mode), tempfile.TemporaryDirectory() as temp:
                daemon = object.__new__(workflow.Daemon)
                campaign = contract(mode)
                spec = dict(schema=campaign["schema"], campaign=campaign, candidate="c0",
                            phase="generate", workspace="/tmp/unit-workspace", argv=["/bin/true"])
                def exchange(directory, label, body, *args):
                    row = ({"job_id": "unit", **(response(False) if mode else {})}
                           if label == "start" else response())
                    for kind, value in (("request", body), ("response", row)):
                        (directory / (label + "-" + kind + ".json")).write_bytes(workflow.wire(value))
                    return row
                daemon.exchange = mock.Mock(side_effect=exchange)
                daemon.job(Path(temp) / "phase", spec)
                request = daemon.exchange.call_args_list[0].args[2]
                self.assertEqual(request.get("resources"), LIMITS if mode else None)
                self.assertNotIn("cgroup_root", request)
                self.assertEqual(request["execution_spec_sha256"], "sha256:" + workflow.sha(workflow.wire(spec)))

    def test_run_defaults_and_total_campaign_plan_are_pinned_across_all_phases(self):
        for mode in (None, "default", "override"):
            with self.subTest(resources=mode), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                repo = root / "repo"
                repo.mkdir()
                args = Namespace(repo=str(repo), bundle=str(root / "bundle"),
                                 runtime_home=str(root / "home"), target="target", timeout_ms=1234,
                                 generate=['["/bin/true"]', '["/bin/false"]'], check='["/bin/true"]',
                                 agentd="unit-agent", verifier="unit-verifier", resources=None,
                                 cgroup_root=str(root) if mode else None)
                profile = {**LIMITS, "wall_timeout_ms": args.timeout_ms} if mode else None
                if mode == "override":
                    profile = {**LIMITS, "pids_max": 32, "wall_timeout_ms": 1500}
                    args.resources = workflow.wire(profile).decode()
                if mode is None:
                    # Shipped callers construct Namespace without the new flags.
                    del args.resources, args.cgroup_root
                daemon = mock.Mock(key="1" * 64)
                daemon.job.side_effect = lambda directory, spec: unit_capture(
                    directory, spec["campaign"], spec["candidate"], spec["phase"], spec)
                with mock.patch.object(workflow, "snapshot", return_value=(contract(False)["baseline"],
                                                                         {"target": b"base"})), \
                     mock.patch.object(workflow, "current", return_value=True), \
                     mock.patch.object(workflow, "build_argv", side_effect=unit_sandbox), \
                     mock.patch.object(workflow, "Daemon", return_value=daemon) as constructor, \
                     mock.patch.object(workflow.subprocess, "run", return_value=Namespace(returncode=0)):
                    result = workflow.run(args)
                    self.assertEqual(result["schema"], "jash.autonomy.v2" if mode else workflow.SCHEMA)
                    self.assertEqual(result["campaign"].get("resources"), profile)
                    self.assertEqual(result["not_verified"], workflow.NOT_VERIFIED)
                    self.assertEqual(len(result["evidence"]), 2)
                    self.assertEqual(constructor.call_args.kwargs["trusted_cgroup_root"], root if mode else None)
                    specs = [call.args[1] for call in daemon.job.call_args_list]
                    self.assertEqual(len(specs), 4)
                    self.assertTrue(all(s["campaign"] == result["campaign"] for s in specs))
                    if not mode:
                        continue
                    phase = root / "bundle/c1/check/spec.json"
                    original = phase.read_bytes()
                    for mutation in ("resources", "plan", "schema", "unknown"):
                        changed = workflow.decode(original)
                        if mutation == "resources":
                            changed["campaign"]["resources"]["pids_max"] -= 1
                        elif mutation == "plan":
                            changed["campaign"]["generators"][0]["argv"] = ["/bin/false"]
                        elif mutation == "schema":
                            changed["schema"] = workflow.SCHEMA
                        else:
                            changed["campaign"]["unknown"] = 1
                        phase.write_bytes(workflow.wire(changed))
                        with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                            workflow.verify_bundle(args.bundle, daemon.key, args.verifier)
                    phase.write_bytes(original)
                    index = root / "bundle/index.json"
                    index.write_bytes(workflow.wire({"schema": workflow.SCHEMA, "candidates": ["c0", "c1"]}))
                    with self.assertRaisesRegex(ValueError, "schema mismatch"):
                        workflow.verify_bundle(args.bundle, daemon.key, args.verifier)
                    index.write_bytes(workflow.wire({"schema": workflow.SCHEMA_RESOURCES, "candidates": ["c0"]}))
                    (root / "bundle/c1").rename(root / "omitted")
                    with self.assertRaisesRegex(ValueError, "planned candidate omission"):
                        workflow.verify_bundle(args.bundle, daemon.key, args.verifier)


class ResourceIntegrationTests(unittest.TestCase):
    """Real agentd + libjef + bubblewrap; requires the delegated-root fixture.

    No builds or fake enforcement. Run after rebuilding agentd, or select only
    ResourceUnitTests while the shared binary is still being implemented.
    """

    def setUp(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from cgroup_fixture import delegated_root, CgroupUnavailable

        @contextmanager
        def checked_root():
            try:
                with delegated_root() as root:
                    yield root
            except CgroupUnavailable as exc:
                self.skipTest(str(exc))

        self.delegated_root = checked_root
        root = Path(__file__).resolve().parents[1]
        build = Path(os.environ.get("JASH_BUILD_DIR", root / "build"))
        self.agent = os.environ.get("JASH_AGENTD_BIN", str(build / "jash-agentd"))
        self.verifier = os.environ.get("JASH_VERIFIER_BIN", str(build / "jef_verify"))
        for binary in (self.agent, self.verifier, "/usr/bin/bwrap"):
            if not os.access(binary, os.X_OK):
                self.skipTest("missing real-test executable: " + binary)
        self.temp = tempfile.TemporaryDirectory(prefix="autonomy-resources-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        (self.repo / "target").write_bytes(b"baseline\n")
        env = {**workflow.ENV, "HOME": str(self.root), "GIT_CONFIG_NOSYSTEM": "1",
               "GIT_CONFIG_GLOBAL": "/dev/null"}
        subprocess.run(["/usr/bin/git", "init", "-q", "--template=", str(self.repo)],
                       env=env, check=True, capture_output=True, timeout=10)
        subprocess.run(["/usr/bin/git", "-C", str(self.repo), "add", "target"],
                       env=env, check=True, capture_output=True, timeout=10)

    def run_campaign(self, root, generator, resources=None, timeout=5000):
        args = Namespace(repo=self.repo, target="target", bundle=self.root / "bundle",
                         runtime_home=self.root / "home", agentd=self.agent, verifier=self.verifier,
                         generate=[workflow.wire(generator).decode()], check='["/bin/true"]',
                         timeout_ms=timeout, cgroup_root=str(root),
                         resources=workflow.wire(resources).decode() if resources else None)
        result = workflow.run(args)
        self.assertEqual((self.repo / "target").read_bytes(), b"baseline\n")
        self.assertEqual(result["schema"], workflow.SCHEMA_RESOURCES)
        verified, _ = workflow.verify_bundle(args.bundle, result["local_pubkey"], self.verifier)
        self.assertEqual(verified["evidence"], result["evidence"])
        phase = args.bundle / "c0/generate"
        start = workflow.decode((phase / "start-response.json").read_bytes())
        terminal = workflow.decode((phase / "terminal-response.json").read_bytes())
        profile = resources or {**LIMITS, "wall_timeout_ms": timeout}
        for row in (start, terminal):
            self.assertEqual(row["resources"], profile)
            self.assertEqual(row["resource_backend"], "cgroup-v2")
            self.assertIs(row["resource_enforced"], True)
        self.assertIs(terminal["resource_empty"], True)
        for key in ("resource_memory_events", "resource_pids_events"):
            self.assertIs(type(terminal[key]), int)
        return result, terminal

    def test_real_defaults_and_no_config_root_in_payload(self):
        with self.delegated_root() as root:
            result, terminal = self.run_campaign(root, ["/usr/bin/python3", "-B", "-c",
                "import os,pathlib; assert 'JASH_AGENT_CGROUP_ROOT' not in os.environ; "
                "assert not pathlib.Path('/sys/fs/cgroup').exists(); "
                "pathlib.Path('target').write_text('candidate')"])
        self.assertEqual(result["winner"], "c0")
        self.assertIs(terminal["resource_violation"], False)
        self.assertIs(terminal["resource_timed_out"], False)

    def test_real_v2_bundle_is_portable_and_publication_recovers(self):
        with self.delegated_root() as root:
            result, _ = self.run_campaign(root, ["/bin/sh", "-c", "printf candidate > target"])
        bundle = self.root / "moved-bundle"
        (self.root / "bundle").rename(bundle)
        verified, artifact = workflow.verify_bundle(bundle, result["local_pubkey"], self.verifier)
        args = Namespace(repo=self.repo, bundle=bundle, pubkey=result["local_pubkey"],
                         destination=self.root / "published")
        published = workflow.publish(args, verified, artifact)
        inode = args.destination.stat().st_ino
        self.assertEqual(published["publication"]["state"], "committed")
        self.assertEqual(workflow.publish(args, verified, artifact, recover=True)
                         ["publication"]["state"], "committed")
        self.assertEqual(args.destination.stat().st_ino, inode)

    def test_real_bounded_pids_events_disqualify_even_rc_zero(self):
        # At most 24 short-lived children, even if enforcement regresses.
        payload = ("import os,time,pathlib\nchildren=[]\n"
                   "for _ in range(24):\n"
                   " try: pid=os.fork()\n"
                   " except BlockingIOError: break\n"
                   " if pid == 0:\n  time.sleep(0.4)\n  os._exit(0)\n"
                   " children.append(pid)\n"
                   "for pid in children: os.waitpid(pid,0)\n"
                   "pathlib.Path('target').write_text('candidate')\n")
        with self.delegated_root() as root:
            result, terminal = self.run_campaign(root, ["/usr/bin/python3", "-B", "-c", payload],
                                                 {**LIMITS, "pids_max": 8})
        self.assertTrue(result["verified"])
        self.assertIsNone(result["winner"])
        self.assertEqual(terminal["rc"], 0)
        self.assertGreater(terminal["resource_pids_events"], 0)
        self.assertIs(terminal["resource_violation"], True)

    def test_real_bounded_memory_failure_has_verifiable_ineligible_evidence(self):
        with self.delegated_root() as root:
            result, terminal = self.run_campaign(root, ["/usr/bin/python3", "-B", "-c",
                "data=bytearray(64*1024*1024)"], {**LIMITS, "memory_max_bytes": 32 * 1024 * 1024})
        self.assertTrue(result["verified"])
        self.assertIsNone(result["winner"])
        self.assertGreater(terminal["resource_memory_events"], 0)
        self.assertIs(terminal["resource_violation"], True)

    def test_real_wall_deadline_has_verifiable_ineligible_evidence(self):
        with self.delegated_root() as root:
            result, terminal = self.run_campaign(root, ["/bin/sleep", "1"],
                                                 {**LIMITS, "wall_timeout_ms": 200})
        self.assertTrue(result["verified"])
        self.assertIsNone(result["winner"])
        self.assertIs(terminal["resource_timed_out"], True)

    def test_real_unsupported_root_refuses_without_candidate_execution(self):
        # Canonical but not a cgroup filesystem: only agentd decides support.
        root = self.root / "not-a-cgroup"
        root.mkdir()
        with self.assertRaises(ValueError):
            self.run_campaign(root, ["/bin/sh", "-c", "printf changed > target"])
        self.assertEqual((self.repo / "target").read_bytes(), b"baseline\n")
        self.assertFalse((self.root / "bundle/index.json").exists())
        self.assertEqual(list(root.iterdir()), [])


if __name__ == "__main__":
    result = unittest.main(verbosity=2, exit=False).result
    sys.exit(1 if not result.wasSuccessful() else 77 if result.skipped else 0)
