#!/usr/bin/env python3
"""Bounded MCP stdio adapter for Jash's compiled read-only tools.

One PTY Jash session is retained across MCP tool calls. An `o:` ref works
only within that process; it is not a durable `a:` artifact or authority.
With explicit --socket and --scope-file, expose the supplied runtime's builtin
catalog and semantic query variants through its existing local API instead.
No mutation authority, shell eval, daemon startup, or implicit ref fetch occurs.
"""

import argparse
import json
from pathlib import Path
import sys

from jash_tools import InteractiveToolSession, ToolError
from jash_client import (JashLocalClient, JashProtocolError, JashCommandError,
                         builtin_catalog, json_dumps_exact, _base64_bytes)


PROTOCOL = "2025-06-18"
MAX_REQUEST = 8192
TOOLS = [
    {
        "name": name,
        "description": description,
        "inputSchema": {
            "type": "object",
            "properties": {
                "root": {"type": "string"},
                "type": {"type": "string", "enum": [
                    "file", "dir", "symlink", "fifo", "socket", "block", "char", "unknown"]},
                "uid": {"type": "integer", "minimum": 0, "maximum": 4294967295},
            },
            "required": ["root"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": True, "destructiveHint": False},
    }
    for name, description in (
        ("fs.list", "Bounded typed listing with session-local verified JTV1 ref"),
        ("fs.count", "Count bounded native filesystem rows without materializing a ref"),
    )
]
TOOLS.append({
    "name": "fs.observation.read",
    "description": "Page a session-local sealed observation reference",
    "inputSchema": {
        "type": "object",
        "properties": {
            "ref": {"type": "string"},
            "offset": {"type": "integer", "minimum": 0},
            "limit": {"type": "integer", "minimum": 1, "maximum": 1024},
        },
        "required": ["ref"],
        "additionalProperties": False,
    },
    "annotations": {"readOnlyHint": True, "destructiveHint": False},
})

BUILTIN_TOOLS = [
    {"name": "builtin.catalog",
     "description": "Page the supplied runtime's compiled builtin contracts, including partial/missing modes",
     "inputSchema": {"type": "object", "properties": {
         "name": {"type": "string"}, "offset": {"type": "integer", "minimum": 0},
         "limit": {"type": "integer", "minimum": 1, "maximum": 64}},
         "additionalProperties": False},
     "annotations": {"readOnlyHint": True, "destructiveHint": False}},
    {"name": "builtin.query",
     "description": "Run a catalogued semantic builtin variant with effect:read only; JTV2 includes verified terminal and original bytes",
     "inputSchema": {"type": "object", "properties": {
         "name": {"type": "string"}, "mode": {"enum": ["json", "data"]},
         "variant": {"type": "string"},
         "args": {"type": "array", "maxItems": 62, "items": {"type": "string"}},
         "stdin_b64": {"type": "string", "contentEncoding": "base64"}},
         "required": ["name", "mode", "variant"], "additionalProperties": False},
     "annotations": {"readOnlyHint": True, "destructiveHint": False}},
]
TOOLS.append({
    "name": "fs.source.read",
    "description": "Read a bounded source page from an admitted C/C++ basename",
    "inputSchema": {
        "type": "object",
        "properties": {
            "path": {"type": "string"},
            "offset": {"type": "integer", "minimum": 0},
            "limit": {"type": "integer", "minimum": 1, "maximum": 1024},
        },
        "required": ["path"],
        "additionalProperties": False,
    },
    "annotations": {"readOnlyHint": True, "destructiveHint": False},
})


class ToolRuntime:
    def __init__(self, binary, socket_path=None, scope_file=None, native_decoder=None):
        self.binary = binary
        self.session = None
        self.initialized = False
        if (socket_path is None) != (scope_file is None):
            raise ValueError("--socket and --scope-file must be supplied together")
        self.socket_path, self.scope_file = socket_path, scope_file
        self.client = self.catalog = None
        self.sequence = 0
        self.native_decoder = native_decoder

    @property
    def tools(self):
        return BUILTIN_TOOLS if self.socket_path is not None else TOOLS

    def builtin_call(self, name, args):
        if self.client is None:
            self.catalog = builtin_catalog(self.binary)
            self.client = JashLocalClient(self.socket_path, self.scope_file, timeout=10)
            try:
                self.client.__enter__()
                if self.catalog.get("profile") != self.client.capabilities.get("profile"):
                    raise JashProtocolError("catalog binary does not match the listener profile")
            except BaseException:
                self.close()
                raise
        operations = [item for item in self.catalog["operations"] if item.get("kind") == "builtin"]
        if name == "builtin.catalog":
            if "name" in args:
                operations = [item for item in operations if item["name"] == args["name"]]
            offset, limit = args.get("offset", 0), args.get("limit", 20)
            selected = operations[offset:offset + limit]
            result = {key: self.catalog[key] for key in ("schema", "version", "profile", "target", "complete", "coverage")}
            result.update(provenance="caller-supplied-binary", operations=selected,
                          offset=offset, total=len(operations),
                          next_offset=offset + len(selected) if offset + len(selected) < len(operations) else None)
            return result, False
        operation = next((item for item in operations if item["name"] == args["name"]), None)
        if operation is None:
            raise ValueError("builtin is absent from the supplied runtime catalog")
        mode = operation.get("structured_modes", {}).get(args["mode"], {})
        variant = next((item for item in mode.get("variants", []) if item["id"] == args["variant"]), None)
        if variant is None or variant.get("framing") not in ("json", "jsonl", "jtv2"):
            raise ValueError("structured variant is absent or its framing is unsupported")
        if variant["framing"] == "jtv2" and self.native_decoder is None:
            raise ValueError("--native-decoder is required for native queries")
        selector = variant.get("selector")
        argv = [operation["name"]] + ([selector] if selector is not None else []) + args.get("args", [])
        stdin = _base64_bytes(args["stdin_b64"], MAX_REQUEST) if "stdin_b64" in args else None
        self.sequence += 1
        result = self.client.structured("mcp-builtin-" + str(self.sequence), argv,
                                       framing=variant["framing"], input_bytes=stdin,
                                       native_decoder=self.native_decoder, caps=("effect:read",))
        document = result.document(include_wire=True)
        document["contract"] = {"builtin": operation["name"], "mode": args["mode"],
                                "variant": variant["id"], "complete": variant.get("complete", False),
                                "qualification": mode.get("qualification", "pending")}
        return document, result.rc != 0 or result.native_complete is False

    def call(self, request):
        try:
            if self.session is None:
                self.session = InteractiveToolSession(self.binary, cwd=Path.cwd())
                self.session.__enter__()
            return self.session.call(request)
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.session is not None:
            self.session.close()
            self.session = None
        if self.client is not None:
            self.client.close()
            self.client = None


def error(ident, code, message):
    return {"jsonrpc": "2.0", "id": ident,
            "error": {"code": code, "message": message}}


def strict_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def invalid_constant(value):
    raise ValueError("non-JSON numeric constant")


def dispatch(request, runtime):
    if not isinstance(request, dict) or request.get("jsonrpc") != "2.0":
        return error(None, -32600, "Invalid Request")
    if "id" not in request:
        # notifications/initialized has no response on stdio.
        return None
    ident = request["id"]
    if isinstance(ident, bool) or not isinstance(ident, (str, int)):
        return error(None, -32600, "Invalid Request")
    method = request.get("method")
    if method == "initialize":
        params = request.get("params")
        if (not isinstance(params, dict) or
                not isinstance(params.get("protocolVersion"), str) or
                not params["protocolVersion"] or
                not isinstance(params.get("capabilities"), dict) or
                not isinstance(params.get("clientInfo"), dict) or
                not isinstance(params["clientInfo"].get("name"), str) or
                not isinstance(params["clientInfo"].get("version"), str)):
            return error(ident, -32602, "Invalid initialize parameters")
        runtime.initialized = True
        result = {"protocolVersion": PROTOCOL,
                  "capabilities": {"tools": {"listChanged": False}},
                  "serverInfo": {"name": "jash-readonly", "version": "0.1"}}
    elif method == "tools/list":
        if not runtime.initialized:
            return error(ident, -32000, "Not initialized")
        result = {"tools": runtime.tools}
    elif method == "tools/call":
        if not runtime.initialized:
            return error(ident, -32000, "Not initialized")
        params = request.get("params")
        if runtime.socket_path is not None:
            if (not isinstance(params, dict) or params.get("name") not in ("builtin.catalog", "builtin.query")
                    or not isinstance(params.get("arguments"), dict)):
                return error(ident, -32602, "Unknown tool or invalid arguments")
            args, name = params["arguments"], params["name"]
            if name == "builtin.catalog":
                invalid = (set(args) - {"name", "offset", "limit"}
                           or ("name" in args and (not isinstance(args["name"], str) or not args["name"]))
                           or ("offset" in args and (type(args["offset"]) is not int or not 0 <= args["offset"] <= 1048576))
                           or ("limit" in args and (type(args["limit"]) is not int or not 1 <= args["limit"] <= 64)))
            else:
                invalid = (set(args) - {"name", "mode", "variant", "args", "stdin_b64"}
                           or not isinstance(args.get("name"), str) or not args["name"]
                           or args.get("mode") not in ("json", "data")
                           or not isinstance(args.get("variant"), str) or not args["variant"]
                           or not isinstance(args.get("args", []), list) or len(args.get("args", [])) > 62
                           or any(not isinstance(item, str) or "\0" in item for item in args.get("args", []))
                           or ("stdin_b64" in args and not isinstance(args["stdin_b64"], str)))
            if invalid:
                return error(ident, -32602, "Unknown tool or invalid arguments")
            try:
                native, failed = runtime.builtin_call(name, args)
                encoded = json_dumps_exact(native)
                if len(encoded) > 4 * 1024 * 1024:
                    raise JashProtocolError("MCP structured result exceeds 4 MiB budget")
            except (JashProtocolError, JashCommandError, OSError, ValueError) as failure:
                if isinstance(failure, (JashProtocolError, OSError)):
                    runtime.close()
                response = getattr(failure, "response", None)
                details = {"schema": "jash.mcp.error.v1", "error": str(failure),
                           "outcome_unknown": getattr(failure, "outcome_unknown", False)}
                if isinstance(response, dict):
                    details.update({key: response[key] for key in ("rc", "error", "outcome") if key in response})
                return {"jsonrpc": "2.0", "id": ident, "result": {
                    "content": [{"type": "text", "text": json_dumps_exact(details)}],
                    "structuredContent": details, "isError": True}}
            return {"jsonrpc": "2.0", "id": ident, "result": {
                "content": [{"type": "text", "text": encoded}],
                "structuredContent": native, "isError": failed}}
        if not isinstance(params, dict) or params.get("name") not in (
                "fs.list", "fs.count", "fs.observation.read", "fs.source.read") or \
                not isinstance(params.get("arguments"), dict):
            return error(ident, -32602, "Unknown tool or invalid arguments")
        args = params["arguments"]
        if params["name"] in ("fs.observation.read", "fs.source.read"):
            field = "ref" if params["name"] == "fs.observation.read" else "path"
            if (set(args) - {field, "offset", "limit"} or
                    not isinstance(args.get(field), str) or not args[field] or
                    len(args[field]) >= 128 or
                    ("offset" in args and (isinstance(args["offset"], bool) or
                                           not isinstance(args["offset"], int) or
                                           not 0 <= args["offset"] <= 9007199254740991)) or
                    ("limit" in args and (isinstance(args["limit"], bool) or
                                          not isinstance(args["limit"], int) or
                                          not 1 <= args["limit"] <= 1024))):
                return error(ident, -32602, "Unknown tool or invalid arguments")
        else:
            if (set(args) - {"root", "type", "uid"} or
                    not isinstance(args.get("root"), str) or not args["root"] or
                    len(args["root"].encode("utf-8")) >= 1024 or
                    ("type" in args and args["type"] not in TOOLS[0]["inputSchema"]["properties"]["type"]["enum"]) or
                    ("uid" in args and (isinstance(args["uid"], bool) or
                                        not isinstance(args["uid"], int) or
                                        not 0 <= args["uid"] <= 4294967295))):
                return error(ident, -32602, "Unknown tool or invalid arguments")
        request_id = "mcp-1"
        try:
            result_request = {"version": 1, "id": request_id,
                              "op": params["name"], **args}
            native = runtime.call(result_request)["result"]
        except (ToolError, OSError, ValueError):
            return {"jsonrpc": "2.0", "id": ident,
                    "result": {"content": [{"type": "text",
                                             "text": "Native observation unavailable or incomplete"}],
                               "isError": True}}
        result = {"content": [{"type": "text",
                                "text": json.dumps(native, ensure_ascii=False, separators=(",", ":"))}],
                  "structuredContent": native, "isError": False}
    else:
        return error(ident, -32601, "Method not found")
    return {"jsonrpc": "2.0", "id": ident, "result": result}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jash", required=True)
    parser.add_argument("--socket")
    parser.add_argument("--scope-file")
    parser.add_argument("--native-decoder", help="trusted jash binary supporting --exec-json (required for JTV2 queries)")
    options = parser.parse_args()
    if (options.socket is None) != (options.scope_file is None):
        parser.error("--socket and --scope-file must be supplied together")
    binary = Path(options.jash).resolve()
    runtime = ToolRuntime(binary, options.socket, options.scope_file, options.native_decoder)
    try:
        while True:
            line = sys.stdin.buffer.readline(MAX_REQUEST + 1)
            if not line:
                break
            if len(line) > MAX_REQUEST or not line.endswith(b"\n"):
                while line and not line.endswith(b"\n"):
                    line = sys.stdin.buffer.readline(MAX_REQUEST + 1)
                response = error(None, -32600, "Request exceeds frame budget")
            else:
                try:
                    request = json.loads(line, object_pairs_hook=strict_pairs,
                                         parse_constant=invalid_constant)
                except (UnicodeDecodeError, ValueError):
                    response = error(None, -32700, "Parse error")
                else:
                    response = dispatch(request, runtime)
            if response is not None:
                sys.stdout.write(json_dumps_exact(response) + "\n")
                sys.stdout.flush()
    finally:
        runtime.close()


if __name__ == "__main__":
    main()
