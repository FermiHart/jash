#!/usr/bin/env python3
"""W07: native local endpoint, scope authentication and client isolation."""
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import time
import unittest
import base64

ROOT = Path(__file__).resolve().parents[1]
AGENT = Path(os.environ.get("JASH_AGENTD_BIN", ROOT / "build/jash-agentd")).resolve()


class Sessions(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="jash-local-")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.path = self.root / "socket"
        self.token = secrets.token_hex(32)
        self.key = self.root / "scope"; self.key.write_text(self.token); self.key.chmod(0o600)
        env = {k: v for k, v in os.environ.items() if not k.startswith("JASH_")}
        env.update(HOME=str(self.root), JASH_SIGIL="0", JASH_TIMELINE="0")
        self.env = env
        self.server = subprocess.Popen([str(AGENT), "--listen", str(self.path), "--scope-file", str(self.key)],
                                       cwd=self.root, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.stop)
        deadline = time.monotonic() + 5
        while not self.path.exists() and self.server.poll() is None and time.monotonic() < deadline: time.sleep(.01)
        self.assertTrue(self.path.exists(), self.server.poll())

    def stop(self):
        if self.server.poll() is None:
            self.server.terminate()
            try: self.server.wait(timeout=5)
            except subprocess.TimeoutExpired: self.server.kill(); self.server.wait()
        self.server.stdout.close(); self.server.stderr.close()

    def open_socket(self):
        deadline = time.monotonic() + 5
        while True:
            client = socket.socket(socket.AF_UNIX); client.settimeout(5)
            try: client.connect(str(self.path)); break
            except (FileNotFoundError, ConnectionRefusedError):
                client.close()
                if self.server.poll() is not None or time.monotonic() >= deadline: raise
                time.sleep(.01)
        self.addCleanup(client.close)
        return client

    def connect(self, token=None):
        client = self.open_socket()
        stream = client.makefile("rwb", buffering=0); self.addCleanup(stream.close)
        self.assertEqual(json.loads(stream.readline())["jash_local"], 1)
        stream.write((json.dumps({"scope": token or self.token}, separators=(",", ":")) + "\n").encode())
        return client, stream, json.loads(stream.readline())

    def request(self, stream, ident, argv, **extra):
        request = {"id": ident, "argv": argv, "caps": ["effect:read", "effect:mutate"], **extra}
        stream.write((json.dumps(request) + "\n").encode())
        return json.loads(stream.readline())

    def test_two_clients_have_isolated_environment(self):
        _, a, hello = self.connect(); self.assertIn("schema_version", hello)
        _, b, _ = self.connect()
        self.assertEqual(self.request(a, "set-a", ["export", "LOCAL_VALUE=first"])["rc"], 0)
        answer = self.request(b, "env-b", ["env"], caps=["effect:exec"])
        self.assertEqual(answer["rc"], 0, answer)
        self.assertNotIn("LOCAL_VALUE=first", answer["stdout"])
        self.assertEqual(self.request(a, "echo-a", ["echo", "active"])["stdout"], "active\n")

    def test_wrong_scope_denied_before_execution(self):
        client, stream, response = self.connect("0" * 64)
        self.assertEqual(response["error"], "scope_denied")
        self.assertEqual(stream.readline(), b"")
        self.assertFalse((self.root / "marker").exists())

    def test_malformed_authentication_frame_is_rejected(self):
        client = self.open_socket()
        stream = client.makefile("rwb", buffering=0); self.addCleanup(stream.close)
        stream.readline()
        stream.write(b'{"scope":"' + self.token.encode() + b'","extra":true}\n')
        self.assertEqual(json.loads(stream.readline())["error"], "scope_denied")

    def test_reconnect_and_permission(self):
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o700)
        client, stream, _ = self.connect()
        self.assertEqual(self.request(stream, "one", ["echo", "one"])["rc"], 0)
        stream.close(); client.close()
        _, second, _ = self.connect()
        self.assertEqual(self.request(second, "two", ["echo", "two"])["stdout"], "two\n")

    def test_reconnect_replays_exact_committed_response_and_rejects_conflict(self):
        (self.root / "source").write_text("one mutation")
        client, first, _ = self.connect()
        argv = ["mv", "--json", "--atomic", "source", "target"]
        response = self.request(first, "reconnect-move", argv)
        self.assertEqual(response["rc"], 0, response)
        first.close(); client.close()
        _, second, _ = self.connect()
        self.assertEqual(self.request(second, "reconnect-move", argv), response)
        denied = self.request(second, "reconnect-move", ["rm", "target"])
        self.assertNotEqual(denied["rc"], 0)
        self.assertEqual((self.root / "target").read_text(), "one mutation")
        self.assertFalse((self.root / "source").exists())

    def test_listener_restart_replays_persisted_response(self):
        (self.root / "source").write_text("persisted")
        client, first, _ = self.connect()
        argv = ["mv", "--json", "--atomic", "source", "target"]
        response = self.request(first, "restart-move", argv)
        self.assertEqual(response["rc"], 0, response)
        first.close(); client.close(); self.stop()
        self.assertFalse(self.path.exists())
        self.server = subprocess.Popen([str(AGENT), "--listen", str(self.path), "--scope-file", str(self.key)],
                                       cwd=self.root, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        deadline = time.monotonic() + 5
        while not self.path.exists() and time.monotonic() < deadline: time.sleep(.01)
        _, second, _ = self.connect()
        self.assertEqual(self.request(second, "restart-move", argv), response)
        self.assertEqual((self.root / "target").read_text(), "persisted")

    def test_disconnect_before_response_can_retry_persisted_request(self):
        (self.root / "source").write_text("delivery unknown")
        client, stream, _ = self.connect()
        argv = ["mv", "--json", "--atomic", "source", "target"]
        request = {"id": "lost-delivery", "argv": argv, "caps": ["effect:read", "effect:mutate"]}
        stream.write((json.dumps(request) + "\n").encode())
        stream.close(); client.close()
        _, retry, _ = self.connect()
        deadline = time.monotonic() + 5
        while True:
            response = self.request(retry, "lost-delivery", argv)
            if response["rc"] == 0 or time.monotonic() >= deadline: break
            time.sleep(.02)
        self.assertEqual(response["rc"], 0, response)
        self.assertEqual(json.loads(response["stdout"])["transaction"], "committed")
        self.assertEqual((self.root / "target").read_text(), "delivery unknown")
        self.assertFalse((self.root / "source").exists())

    def test_slow_unauthenticated_client_does_not_block_another(self):
        slow = self.open_socket()
        _, fast, _ = self.connect()
        self.assertEqual(self.request(fast, "fast", ["echo", "done"])["stdout"], "done\n")

    def test_gossip_cross_client_cursor_and_reconnect(self):
        _, a, _ = self.connect(); _, b, _ = self.connect()
        sent = self.request(a, "gossip-send", ["gossip", "--channel", "work", "send", "shared data"])
        self.assertEqual(sent["rc"], 0, sent)
        self.assertTrue(json.loads(sent["stdout"])["persisted"])
        received = self.request(b, "gossip-receive", ["gossip", "--channel", "work", "receive", "--after", "0"])
        self.assertEqual(received["rc"], 0, received)
        data = json.loads(received["stdout"])
        self.assertEqual(data["messages"][0]["message"], "shared data")
        empty = self.request(b, "gossip-next", ["gossip", "--channel", "work", "receive", "--after", str(data["next_cursor"])])
        self.assertEqual(json.loads(empty["stdout"])["messages"], [])

    def test_cursor_expiry_is_explicit(self):
        _, stream, _ = self.connect()
        for i in range(35):
            result = self.request(stream, f"send-{i}", ["gossip", "--channel", "retained", "send", str(i)])
            self.assertEqual(result["rc"], 0, result)
        result = self.request(stream, "old-cursor", ["gossip", "--channel", "retained", "receive", "--after", "1"])
        self.assertEqual(result["rc"], 2)
        self.assertEqual(json.loads(result["stdout"])["error"], "cursor_expired")

    def test_capacity_rejects_excess_clients(self):
        clients = []
        for _ in range(16):
            client = self.open_socket(); clients.append(client)
            self.assertIn(b"jash_local", client.recv(256))
        client = self.open_socket()
        self.assertIn(b"peer_or_capacity_denied", client.recv(256))

    def test_listener_does_not_replace_existing_path(self):
        before = self.path.stat().st_ino
        p = subprocess.run([str(AGENT), "--listen", str(self.path), "--scope-file", str(self.key)],
                           capture_output=True, timeout=5)
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(self.path.stat().st_ino, before)

    def test_transport_is_discoverable(self):
        d = json.loads(subprocess.check_output([str(AGENT), "--contract-json", "invocation:listen"], timeout=5))
        self.assertEqual(d["input_transport"], "unix-jsonl")
        self.assertEqual(d["availability"], "compiled")
        self.assertEqual(d["traits"]["authentication"], "canonical-scope-token-v1")

    def test_mirror_verifies_a_real_receipt_from_another_connection(self):
        _, first, _ = self.connect()
        executed = self.request(first, "mirror-source", ["echo", "observed"])
        self.assertEqual(executed["rc"], 0, executed)
        _, second, _ = self.connect()
        args = ["mirror", "--json", "--receipt", executed["receipt_ref"],
                "--blake3", executed["receipt_blake3"], "--pubkey", executed["receipt_pubkey"]]
        answer = self.request(second, "mirror-view", args)
        self.assertEqual(answer["rc"], 0, answer)
        document = json.loads(answer["stdout"])
        self.assertTrue(document["verified"])
        receipt = json.loads(document["receipt"])
        payload = json.loads(base64.b64decode(receipt["payload"]))
        self.assertEqual(payload["request_id"], "mirror-source")
        self.assertEqual(payload["rc"], executed["rc"])
        args[-1] = "0" * 64
        rejected = self.request(second, "mirror-wrong-key", args)
        self.assertEqual(rejected["rc"], 1)
        self.assertFalse(json.loads(rejected["stdout"])["verified"])
        args[-1] = executed["receipt_pubkey"]
        receipt_path = self.root / ".jash/agent_receipts" / executed["receipt_ref"]
        receipt_path.write_text("{}")
        tampered = self.request(second, "mirror-tampered", args)
        self.assertEqual(tampered["rc"], 1)
        self.assertEqual(json.loads(tampered["stdout"])["error"], "receipt_hash_mismatch")

    def test_mirror_full_binding_rejects_modified_response_and_request(self):
        _, stream, _ = self.connect()
        request = {"id": "full-binding", "argv": ["echo", "bound"], "caps": ["effect:read"]}
        wire = (json.dumps(request) + "\n").encode(); stream.write(wire)
        response_bytes = stream.readline(); response = json.loads(response_bytes)
        request_path = self.root / "request.json"; request_path.write_bytes(wire)
        response_path = self.root / "response.json"; response_path.write_bytes(response_bytes)
        args = ["mirror", "--json", "--receipt", response["receipt_ref"], "--blake3", response["receipt_blake3"],
                "--pubkey", response["receipt_pubkey"], "--request", str(request_path), "--response", str(response_path)]
        checked = self.request(stream, "full-check", args)
        self.assertEqual(checked["rc"], 0, checked)
        self.assertEqual(json.loads(checked["stdout"])["verification_scope"], "runtime-request-response-and-streams")
        response_path.write_bytes(response_bytes.replace(b'bound', b'wrong'))
        self.assertEqual(self.request(stream, "bad-response", args)["rc"], 1)
        response_path.write_bytes(response_bytes)
        request_path.write_bytes(wire.replace(b'bound', b'wrong'))
        self.assertEqual(self.request(stream, "bad-request", args)["rc"], 1)

    def test_mirror_full_binding_checks_explicit_overflow_stream(self):
        data = self.root / "payload"; data.write_bytes(b"x" * 40000)
        _, stream, _ = self.connect()
        request = {"id": "overflow-binding", "argv": ["cat", str(data)], "caps": ["effect:read"]}
        wire = (json.dumps(request) + "\n").encode(); stream.write(wire)
        raw = stream.readline(); response = json.loads(raw)
        self.assertEqual(response["rc"], 0, response); self.assertIn("stdout_ref", response)
        req = self.root / "request.json"; req.write_bytes(wire)
        res = self.root / "response.json"; res.write_bytes(raw)
        args = ["mirror", "--json", "--receipt", response["receipt_ref"], "--blake3", response["receipt_blake3"],
                "--pubkey", response["receipt_pubkey"], "--request", str(req), "--response", str(res), "--stdout", str(data)]
        checked = self.request(stream, "overflow-check", args)
        self.assertEqual(checked["rc"], 0, checked)
        data.write_bytes(b"y" * 40000)
        self.assertEqual(self.request(stream, "overflow-bad", args)["rc"], 1)


class MirrorSignature(unittest.TestCase):
    def test_verifier_snapshots_are_sealed_and_independent(self):
        with tempfile.TemporaryDirectory(prefix="jash-sealed-") as tmp:
            binary = str(Path(tmp) / "snapshot")
            command = [os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g", "-I" + str(ROOT / "c"),
                       str(ROOT / "tests/test_ipc_snapshots.c"), str(ROOT / "c/osif/ipc.c"), "-o", binary]
            if os.environ.get("SANITIZE") == "1": command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
            built = subprocess.run(command, capture_output=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            checked = subprocess.run([binary, tmp], capture_output=True, timeout=10)
            self.assertEqual(checked.returncode, 0, checked.stderr)

    def test_snapshot_verifier_pins_identity_and_rejects_modified_payload(self):
        with tempfile.TemporaryDirectory(prefix="jash-mirror-signature-") as tmp:
            root = Path(tmp)
            env = {k: v for k, v in os.environ.items() if not k.startswith("JASH_")}
            env.update(HOME=tmp, JASH_SIGIL="0", JASH_TIMELINE="0")
            request = {"id": "snapshot-proof", "argv": ["echo", "observed"], "caps": ["effect:read"]}
            p = subprocess.run([str(AGENT)], input=(json.dumps(request) + "\n").encode(),
                               cwd=root, env=env, capture_output=True, timeout=15)
            result = json.loads(p.stdout.splitlines()[-1])
            self.assertEqual(result["rc"], 0, result)
            binary = str(root / "verify")
            sources = [ROOT / "tests/test_mirror_snapshot.c", ROOT / "libjef/jef_verify.c", ROOT / "libjef/ed25519.c"]
            command = [os.environ.get("CC", "cc"), "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-I" + str(ROOT / "c"),
                       *map(str, sources), "-o", binary]
            if os.environ.get("SANITIZE") == "1":
                command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
            build = subprocess.run(command, capture_output=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stderr)
            checked = subprocess.run([binary, str(root / ".jash/agent_receipts" / result["receipt_ref"]), result["receipt_pubkey"]],
                                     capture_output=True, timeout=30)
            self.assertEqual(checked.returncode, 0, checked.stderr)


if __name__ == "__main__": unittest.main()
