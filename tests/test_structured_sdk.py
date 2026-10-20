"""B26: exact SDK values, verified JTV2, scoped read authority and MCP discovery."""
import base64
from decimal import Decimal
import importlib.util
import io
import json
import os
from pathlib import Path
import secrets
import select
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
JASH = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()
AGENT = Path(os.environ.get("JASH_AGENTD_BIN", ROOT / "build/jash-agentd")).resolve()
spec = importlib.util.spec_from_file_location("structured_sdk", os.environ.get(
    "JASH_CLIENT_PATH", ROOT / "integrations/jash_client.py"))
sdk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sdk)


def environment(home):
    env = {key: value for key, value in os.environ.items() if not key.startswith("JASH_")}
    env.update(HOME=str(home), JASH_SIGIL="0", JASH_TIMELINE="0", JASH_MEMO="0",
               JASH_NUDGE="0", JASH_ADOPT="0", JASH_BRIEF="0", JASH_WORLD="0",
               JASH_PREFETCH="0", JASH_LOOPGUARD="off")
    return env


class FakeSocket:
    def __init__(self):
        self.sent = []
        self.closed = False

    def sendall(self, data):
        self.sent.append(data)

    def settimeout(self, _):
        pass

    def close(self):
        self.closed = True


def fake_client(frames):
    client = sdk.JashLocalClient("unused", "unused")
    client._socket = FakeSocket()
    client._input = io.BytesIO(b"".join(json.dumps(frame).encode() + b"\n" for frame in frames))
    return client


def start(**extra):
    return {"id": "one", "protocol_version": 2, "event": "start", **extra}


def chunk(data, seq=0, stream="stdout"):
    return {"id": "one", "protocol_version": 2, "event": "chunk", "seq": seq,
            "stream": stream, "data_b64": base64.b64encode(data).decode()}


def result(stdout=0, stderr=0):
    return {"id": "one", "protocol_version": 2, "event": "result", "rc": 0,
            "stdout_bytes": stdout, "stderr_bytes": stderr,
            "stdout_ref": "explicit-only", "stderr_ref": "explicit-only"}


