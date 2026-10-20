# Scoped Python client for jash-agentd (Linux)

`integrations/jash_client.py` connects to the existing `jash-agentd --listen`
Unix socket. The caller starts the service and owns the 0600 scope file:

```python
from integrations.jash_client import JashLocalClient

with JashLocalClient("/run/user/1000/jash.sock", "/run/user/1000/jash.scope") as client:
    result = client.request("cwd-1", ["pwd"])  # only effect:read by default
    print(result["rc"], result["stdout"])
    # Additional authority is explicit and still checked by jash-agentd:
    # client.request("make-1", ["mkdir", "example"], caps=("effect:mutate",))
```

The SDK enforces a bounded JSONL frame, verifies the local greeting, checks
scope file owner and permissions, and binds each response to its request ID.
The ordinary `request` method keeps the existing response envelope. Explicit
structured methods below validate the chosen format. The client starts no
daemon, confers no extra capabilities, and never promotes an `o:` ref.

The separate `control(id, op, caps=(...), **fields)` method admits only the
existing version-2 `data.import`, `data.promote`, `data.attach`, and `data.read`
operations. The default capability tuple is empty: import needs `data:create`,
promotion needs `data:manage`, and attach/read need `data:read`. For example,
`control("import-1", "data.import", caps=("data:create",),
transport="inline", data_b64="YWJj")` imports an explicitly supplied blob.
Promote that returned volatile `artifact_ref` with a stable new request ID,
then attach the returned durable `a:` ref before `data.read`. Exact promotion
replay is enforced by the daemon, not a client-side guess. This path does not
turn session-local `o:` refs into `a:` without the separately authorized FD
handoff from `Docs/data/AI_TYPED_OBSERVATIONS.md`.

The same explicit `control` method admits existing `job.start`, `job.get`,
`job.wait`, `job.read`, `job.result`, and `job.cancel` requests. A background
`job.start` requires **both** `effect:exec` and `job:create`; `job.wait/read`
need `job:read`. A restarted daemon can answer `job.result` for a retained
terminal job: its archival envelope has `available`, `response_json` and a
terminal receipt, not a top-level `rc`. No job capability is granted by the
client default, and this narrow Python adapter is not a full job scheduler.

## Semantic JSON and JTV2 builtins

`structured` selects protocol v2, collects the ordered stdout/stderr chunks,
and requires a bound result frame with exact byte counts. The framing is an
explicit caller argument; human output is never inferred to be structured:

```python
from integrations.jash_client import JashLocalClient

with JashLocalClient(socket_path, scope_file, timeout=10) as client:
    total = client.structured(
        "sum-1", ["sum", "n"], framing="jsonl",
        input_bytes=b'{"n":0.1}\n{"n":0.2}\n')
    assert str(total.value[0]["sum"]) == "0.3"  # Decimal, not binary float

    files = client.structured(
        "stat-1", ["stat", "--data", "example"], framing="jtv2",
        native_decoder="./build/jash")
    assert files.native_complete
    selected = client.structured(
        "filter-1", ["where", "--data", "size>0"], framing="jtv2",
        input_bytes=files.stdout, native_decoder="./build/jash")
```

`json` and `jsonl` reject duplicate keys, invalid UTF-8, unpaired surrogates,
nonfinite numbers and incomplete documents. Python integers retain their exact
value, and fractional/exponent JSON numbers use `decimal.Decimal`.
`json_dumps_exact` serializes those Decimal values without a float conversion.
An argv argument may be `bytes`; the SDK sends the existing canonical base64
argv object and rejects decoded NUL. `input_bytes` is raw stdin, including NUL.

For JTV2, the explicitly selected trusted **general `jash` binary** invokes
`json --inspect-data` in an isolated HOME/cwd. The production C importer verifies
schemas, framing, item credits, the ordered hash and terminal before the SDK
returns a projection. `jash-agentd` deliberately has no `--exec-json` entry
point; use `build/jash` as `native_decoder`. The projection preserves booleans,
null, nested values, exact integers and tagged bytes. Paths, symbols and unit
kinds are projected into JSON, so `result.stdout` retains the original JTV2
bytes for composition; projecting to JSON is not a lossless native round trip.

`result.rc`, `result.terminal` and `result.native_complete` are distinct. A valid
FAILED/CANCELLED/INCOMPLETE stream remains inspectable with
`native_complete == False`. Admission failures raise `JashCommandError` with
the original response. A malformed transport or truncated stream raises
`JashProtocolError`, closes the connection when ordering is uncertain, and
never retries the request. Preserve the request ID and receipt when resolving
an uncertain effect. Replayed v2 responses contain retained refs without
replayed chunks: `output_bytes` explicitly refuses to treat a head/ref as full
output or follow a path implicitly.

