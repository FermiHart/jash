"""Independent small JDV1/JSC1 reader for builtin integration assertions.

The production importer/verifier has separate C tests. This reader checks
canonical field order, schemas, values, framing and terminal accounting without
using the builtin's projection code to manufacture expected observations.
"""

import base64
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
from types import SimpleNamespace
import unittest

from test_ls_data_wire import frames


ROOT = Path(__file__).resolve().parents[1]
JASH = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()
AGENT = Path(os.environ.get("JASH_AGENTD_BIN", ROOT / "build/jash-agentd")).resolve()
KINDS = ("nothing", "bool", "i64", "u64", "filesize", "timestamp", "duration",
         "string", "bytes", "path", "symbol", "list", "record")


class Reader:
    def __init__(self, data):
        self.data = data
        self.position = 0

    def take(self, count):
        assert 0 <= count <= len(self.data) - self.position
        value = self.data[self.position:self.position + count]
        self.position += count
        return value

    def byte(self):
        return self.take(1)[0]

    def integer(self, signed=False):
        return int.from_bytes(self.take(8), "big", signed=signed)

    def blob(self):
        return self.take(self.integer())

    def value(self):
        tag = self.byte()
        assert tag < len(KINDS)
        kind = KINDS[tag]
        if kind == "nothing":
            value = None
        elif kind == "bool":
            value = self.byte()
            assert value in (0, 1)
            value = bool(value)
        elif kind in ("i64", "u64", "filesize", "timestamp", "duration"):
            value = self.integer(kind in ("i64", "timestamp", "duration"))
        elif kind in ("string", "bytes", "path", "symbol"):
            value = self.blob()
            if kind in ("string", "symbol"):
                value = value.decode("utf-8")
        elif kind == "list":
            value = [self.value() for _ in range(self.integer())]
        else:
            value = {}
            previous = None
            for _ in range(self.integer()):
                key = self.blob()
                assert previous is None or previous < key
                previous = key
                value[key.decode("utf-8")] = self.value()
        return SimpleNamespace(kind=kind, value=value)

    def schema(self):
        kind, nullable = self.byte(), self.byte()
        assert nullable in (0, 1)
        result = {"kind": kind, "nullable": bool(nullable)}
        if kind == 1:
            result["scalar"] = KINDS[self.byte()]
        elif kind == 2:
            result["element"] = self.schema()
        elif kind == 3:
            result["open"] = bool(self.byte())
            result["fields"] = {}
            previous = None
            for _ in range(self.integer()):
                key = self.blob()
                assert previous is None or previous < key
                previous = key
                optional = self.byte()
                assert optional in (0, 1)
                result["fields"][key.decode("utf-8")] = (bool(optional), self.schema())
        else:
            assert kind in (0, 4, 5)
        return result


def accepts(schema, value):
    if value.kind == "nothing" and schema["nullable"]:
        return True
    if schema["kind"] == 4:
        return True
    if schema["kind"] == 1:
        return value.kind == schema["scalar"]
    if schema["kind"] == 2:
        return value.kind == "list" and all(accepts(schema["element"], v) for v in value.value)
    if schema["kind"] == 3 and value.kind == "record":
        fields = schema["fields"]
        if not schema["open"] and not set(value.value).issubset(fields):
            return False
        return all((key not in value.value and optional) or
                   (key in value.value and accepts(child, value.value[key]))
                   for key, (optional, child) in fields.items())
    return False


