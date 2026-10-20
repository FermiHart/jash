"""Minimal Python client for jash-agentd's existing scoped local JSONL API.

No daemon is started here. Callers own the listener, scope file, request IDs,
and all non-read capabilities. Structured decoding is explicit and never
infers a contract from human output or follows a retained-output path.
"""

import json
import base64
from decimal import Decimal, InvalidOperation
import os
from pathlib import Path
import socket
import stat
import subprocess
import tempfile
import time


MAX_FRAME = 1024 * 1024
MAX_OUTPUT = 16 * 1024 * 1024
MAX_STDIN = 192 * 1024 - 1
DATA_FIELDS = {
    "data.import": frozenset(("transport", "data_b64")),
    "data.promote": frozenset(("artifact_ref",)),
    "data.attach": frozenset(("artifact_ref",)),
    "data.read": frozenset(("artifact_ref", "offset", "limit")),
    "job.start": frozenset(("argv", "background", "timeout_ms", "resources",
                             "cwd", "env")),
    "job.get": frozenset(("job_id",)),
    "job.wait": frozenset(("job_id", "wait_ms")),
    "job.read": frozenset(("job_id", "stream", "offset", "limit")),
    "job.result": frozenset(("job_id",)),
    "job.cancel": frozenset(("job_id", "grace_ms")),
}


class JashProtocolError(RuntimeError):
    def __init__(self, message, *, response=None, outcome_unknown=False):
        super().__init__(message)
        self.response = response
        self.outcome_unknown = outcome_unknown


class JashCommandError(RuntimeError):
    """Admission/runtime error without a decodable command report."""
    def __init__(self, response):
        super().__init__(str(response.get("error", "command result unavailable")))
        self.response = response