Limits are one MiB per JSONL frame, 16 MiB collected per output stream, 65,536
chunks, 64 argv arguments, 192 KiB minus one byte stdin, and one configured
timeout interval for a v2 reply. The decoder has its documented C item/arena
bounds as well. A larger native result must use an explicitly supported data
transport; this SDK does not silently spool, truncate or promote it.

## Read-only typed tool batch

`integrations/jash_tools.py` is a separate adapter for the Jash `? tool`
surface, not an agentd mutation client:

```python
from integrations.jash_tools import ToolSession, openai_tool_call

request = openai_tool_call({
    "id": "count-1", "function": {
        "name": "fs.count", "arguments": '{"root":".","type":"file"}'}}
)
with ToolSession("./build/jash", cwd=".") as tools:
    result = tools.call(request)
    print(result["result"]["items_seen"])
```

An Anthropic-style `tool_use` block uses `anthropic_tool_use`; a client with no
model can construct the version-1 request directly. `run([requests...])`
evaluates at most 32 calls in **one noninteractive script**; `fs.list` and
`fs.observation.read latest` must be in the same batch for the volatile `o:`
reference to remain valid. `call` runs a separate process for each invocation,
so its `o:` reference cannot be resumed by another `call`.
`InteractiveToolSession` instead keeps a Linux PTY Jash process alive and
supports successive `call()` operations: list now, inspect its returned
observation, then decide which bounded page to read. Both normalized provider
envelopes and model-free requests use the same local dispatcher. The PTY
adapter does not contact a model API, supply mutation capabilities or make a
volatile ref durable; terminate the session to release it.
`run_tool_loop(live_session, provider_next, openai_tool_call)` (or
`anthropic_tool_use`) lets a caller-supplied transport yield up to eight tool
envelopes and inspect prior typed results before each next turn. `None`
terminates the loop. The transport owns its model credentials and follow-up
messages; this function only dispatches admitted read-only operations. It
does not execute a model's text or infer shell commands from model output.

## MCP stdio (read-only slice)

`python3 -B integrations/jash_mcp.py --jash ./build/jash` exposes MCP
`2025-06-18` `initialize`, `tools/list` and `tools/call` over newline-delimited
JSON-RPC on stdio. It offers only `fs.count`, `fs.list` and
`fs.observation.read`/`fs.source.read` with bounded inputs. An interactive Jash PTY process
stays alive for the MCP session: `fs.list` yields an `o:` reference, and a later
`fs.observation.read` within **that same MCP process** can page it. Exiting,
restarting or replacing the underlying observation invalidates the ref. This
is not a server for mutation tools, a durable cross-process observation lease,
or full W29 qualification.
The MCP process runs with the caller's filesystem read authority; avoid
launching it under a broader OS account than the client should use.

### Compiled builtin catalog and scoped queries

The optional scoped mode uses an already running listener and a caller-owned
scope file. It exposes `builtin.catalog` and `builtin.query` instead of the
four PTY tools:

```sh
python3 -B integrations/jash_mcp.py \
  --jash ./build/jash-agentd --native-decoder ./build/jash \
  --socket /run/user/1000/jash.sock --scope-file /run/user/1000/jash.scope
```

`--jash` supplies the compiled catalog, whose profile must match the listener
handshake; use binaries from the same build. `--native-decoder` is required
for JTV2 queries and runs only the fixed inspection command. The adapter does
not start the listener or infer its scope. Catalog pages retain the actual
`complete`, `qualification`, missing modes and variant limits; discovery is
not a claim of global qualification. Pages default to 20 builtins and allow
at most 64, or an exact `name` filter.

`builtin.query` takes `name`, `mode` (`json`/`data`), a catalogued `variant`,
optional literal `args`, and optional canonical `stdin_b64`. It prepends only
that variant's declared selector. The handler validates the actual option
grammar and the daemon authorizes the final argv. Every MCP query carries
**only `effect:read`**, including attempts to select a mutation report; there
is no capability override. Valid native results include the inspected
terminal and original `wire_b64`; `isError` is true for failed or incomplete
native results. MCP request frames are bounded to 8 KiB and semantic output
documents to 4 MiB, with explicit errors rather than truncated results.

Regression gates: `tests/test_structured_sdk.py` covers codec negative controls,
the real C importer, scoped query/filter/apply/verify with separate explicit
SDK mutation authority, large chunks/replay, and read-only MCP. The previous
`tests/test_python_local_client.py` and `tests/test_jash_mcp.py` retain their
ordinary local API and four-tool PTY contracts.