def stream(payload, *, credit_kind="canonical"):
    assert credit_kind in ("canonical", "none")
    parsed = list(frames(payload, b"JTV2"))
    assert [tag for tag, _ in parsed[:2]] == [b"D", b"G"]
    assert parsed[-1][0] == b"T"
    assert sum(tag == b"T" for tag, _ in parsed) == 1
    schemas, items, credits = {}, [], []
    for tag, raw in parsed[:-1]:
        if tag in (b"D", b"G", b"F"):
            reader = Reader(raw)
            assert reader.take(4) == b"JSC1"
            schemas[tag] = reader.schema()
        else:
            assert tag == b"I"
            reader = Reader(raw)
            credit = reader.integer()
            assert reader.take(4) == b"JDV1"
            item = reader.value()
            assert accepts(schemas[b"G"], item), item
            assert credit == (len(raw) - 8 if credit_kind == "canonical" else 0)
            credits.append(credit)
            items.append(item)
        assert reader.position == len(reader.data)
    raw = parsed[-1][1]
    assert len(raw) >= 96
    state, completeness, truncated, hash_present, algorithm = raw[:5]
    count, logical, errors, producer, transform, transport = struct.unpack_from(">QQQqqq", raw, 8)
    reason_length = int.from_bytes(raw[88:96], "big")
    assert len(raw) == 96 + reason_length
    assert count == len(items) and logical == sum(credits)
    assert transform == 0 and transport == 0
    if state == 0:
        assert completeness == 1 and not truncated and hash_present and algorithm == 1
        assert b"F" in schemas
        assert all(accepts(schemas[b"F"], item) for item in items)
    else:
        assert completeness == 2 and b"F" not in schemas
    return SimpleNamespace(items=items, schemas=schemas, count=count, errors=errors,
                           state=state, truncated=bool(truncated), producer=producer,
                           reason=raw[96:].decode("utf-8"), logical=logical)


class NativeQueryCase(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="jash-native-query-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.work = self.root / "work"
        self.work.mkdir()
        home = self.root / "home"
        home.mkdir()
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("JASH_")}
        self.env.update(HOME=str(home), JASH_AGENT="1", JASH_SIGIL="0", JASH_NUDGE="0",
                        JASH_TIMELINE="0", JASH_MEMO="0", JASH_ADOPT="0", JASH_BRIEF="0",
                        JASH_LOOPGUARD="off", JASH_WORLD="0", JASH_PREFETCH="0")
        self.serial = 0

    def run_command(self, argv, *, strict=False, stdin=None, caps=None):
        args = [{"encoding": "base64", "data": base64.b64encode(arg).decode("ascii")}
                if isinstance(arg, bytes) else arg for arg in argv]
        request = {"argv": args, "cwd": str(self.work)}
        if stdin is not None:
            request["stdin_b64"] = base64.b64encode(stdin).decode("ascii")
        if not strict:
            request["request_version"] = 2
            return subprocess.run([str(JASH), "--exec-json", json.dumps(request)],
                                  cwd=self.work, env=self.env, capture_output=True, timeout=20)
        self.serial += 1
        request.update(id=f"native-query-{self.serial}", protocol_version=2,
                       caps=caps or ["effect:read"])
        result = subprocess.run([str(AGENT)], input=(json.dumps(request) + "\n").encode(),
                                cwd=self.work, env=self.env, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        replies = [row for row in rows if row.get("id") == request["id"]]
        terminal = [row for row in replies if "rc" in row]
        self.assertEqual(len(terminal), 1, replies)
        output = {}
        for channel in ("stdout", "stderr"):
            output[channel] = b"".join(base64.b64decode(row["data_b64"], validate=True)
                                       for row in replies if row.get("event") == "chunk"
                                       and row["stream"] == channel)
            if channel + "_bytes" in terminal[0]:
                self.assertEqual(len(output[channel]), terminal[0][channel + "_bytes"])
        if "error" in terminal[0]:
            output["stderr"] += json.dumps(terminal[0]).encode()
        return SimpleNamespace(returncode=terminal[0]["rc"], **output)

    def native(self, argv, *, rc=0, **kwargs):
        result = self.run_command(argv, **kwargs)
        self.assertEqual(result.returncode, rc, result.stderr)
        decoded = stream(result.stdout)
        self.assertEqual(decoded.producer, rc)
        return decoded

    def scalar(self, record, key, kind, expected=None):
        item = record.value[key]
        self.assertEqual(item.kind, kind, (key, item))
        if expected is not None:
            self.assertEqual(item.value, expected, key)
        return item.value