class ExactCodecTests(unittest.TestCase):
    def test_exact_json_nested_unicode_and_decimal_roundtrip(self):
        raw = b'{"n":18446744073709551615,"f":0.10000000000000000001,"v":[true,null,{"\\u0000":"\\ud83d\\ude00"}]}'
        value = sdk.json_loads_exact(raw)
        self.assertEqual(value["n"], 18446744073709551615)
        self.assertEqual(value["f"], Decimal("0.10000000000000000001"))
        self.assertIs(value["v"][0], True)
        self.assertIsNone(value["v"][1])
        self.assertEqual(value["v"][2], {"\0": "😀"})
        self.assertEqual(sdk.json_loads_exact(sdk.json_dumps_exact(value)), value)

    def test_json_and_output_negative_controls(self):
        for invalid in (b'{"a":1,"a":2}', b'NaN', b'Infinity', b'"\\ud800"', b'"\xff"', b'{} trailing'):
            with self.subTest(invalid=invalid), self.assertRaises(sdk.JashProtocolError):
                sdk.json_loads_exact(invalid)
        for invalid in (Decimal("NaN"), float("inf"), '\ud800', {'\ud800': 1}):
            with self.assertRaises(ValueError):
                sdk.json_dumps_exact(invalid)
        value = {"stdout": {"type": "bytes", "encoding": "base64", "data": "AP8="}, "stdout_bytes": 2}
        self.assertEqual(sdk.output_bytes(value), b"\0\xff")
        for invalid in ({"stdout_head": "partial", "stdout_ref": "do-not-open", "stdout_bytes": 50},
                        {**value, "stdout_bytes": 1}, {"stdout": "é", "stdout_bytes": 1},
                        {"stdout": {"type": "bytes", "encoding": "base64", "data": "Zh=="}, "stdout_bytes": 1}):
            with self.assertRaises(sdk.JashProtocolError):
                sdk.output_bytes(invalid)

    def test_stream_collection_binds_id_global_sequence_and_exact_lengths(self):
        client = fake_client([start(), chunk(b'{"v":'), chunk(b'warning', 1, 'stderr'),
                              chunk(b'9007199254740993}', 2), result(22, 7)])
        answer = client.structured("one", ["echo", "--json"], framing="json")
        self.assertEqual(answer.value, {"v": 9007199254740993})
        self.assertEqual(answer.stderr, b"warning")
        sent = json.loads(client._socket.sent[0])
        self.assertEqual(sent["caps"], ["effect:read"])
        self.assertEqual(sent["protocol_version"], 2)
        client.close()

    def test_invalid_streams_close_connection_without_retry(self):
        controls = [
            [chunk(b'x'), result(1)], [start(), start(), result()],
            [start(), {**chunk(b'x'), "id": "other"}, result(1)],
            [start(), chunk(b'x', 1), result(1)],
            [start(), {**chunk(b'x'), "seq": False}, result(1)],
            [start(), {**chunk(b'x'), "stream": {}}, result(1)],
            [start(), {**chunk(b'x'), "data_b64": "eB=="}, result(1)],
            [start(), chunk(b'x'), result(2)], [start(), chunk(b'x')],
            [start(), chunk(b''), result()],
        ]
        for frames in controls:
            client = fake_client(frames)
            connection = client._socket
            with self.subTest(frames=frames), self.assertRaises(sdk.JashProtocolError) as caught:
                client.request("one", ["pwd"], stream=True)
            self.assertTrue(caught.exception.outcome_unknown)
            self.assertTrue(connection.closed)
            self.assertEqual(len(connection.sent), 1)

    def test_replay_does_not_treat_retained_ref_as_empty_complete_output(self):
        client = fake_client([start(replayed=True), result(80)])
        with self.assertRaises(sdk.JashProtocolError):
            client.structured("one", ["pwd", "--json"], framing="json")
        self.assertEqual(len(client._socket.sent), 1)
        client.close()

    def test_raw_argv_and_input_bounds_before_sending(self):
        client = fake_client([{"id": "one", "rc": 125, "error": "capability denied"}])
        answer = client.request("one", ["echo", b"\xff"], input_bytes=b"\0\xff")
        self.assertEqual(answer["rc"], 125)
        request = json.loads(client._socket.sent[0])
        self.assertEqual(request["argv"][1], {"encoding": "base64", "data": "/w=="})
        self.assertEqual(base64.b64decode(request["stdin_b64"]), b"\0\xff")
        sent = len(client._socket.sent)
        for argv, stdin in ((["echo", "\0"], None), (["echo"], b'x' * (sdk.MAX_STDIN + 1))):
            with self.assertRaises(ValueError):
                client.request("next", argv, input_bytes=stdin)
        self.assertEqual(len(client._socket.sent), sent)
        client.close()


class NativeDecoderTests(unittest.TestCase):
    def test_real_importer_projects_complete_and_failed_terminals(self):
        with tempfile.TemporaryDirectory(prefix="jash-sdk-native-") as tmp:
            def native(argv, data=b''):
                return subprocess.run([str(JASH), "--exec-json", json.dumps({"argv": argv})],
                                      cwd=tmp, env=environment(tmp), input=data,
                                      capture_output=True, timeout=10)
            source = b'{"n":9007199254740993,"nested":[true,null,"\\u0000"]}'
            wire = native(["json", "--to-data"], source).stdout
            projection = sdk.native_projection(wire, JASH)
            self.assertEqual(projection["items"], [json.loads(source)])
            self.assertEqual(projection["terminal"]["state"], "complete")
            self.assertTrue(projection["terminal"]["hash_verified"])
            for corrupt in (wire[:-1], wire + b'x', wire.replace(b'9007', b'9008', 1) if b'9007' in wire else wire[:40] + bytes([wire[40] ^ 1]) + wire[41:]):
                with self.assertRaises(sdk.JashProtocolError):
                    sdk.native_projection(corrupt, JASH)
            failed = native(["calc", "--data", "1/0"])
            self.assertNotEqual(failed.returncode, 0)
            report = sdk.native_projection(failed.stdout, JASH)
            self.assertEqual(report["items"], [])
            self.assertEqual(report["terminal"]["state"], "failed")
            self.assertEqual(report["terminal"]["producer_status"], 2)
            self.assertNotEqual(native(["json", "--from-data"], failed.stdout).returncode, 0)


class ScopedRuntimeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="jash-sdk-scoped-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.home = self.root / 'home'
        self.home.mkdir()
        self.work = self.root / 'work'
        self.work.mkdir()
        self.scope = self.root / 'scope'
        self.scope.write_text(secrets.token_hex(32))
        self.scope.chmod(0o600)
        self.socket = self.root / 'socket'
        self.env = environment(self.home)
        self.server = subprocess.Popen([str(AGENT), '--listen', str(self.socket),
                                        '--scope-file', str(self.scope)], cwd=self.work, env=self.env,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.stop)
        deadline = time.monotonic() + 5
        while not self.socket.exists() and self.server.poll() is None and time.monotonic() < deadline:
            time.sleep(.01)
        diagnostic = self.server.stderr.read().decode(errors='replace') if self.server.poll() is not None else ''
        self.assertTrue(self.socket.exists(), 'scoped Unix listener unavailable: ' + diagnostic)

    def stop(self):
        if self.server.poll() is None:
            self.server.terminate()
            try:
                self.server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.server.kill()
                self.server.wait(timeout=5)
        self.server.stdout.close()
        self.server.stderr.close()

    def test_native_query_filter_apply_verify_keeps_authority_explicit(self):
        (self.work / 'source').write_bytes(b'payload\0\xff')
        with sdk.JashLocalClient(self.socket, self.scope, timeout=10) as client:
            def query(ident, argv, **kwargs):
                return client.structured(ident, argv, framing='jtv2', native_decoder=JASH, **kwargs)
            listing = query('metadata', ['stat', '--data', 'source'])
            self.assertTrue(listing.native_complete)
            filtered = query('filter', ['where', '--data', 'size>0'], input_bytes=listing.stdout)
            self.assertEqual(len(filtered.value), 1)
            with self.assertRaises(sdk.JashCommandError):
                query('denied', ['cp', '--data', 'source', 'copied'])
            self.assertFalse((self.work / 'copied').exists())
            copied = query('copy', ['cp', '--data', 'source', 'copied'], caps=('effect:mutate',))
            self.assertTrue(copied.native_complete)
            self.assertTrue(any(row.get('kind') == 'action' for row in copied.value))
            verified = query('verify', ['stat', '--data', 'copied'])
            self.assertEqual(verified.value[0]['size'], len(b'payload\0\xff'))
            self.assertEqual((self.work / 'copied').read_bytes(), b'payload\0\xff')

    def test_exact_json_and_native_bytes_over_real_v2_transport(self):
        with sdk.JashLocalClient(self.socket, self.scope, timeout=10) as client:
            value = client.structured('decimal', ['sum', 'n'], framing='jsonl',
                                      input_bytes=b'{"n":0.1}\n{"n":0.2}\n')
            self.assertEqual(value.value[0]['sum'], Decimal('0.3'))
            raw = client.structured('raw', ['echo', '--data', b'\xff', ''],
                                    framing='jtv2', native_decoder=JASH)
            self.assertEqual(base64.b64decode(raw.value[0][0]['data']), b'\xff')
            self.assertEqual(raw.value[0][1], '')
            failed = client.structured('failed', ['calc', '--data', '1/0'],
                                       framing='jtv2', native_decoder=JASH)
            self.assertFalse(failed.native_complete)
            self.assertEqual(failed.rc, 2)
            self.assertEqual(failed.terminal['state'], 'failed')
            for input_bytes in (b'{"n":1}\n{broken}\n', b'{"n":NaN}\n'):
                with self.assertRaises(sdk.JashCommandError):
                    client.structured('bad-' + str(len(input_bytes)), ['where', 'n>0'],
                                      framing='jsonl', input_bytes=input_bytes)

    def test_chunked_large_response_and_replay_are_distinguished(self):
        (self.work / 'large').write_bytes(b'x' * 50000)
        with sdk.JashLocalClient(self.socket, self.scope, timeout=10) as client:
            response = client.request('large', ['cat', 'large'], stream=True)
            self.assertEqual(sdk.output_bytes(response), b'x' * 50000)
            replay = client.request('large', ['cat', 'large'], stream=True)
            self.assertEqual(replay['rc'], 0)
            with self.assertRaises(sdk.JashProtocolError):
                sdk.output_bytes(replay)

    def test_configured_mcp_catalog_native_query_and_denied_effect(self):
        process = subprocess.Popen(['python3', '-B', str(ROOT / 'integrations/jash_mcp.py'),
                                    '--jash', str(AGENT), '--socket', str(self.socket),
                                    '--scope-file', str(self.scope), '--native-decoder', str(JASH)], cwd=self.work, env=self.env,
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            def request(ident, method, params=None):
                process.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': ident, 'method': method,
                                               'params': params}).encode() + b'\n')
                process.stdin.flush()
                self.assertTrue(select.select([process.stdout], [], [], 15)[0], 'MCP timed out')
                answer = json.loads(process.stdout.readline())
                self.assertEqual(answer['id'], ident)
                return answer
            request(1, 'initialize', {'protocolVersion': '2025-06-18', 'capabilities': {},
                                     'clientInfo': {'name': 'test', 'version': '1'}})
            catalog = request(2, 'tools/list')
            self.assertEqual({tool['name'] for tool in catalog['result']['tools']},
                             {'builtin.catalog', 'builtin.query'})
            contract = request(3, 'tools/call', {'name': 'builtin.catalog', 'arguments': {'name': 'where'}})
            self.assertFalse(contract['result']['isError'])
            metadata = contract['result']['structuredContent']
            self.assertEqual(metadata['profile'], 'agent-strict')
            self.assertEqual(metadata['total'], 1)
            self.assertFalse(metadata['complete'])
            variants = metadata['operations'][0]['structured_modes']['data']['variants']
            matching = [variant for variant in variants if variant.get('selector') == '--to-data'
                        and variant.get('input_framing') == 'jsonl' and variant.get('framing') == 'jtv2']
            self.assertEqual(len(matching), 1, variants)
            selected = matching[0]
            args = {'name': 'where', 'mode': 'data', 'variant': selected['id'], 'args': ['n>1'],
                    'stdin_b64': base64.b64encode(b'{"n":9007199254740993,"b":true}\n').decode()}
            native = request(4, 'tools/call', {'name': 'builtin.query', 'arguments': args})
            self.assertFalse(native['result']['isError'], native)
            value = native['result']['structuredContent']
            self.assertEqual(value['value'], [{'n': 9007199254740993, 'b': True}])
            self.assertTrue(value['native_complete'])
            self.assertTrue(base64.b64decode(value['wire_b64']).startswith(b'JTV2'))
            self.assertEqual(value['contract']['variant'], selected['id'])
            mutation_contract = request(5, 'tools/call', {'name': 'builtin.catalog',
                                                        'arguments': {'name': 'touch'}})
            self.assertFalse(mutation_contract['result']['isError'])
            mutation = mutation_contract['result']['structuredContent']['operations'][0]
            candidates = [variant for variant in mutation['structured_modes']['json']['variants']
                          if variant.get('selector') == '--json' and variant.get('framing') == 'json'
                          and variant.get('input_framing') == 'none']
            self.assertEqual(len(candidates), 1, mutation)
            denied = request(6, 'tools/call', {'name': 'builtin.query', 'arguments': {
                'name': 'touch', 'mode': 'json', 'variant': candidates[0]['id'], 'args': ['not-created']}})
            self.assertTrue(denied['result']['isError'])
            self.assertEqual(denied['result']['structuredContent']['rc'], 125)
            self.assertFalse((self.work / 'not-created').exists())
            invalid = request(7, 'tools/call', {'name': 'builtin.query', 'arguments': {
                'name': 'eval', 'mode': 'json', 'variant': 'invented', 'args': ['touch hidden']}})
            self.assertTrue(invalid['result']['isError'])
            self.assertFalse((self.work / 'hidden').exists())
            excessive = request(8, 'tools/call', {'name': 'builtin.catalog', 'arguments': {'limit': 65}})
            self.assertEqual(excessive['error']['code'], -32602)
        finally:
            process.stdin.close()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=5)
            process.stdout.close()
            process.stderr.close()


if __name__ == '__main__':
    unittest.main()
