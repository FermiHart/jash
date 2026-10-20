"""Provider-independent read-only tools over one bounded jash script batch.

Adapters normalize model tool-call envelopes only. They cannot choose shell
commands or capabilities. The compiled `? tool` dispatcher makes the final
read-only admission decision and retains volatile `o:` refs within each batch.
"""

import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import subprocess
import time


OPERATIONS = frozenset(("fs.list", "fs.count", "fs.observation.read",
                        "fs.source.read"))
FIELDS = frozenset(("root", "type", "uid", "ref", "path", "offset", "limit"))
ID = re.compile(r"[A-Za-z0-9._-]{1,47}\Z", re.ASCII)


class ToolError(RuntimeError):
    pass


def _normalize(ident, name, arguments):
    if (not isinstance(ident, str) or ID.fullmatch(ident) is None or
            name not in OPERATIONS or not isinstance(arguments, dict) or
            set(arguments) - FIELDS):
        raise ToolError("invalid tool call envelope")
    return {"version": 1, "id": ident, "op": name, **arguments}


def openai_tool_call(call):
    """Normalize one OpenAI-style function tool call without executing it."""
    if not isinstance(call, dict) or not isinstance(call.get("function"), dict):
        raise ToolError("invalid OpenAI tool call")
    fn = call["function"]
    try:
        arguments = json.loads(fn["arguments"])
    except (KeyError, TypeError, ValueError) as error:
        raise ToolError("invalid OpenAI tool arguments") from error
    return _normalize(call.get("id"), fn.get("name"), arguments)


def anthropic_tool_use(call):
    """Normalize one Anthropic-style tool_use block without executing it."""
    if not isinstance(call, dict) or call.get("type") != "tool_use":
        raise ToolError("invalid Anthropic tool_use")
    return _normalize(call.get("id"), call.get("name"), call.get("input"))


def run_tool_loop(session, provider_next, adapter, *, max_calls=8):
    """Drive a bounded read-only loop with a caller-owned model transport.

    `provider_next(prior_results)` yields one provider tool envelope or None
    for a terminal answer. This layer does not choose a model, perform HTTP,
    or accept shell commands. Results remain available for the caller to feed
    back to that transport on the next turn. An interactive session is required
    so volatile observation references never silently cross a process boundary.
    """
    if not isinstance(session, InteractiveToolSession):
        raise ToolError("multi-turn tools require a live Jash session")
    if not callable(provider_next) or not callable(adapter) or \
            isinstance(max_calls, bool) or not isinstance(max_calls, int) or \
            not 1 <= max_calls <= 8:
        raise ValueError("tool loop requires callbacks and a budget of 1..8")
    outcomes = []
    for turn in range(max_calls + 1):
        envelope = provider_next(tuple(outcomes))
        if envelope is None:
            return outcomes
        if turn == max_calls:
            raise ToolError("tool call budget exceeded")
        outcomes.append(session.call(adapter(envelope)))
    raise ToolError("unreachable tool loop state")


