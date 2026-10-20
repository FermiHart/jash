# Builtin Semantic Audit

`builtin_audit.py` reads the live registry from `build/jash --builtins-json` and
executes an explicit semantic probe for every registered name. The audit is
complete only when the registry reports 171 names and the report contains:

```text
registered=171 audited=171 executed=171 failed=0
```

Run the complete audit with:

```bash
python3 tests/builtin_audit.py \
  --json /tmp/jash-builtin-audit.json \
  --csv /tmp/jash-builtin-audit.csv
```

`--max-runs` is available only for intentionally incomplete diagnostic runs;
the resulting `UNPROBED_LIMIT` records make the command fail. A missing fixture
is likewise `UNPROBED`, never coverage. There are no help-only probes and no
`SKIP_GUARDED` coverage records.

Each record and case contains `executed`, the actual `probe` script and argv,
the expected exit/signal/timeout contract, observed exit/signal/timeout,
stdout, stderr, filesystem mutations, and sampled descendants. `PASS` requires
the contract to match. `GUARDED_EXECUTED` is reserved for a builtin that itself
rejects or confirms a potentially destructive request. `OBSERVED_SUSPEND` is
used only when `suspend` is actually seen stopped and the harness then kills
its process group.

Fixtures create a fresh private case tree, private `HOME` and `TMPDIR`, a
minimal environment, a new process group, CPU/address-space/file/resource
limits, a wall timeout, filesystem snapshots, and `/proc` descendant sampling.
System and agent probes therefore operate only on temporary state. Mutators
use files under the case directory; `cd`, `source`, `.`, `exec`, `return`,
`break`, `continue`, jobs, `kill -0`, `timeout`, and the file operations are
executed rather than represented by `--help`.

Contract-sensitive probes use implemented operations: `glob --json '*.txt'`,
`digest --no-cache .`, `gossip --channel audit send audit-message`, and
`forge --json 'audit scaffold'`. The `json git status` probe first runs
`git init -q` in that fixture's private working directory, never in the
repository that hosts the audit.

No registered shutdown or reboot builtin exists in the current 171-name
catalog. If one is added, its fixture must exercise parsing or its own
guardrail without performing the action and may use `GUARDED_EXECUTED` only
when that builtin rejects or confirms the request.

The current guarded, non-destructive observations are `redo` (no undo state),
`fc` (no history entry), `tdiff` (no checkpoint), and `kill -0 $$` (the builtin
rejects signalling the critical shell PID). They are reported as
`GUARDED_EXECUTED`, not skipped; none performs the destructive action.
