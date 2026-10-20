# Sigil Audit

`sigil_audit.py` is an executable, black-box audit of `build/jash`. It does not
modify production code or run commands in the real repository.

Run it from any directory:

```sh
python3 /home/alf/OS/Jash/tests/sigil_audit.py
```

Use a different binary or retain the JSON report on disk with:

```sh
python3 tests/sigil_audit.py --jash build/jash --report /tmp/sigil-audit.json
```

## Contract Covered

The probes are derived from `c/module/sigil.c`, `modules/sigil/sigil_module.c`,
`c/main.c`, and `SPEC.md`:

- Levels `off`, `log`, `sign`, and `seal` execute real successful and failing commands.
- `log` produces a repository-scoped chain and ratchet; `verify --json` must pass.
- `sign` and `seal` execute real commands and produce the chain; explicit proof emission in the same isolated HOME creates an Ed25519 `session.jashproof`, and the proof plus DSSE envelope are verified.
- Runtime transitions prove that commands executed while `off` are not recorded and later levels are.
- Missing module, scoped ratchet, proof, and an invalid signing-key path exercise fail-closed guardrails.
- The normal missing Ed25519 seed is intentionally not treated as an error: the implementation creates it from `/dev/urandom` with mode `0600`. The invalid-path probe verifies the actual refusal path.
- Every probe runs in a new process with a private `HOME`, `TMPDIR`, and repository containing only a private `.git` directory.
- Created files, chain/proof structure, permissions, JSON verification fields, and raw-secret absence are recorded.

The chain and proof paths follow the implementation: inside the private repo
they are `~/.jash/chain-<repo>.log` and `~/.jash/ratchet-<repo>.key`; the text
receipt is `~/.jash/session.jashproof` and the optional DSSE receipt is
`~/.jash/session.jashproof.dsse`.

The current `build/jash` does not invoke the module's `sigil_shutdown` operation
on normal `-c` return, so automatic proof-at-exit is not assumed by this audit.
The explicit `sigil proof emit` probe is used because it is the reachable,
durable receipt contract and avoids reporting a proof as current after a later
command has appended to the chain.

## JSON Result

The report has one object per process probe. Each object records `probe`,
`level`, command exit status, captured `stdout` and `stderr`, newly created
files, receipt checks, sandbox persistence checks, and `PASS`/`FAIL`. The
program exits non-zero if the binary is unavailable or any probe fails.

No real credentials are inherited. A private sentinel is supplied only to
check that command output and persisted receipts do not expose raw secrets;
the report redacts it if a failure ever leaks it.