class ToolSession:
    def __init__(self, binary, *, cwd, env=None, timeout=20):
        self.binary = str(Path(binary).resolve())
        self.cwd = cwd
        self.env = dict(os.environ if env is None else env)
        self.timeout = timeout

    def __enter__(self):
        return self

    def _validate(self, request):
        if not isinstance(request, dict):
            raise ToolError("tool request is not an object")
        if set(request) - {"version", "id", "op"} - FIELDS:
            raise ToolError("unrecognized tool request field")
        if request.get("version") != 1 or isinstance(request.get("version"), bool):
            raise ToolError("unsupported tool request version")
        if not isinstance(request.get("id"), str) or ID.fullmatch(request["id"]) is None:
            raise ToolError("invalid tool request ID")
        if request.get("op") not in OPERATIONS:
            raise ToolError("tool request is not read-only")
        payload = json.dumps(request, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        if len(payload) > 4096:
            raise ToolError("tool request exceeds model budget")
        return b"? tool " + payload + b"\n"

    def run(self, requests):
        """Run a fixed batch in one jash process, so `o:` stays session-local.

        A noninteractive jash may wait for stdin EOF before evaluating a script.
        This is a bounded batch, not an interactive multi-turn model loop.
        """
        if not isinstance(requests, (tuple, list)) or not requests or len(requests) > 32:
            raise ToolError("batch needs 1..32 read-only requests")
        payload = b"? on\n" + b"".join(self._validate(request) for request in requests)
        try:
            executed = subprocess.run(
                [self.binary], cwd=self.cwd, env=self.env, input=payload,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                timeout=self.timeout)
        except subprocess.TimeoutExpired as error:
            raise ToolError("tool batch timed out") from error
        if b"ON -- modulo carregado" not in executed.stdout:
            raise ToolError("jash AI module was not loaded")
        results = []
        for line in executed.stdout.splitlines():
            if not line.startswith(b'{"schema":"jash.ai-tool-result.v1"'):
                continue
            try:
                result = json.loads(line)
            except (ValueError, UnicodeDecodeError) as error:
                raise ToolError("invalid tool result") from error
            results.append(result)
        if len(results) != len(requests) or executed.returncode:
            raise ToolError("tool batch refused or did not complete")
        for request, answer in zip(requests, results):
            if (answer.get("schema") != "jash.ai-tool-result.v1" or
                    answer.get("id") != request["id"] or
                    not isinstance(answer.get("result"), dict)):
                raise ToolError("unbound or malformed tool result")
        return results

    def call(self, request):
        return self.run([request])[0]

    def close(self):
        pass

    def __exit__(self, *_):
        self.close()


class InteractiveToolSession(ToolSession):
    """Live PTY tool loop for dependent observations; Linux-qualified only.

    A PTY is needed because the interactive Jash processes submitted commands
    without waiting for stdin EOF. No shell command is formed from tool args:
    the JSON goes to the same compiled `? tool` admission boundary.
    """

    def __init__(self, binary, *, cwd, env=None, timeout=20):
        super().__init__(binary, cwd=cwd, env=env, timeout=timeout)
        self._pid = None
        self._fd = None
        self._pending = bytearray()

    def _until(self, marker):
        deadline = time.monotonic() + self.timeout
        while True:
            index = self._pending.find(marker)
            if index >= 0:
                data = bytes(self._pending[:index + len(marker)])
                del self._pending[:index + len(marker)]
                return data
            if b"[ai] tool observation unavailable" in self._pending or \
                    b"[ai] tool request denied" in self._pending:
                raise ToolError("native tool refused or failed the request")
            if len(self._pending) > 65536:
                raise ToolError("interactive tool output exceeds budget")
            ready, _, _ = select.select([self._fd], [], [],
                                        max(0, deadline - time.monotonic()))
            if not ready:
                raise ToolError("interactive tool response timed out")
            try:
                data = os.read(self._fd, 8192)
            except OSError as error:
                raise ToolError("interactive Jash exited") from error
            if not data:
                raise ToolError("interactive Jash exited")
            self._pending.extend(data)

    def __enter__(self):
        self._pid, self._fd = pty.fork()
        if self._pid == 0:
            try:
                os.chdir(self.cwd)
                os.environ.clear()
                os.environ.update(self.env)
                os.execv(self.binary, [self.binary])
            except BaseException:
                os._exit(127)
        try:
            self._until(b"$")
            os.write(self._fd, b"? on\r")
            self._until(b"ON -- modulo carregado")
            return self
        except BaseException:
            self.close()
            raise

    def call(self, request):
        if self._pid is None or self._fd is None:
            raise ToolError("interactive tool session is not running")
        payload = self._validate(request)
        framed = payload[:-1] + b"\r"
        if os.write(self._fd, framed) != len(framed):
            raise ToolError("partial interactive tool request")
        marker = (b'{"schema":"jash.ai-tool-result.v1","id":"' +
                  request["id"].encode("ascii") + b'"')
        self._until(marker)
        line = marker + self._until(b"\n")
        try:
            answer = json.loads(line)
        except (UnicodeDecodeError, ValueError) as error:
            raise ToolError("invalid interactive tool result") from error
        if (answer.get("schema") != "jash.ai-tool-result.v1" or
                answer.get("id") != request["id"] or
                not isinstance(answer.get("result"), dict)):
            raise ToolError("unbound interactive tool result")
        return answer

    def close(self):
        if self._pid is None:
            return
        if self._fd is not None:
            try:
                os.write(self._fd, b"\x04")
            except OSError:
                pass
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            done, _ = os.waitpid(self._pid, os.WNOHANG)
            if done:
                break
            time.sleep(.02)
        else:
            try:
                os.kill(self._pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                done, _ = os.waitpid(self._pid, os.WNOHANG)
                if done:
                    break
                time.sleep(.02)
            else:
                try:
                    os.kill(self._pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                os.waitpid(self._pid, 0)
        if self._fd is not None:
            os.close(self._fd)
        self._fd = None
        self._pid = None
        self._pending.clear()