def _strict_pairs(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("duplicate JSON key")
        value[key] = item
    return value


def _invalid_constant(_):
    raise ValueError("nonfinite JSON number")


def json_loads_exact(raw):
    """Strict JSON with arbitrary exact integers and Decimal fractions."""
    try:
        value = json.loads(raw, object_pairs_hook=_strict_pairs,
                           parse_float=Decimal, parse_constant=_invalid_constant)
        pending = [(value, 0)]
        nodes = 0
        while pending:
            item, depth = pending.pop()
            nodes += 1
            if depth > 64 or nodes > 1048576:
                raise ValueError("JSON semantic bounds")
            if isinstance(item, str):
                item.encode("utf-8", "strict")
            elif isinstance(item, Decimal) and not item.is_finite():
                raise ValueError("nonfinite JSON number")
            elif isinstance(item, list):
                pending.extend((child, depth + 1) for child in item)
            elif isinstance(item, dict):
                for key, child in item.items():
                    key.encode("utf-8", "strict")
                    pending.append((child, depth + 1))
        return value
    except (ValueError, UnicodeError, InvalidOperation, RecursionError) as error:
        raise JashProtocolError("invalid or oversized structured JSON") from error


def json_dumps_exact(value):
    """Serialize SDK Decimal values as JSON numbers without a float cast."""
    def emit(item, depth=0):
        if depth > 64:
            raise ValueError("JSON depth limit")
        if isinstance(item, Decimal):
            if not item.is_finite():
                raise ValueError("nonfinite JSON number")
            return str(item)
        if isinstance(item, list) or isinstance(item, tuple):
            return "[" + ",".join(emit(child, depth + 1) for child in item) + "]"
        if isinstance(item, dict):
            if not all(isinstance(key, str) for key in item):
                raise ValueError("JSON object keys must be strings")
            for key in item:
                key.encode("utf-8", "strict")
            return "{" + ",".join(json.dumps(key, ensure_ascii=True) + ":" + emit(child, depth + 1)
                                   for key, child in item.items()) + "}"
        if isinstance(item, str):
            item.encode("utf-8", "strict")
        return json.dumps(item, ensure_ascii=True, allow_nan=False, separators=(",", ":"))
    return emit(value)


def _base64_bytes(encoded, maximum=MAX_OUTPUT):
    if not isinstance(encoded, str) or len(encoded) > 4 * ((maximum + 2) // 3):
        raise JashProtocolError("invalid or oversized base64 payload")
    try:
        data = base64.b64decode(encoded, validate=True)
    except (ValueError, UnicodeError) as error:
        raise JashProtocolError("invalid base64 payload") from error
    if len(data) > maximum or base64.b64encode(data).decode("ascii") != encoded:
        raise JashProtocolError("noncanonical or oversized base64 payload")
    return data


def output_bytes(response, stream="stdout"):
    """Return complete inline/collected bytes; never interpret a head as data."""
    if stream not in ("stdout", "stderr") or not isinstance(response, dict):
        raise ValueError("invalid command response or stream")
    captured = getattr(response, "_captured", None)
    if captured is not None:
        data = captured[stream]
    elif stream in response:
        value = response[stream]
        if isinstance(value, str):
            try:
                data = value.encode("utf-8", "strict")
            except UnicodeError as error:
                raise JashProtocolError("invalid output text", response=response) from error
        elif (isinstance(value, dict) and set(value) == {"type", "encoding", "data"}
              and value.get("type") == "bytes" and value.get("encoding") == "base64"):
            data = _base64_bytes(value["data"])
        else:
            raise JashProtocolError("invalid output representation", response=response)
    else:
        raise JashProtocolError("complete output unavailable; retained refs require an explicit fetch",
                                response=response, outcome_unknown=True)
    size = response.get(stream + "_bytes")
    if type(size) is not int or size != len(data) or len(data) > MAX_OUTPUT:
        raise JashProtocolError("output length mismatch", response=response, outcome_unknown=True)
    return data


def _decoder_run(binary, arguments, *, input_bytes=None, timeout=10):
    """Run only caller-selected local metadata/decoder entry points."""
    with tempfile.TemporaryDirectory(prefix="jash-sdk-decoder-") as temporary:
        env = {key: value for key, value in os.environ.items() if not key.startswith("JASH_")}
        env.update(HOME=temporary, JASH_SIGIL="0", JASH_TIMELINE="0", JASH_MEMO="0",
                   JASH_NUDGE="0", JASH_ADOPT="0", JASH_BRIEF="0", JASH_QUIET="1",
                   JASH_LOOPGUARD="off", JASH_WORLD="0", JASH_PREFETCH="0")
        try:
            result = subprocess.run([str(Path(binary).resolve()), *arguments], input=input_bytes,
                                    capture_output=True, cwd=temporary, env=env, timeout=timeout)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise JashProtocolError("local native decoder unavailable") from error
    if result.returncode != 0 or len(result.stdout) > 64 * 1024 * 1024:
        raise JashProtocolError("local native decoder rejected the result")
    return json_loads_exact(result.stdout)


def builtin_catalog(binary, *, timeout=10):
    """Compiled inventory of the explicitly supplied runtime binary."""
    value = _decoder_run(binary, ["--contract-json"], timeout=timeout)
    if not isinstance(value, dict) or not isinstance(value.get("operations"), list):
        raise JashProtocolError("invalid compiled builtin inventory")
    return value


def native_projection(wire, binary, *, timeout=10):
    """Validate JTV2 with the production C importer; retain native wire separately.

    JSON projection intentionally loses units/symbol/path distinctions. No
    Python wire parser or tag inference is used. Valid failed terminals remain
    visible and are never promoted to complete results.
    """
    if not isinstance(wire, bytes) or len(wire) > MAX_OUTPUT or not wire.startswith(b"JTV2"):
        raise JashProtocolError("invalid native wire input")
    value = _decoder_run(binary, ["--exec-json", '{"argv":["json","--inspect-data"]}'],
                         input_bytes=wire, timeout=timeout)
    if (not isinstance(value, dict) or value.get("schema") != "jash.data.projection.v1"
            or not isinstance(value.get("items"), list) or not isinstance(value.get("terminal"), dict)):
        raise JashProtocolError("native decoder returned no terminal contract")
    end = value["terminal"]
    if (end.get("hash_verified") is not True or not isinstance(end.get("hash"), str)
            or len(end["hash"]) != 64 or any(c not in "0123456789abcdef" for c in end["hash"])
            or type(end.get("items")) is not int or end["items"] != len(value["items"])):
        raise JashProtocolError("native result has no verified ordered prefix")
    if (end.get("state") not in ("complete", "cancelled", "failed", "incomplete")
            or end.get("completeness") not in ("unknown", "complete", "partial")
            or type(end.get("truncated")) is not bool
            or any(type(end.get(key)) is not int or not 0 <= end[key] < 2**64
                   for key in ("items", "logical_bytes", "errors"))
            or any(type(end.get(key)) is not int or not -(2**63) <= end[key] < 2**63
                   for key in ("producer_status", "transform_status", "transport_status"))):
        raise JashProtocolError("invalid native terminal fields")
    return value


class JashStructuredResult:
    def __init__(self, response, framing, stdout, stderr, value, terminal=None):
        self.response, self.framing = response, framing
        self.stdout, self.stderr = stdout, stderr
        self.value, self.terminal = value, terminal

    @property
    def rc(self):
        return self.response["rc"]

    @property
    def native_complete(self):
        if self.terminal is None:
            return None
        end = self.terminal
        return (self.rc == 0 and end.get("state") == "complete"
                and end.get("completeness") == "complete" and end.get("truncated") is False
                and all(end.get(key) == 0 for key in ("errors", "producer_status", "transform_status", "transport_status")))

    def document(self, *, include_wire=False):
        result = {"schema": "jash.sdk.structured-result.v1", "framing": self.framing,
                  "rc": self.rc, "value": self.value}
        if self.terminal is not None:
            result.update(terminal=self.terminal, native_complete=self.native_complete)
            if include_wire:
                result["wire_b64"] = base64.b64encode(self.stdout).decode("ascii")
        return result


class _ExecutionResponse(dict):
    def __init__(self, response, captured):
        super().__init__(response)
        self._captured = captured


def _scope_token(path):
    flags = os.O_RDONLY | os.O_CLOEXEC | getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(path, flags)
    try:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid()
                or info.st_mode & 0o077):
            raise JashProtocolError("scope file has unsafe identity or permissions")
        raw = os.read(fd, 256)
        if len(raw) > 128:
            raise JashProtocolError("scope token exceeds budget")
        token = raw.strip().decode("ascii")
        if len(token) != 64 or any(c not in "0123456789abcdef" for c in token):
            raise JashProtocolError("invalid scope token")
        return token
    finally:
        os.close(fd)


class JashLocalClient:
    """Scoped connection; default requests carry *only* effect:read."""

    def __init__(self, socket_path, scope_file, timeout=5):
        self.socket_path = Path(socket_path)
        self.scope_file = Path(scope_file)
        self.timeout = timeout
        self._socket = None
        self._input = None
        self.capabilities = None

    def _receive(self):
        data = self._input.readline(MAX_FRAME + 1)
        if not data or len(data) > MAX_FRAME or not data.endswith(b"\n"):
            raise JashProtocolError("missing or incomplete JSONL response")
        response = json_loads_exact(data)
        if not isinstance(response, dict):
            raise JashProtocolError("unexpected response type")
        return response

    def _send(self, request):
        payload = json.dumps(request, separators=(",", ":"), ensure_ascii=True,
                             allow_nan=False).encode() + b"\n"
        if len(payload) > MAX_FRAME:
            raise ValueError("request exceeds the JSONL frame budget")
        self._socket.sendall(payload)

    def __enter__(self):
        token = _scope_token(self.scope_file)
        try:
            self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self._socket.settimeout(self.timeout)
            deadline = time.monotonic() + self.timeout
            while True:
                try:
                    self._socket.connect(str(self.socket_path))
                    break
                except (FileNotFoundError, ConnectionRefusedError):
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(.01)
            self._input = self._socket.makefile("rb", buffering=0)
            if self._receive().get("jash_local") != 1:
                raise JashProtocolError("unsupported local handshake")
            self._send({"scope": token})
            admitted = self._receive()
            if "error" in admitted or "schema_version" not in admitted:
                raise JashProtocolError("local scope denied or unsupported")
            self.capabilities = admitted
            return self
        except BaseException:
            self.close()
            raise

    def _execution(self, request_id):
        """Consume one v2 execution, including a mandatory result frame.

        Sequence numbers cover both output streams. A replay has no chunks:
        its refs remain explicit, never an empty or automatically fetched result.
        """
        started = replayed = False
        sequence = 0
        captured = {"stdout": bytearray(), "stderr": bytearray()}
        deadline = time.monotonic() + self.timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise JashProtocolError("execution response deadline exceeded", outcome_unknown=True)
            self._socket.settimeout(remaining)
            frame = self._receive()
            if frame.get("id") != request_id:
                raise JashProtocolError("response is not bound to the request ID", outcome_unknown=True)
            event = frame.get("event")
            if event is None and "error" in frame:
                if started and not replayed:
                    raise JashProtocolError("execution ended without a result frame", response=frame,
                                            outcome_unknown=True)
                return frame
            if type(frame.get("protocol_version")) is not int or frame["protocol_version"] != 2:
                raise JashProtocolError("unsupported execution frame", outcome_unknown=True)
            if event == "start":
                if started or ("replayed" in frame and type(frame["replayed"]) is not bool):
                    raise JashProtocolError("invalid repeated execution start", outcome_unknown=True)
                started = True
                replayed = frame.get("replayed", False)
            elif event == "chunk":
                if (not started or replayed or sequence >= 65536
                        or not isinstance(frame.get("stream"), str) or frame["stream"] not in captured
                        or type(frame.get("seq")) is not int or frame["seq"] != sequence):
                    raise JashProtocolError("invalid output chunk order", outcome_unknown=True)
                data = _base64_bytes(frame.get("data_b64"), 4096)
                target = captured[frame["stream"]]
                if not data or len(target) + len(data) > MAX_OUTPUT:
                    raise JashProtocolError("output exceeds collection bounds", outcome_unknown=True)
                target.extend(data)
                sequence += 1
            elif event == "result":
                if not started or type(frame.get("rc")) is not int:
                    raise JashProtocolError("result without execution start or status", outcome_unknown=True)
                if replayed:
                    return frame
                result = _ExecutionResponse(frame, {key: bytes(value) for key, value in captured.items()})
                output_bytes(result, "stdout")
                output_bytes(result, "stderr")
                return result
            else:
                raise JashProtocolError("unknown execution frame", outcome_unknown=True)

    def request(self, request_id, argv, *, caps=("effect:read",), input_bytes=None, stream=False):
        """Execute literal argv with explicit capabilities; never retry effects.

        Bytes arguments become the existing base64 argv representation. v2 is
        selected explicitly by stream or input_bytes and collects at most 16 MiB
        per stream, 65,536 chunks, and one timeout interval for the whole reply.
        """
        if self._socket is None:
            raise JashProtocolError("client is not connected")
        if not isinstance(request_id, str) or not request_id or "\x00" in request_id:
            raise ValueError("request_id must be a nonempty string without NUL")
        if not isinstance(argv, (list, tuple)) or not 1 <= len(argv) <= 64:
            raise ValueError("argv must contain 1 to 64 literal arguments")
        encoded_argv = []
        for item in argv:
            if isinstance(item, bytes) and b"\0" not in item:
                encoded_argv.append({"encoding": "base64", "data": base64.b64encode(item).decode("ascii")})
            elif isinstance(item, str) and "\0" not in item:
                item.encode("utf-8", "strict")
                encoded_argv.append(item)
            else:
                raise ValueError("argv arguments must be text or bytes without NUL")
        if not isinstance(caps, (list, tuple)) or not all(
                isinstance(cap, str) for cap in caps):
            raise ValueError("caps must be a string array")
        if type(stream) is not bool or (input_bytes is not None and
                (not isinstance(input_bytes, bytes) or len(input_bytes) > MAX_STDIN)):
            raise ValueError("invalid streaming flag or stdin byte budget")
        request = {"id": request_id, "argv": encoded_argv, "caps": list(caps)}
        if stream or input_bytes is not None:
            request["protocol_version"] = 2
            if input_bytes is not None:
                request["stdin_b64"] = base64.b64encode(input_bytes).decode("ascii")
        try:
            self._send(request)
            answer = self._execution(request_id) if "protocol_version" in request else self._receive()
            if answer.get("id") != request_id:
                raise JashProtocolError("response is not bound to the request ID", outcome_unknown=True)
            self._socket.settimeout(self.timeout)
            return answer
        except (JashProtocolError, OSError) as error:
            self.close()
            if isinstance(error, JashProtocolError):
                error.outcome_unknown = True
                raise
            raise JashProtocolError("execution transport failed; outcome may be unknown",
                                    outcome_unknown=True) from error

    def structured(self, request_id, argv, *, framing, caps=("effect:read",),
                   input_bytes=None, native_decoder=None):
        """Decode an explicitly selected semantic mode, preserving command rc.

        No automatic mode inference, retry, ref fetch, or capability elevation.
        JSON syntax is strict, integers exact, fractions Decimal. Native input
        is validated by the caller-selected runtime's production JTV2 importer;
        its JSON projection and original bytes are both retained.
        """
        if framing not in ("json", "jsonl", "jtv2"):
            raise ValueError("unsupported structured framing")
        if framing == "jtv2" and native_decoder is None:
            raise ValueError("native_decoder is required for JTV2")
        answer = self.request(request_id, argv, caps=caps, input_bytes=input_bytes, stream=True)
        if type(answer.get("rc")) is not int or "error" in answer:
            raise JashCommandError(answer)
        stdout, stderr = output_bytes(answer), output_bytes(answer, "stderr")
        if not stdout and answer["rc"] != 0:
            raise JashCommandError(answer)
        try:
            if framing == "jtv2":
                projection = native_projection(stdout, native_decoder, timeout=self.timeout)
                return JashStructuredResult(answer, framing, stdout, stderr,
                                            projection["items"], projection["terminal"])
            if framing == "json":
                value = json_loads_exact(stdout)
            else:
                value = [json_loads_exact(line) for line in stdout.splitlines() if line.strip()]
            return JashStructuredResult(answer, framing, stdout, stderr, value)
        except JashProtocolError as error:
            error.response = answer
            raise

    def control(self, request_id, operation, *, caps=(), **fields):
        """Explicit v2 data/job request; no implicit authority or FD lane."""
        if self._socket is None:
            raise JashProtocolError("client is not connected")
        if (not isinstance(request_id, str) or not 1 <= len(request_id) <= 47 or
                any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"
                    for c in request_id)):
            raise ValueError("invalid data-plane request ID")
        if operation not in DATA_FIELDS or set(fields) - DATA_FIELDS[operation]:
            raise ValueError("unsupported data-plane operation or field")
        if not isinstance(caps, (list, tuple)) or not all(
                isinstance(cap, str) for cap in caps):
            raise ValueError("caps must be a string array")
        self._send({"id": request_id, "protocol_version": 2,
                    "op": operation, "caps": list(caps), **fields})
        answer = self._receive()
        if answer.get("id") != request_id:
            raise JashProtocolError("data-plane response is not bound to the request ID")
        return answer

    def close(self):
        if self._input is not None:
            self._input.close()
            self._input = None
        if self._socket is not None:
            self._socket.close()
            self._socket = None

    def __exit__(self, *_):
        self.close()
