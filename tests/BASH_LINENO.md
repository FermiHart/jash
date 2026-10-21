# BASH_LINENO compatibility contract

`BASH_LINENO` is a diagnostic view of active caller frames, not the current
execution line (`LINENO`), following the caller-frame meaning in the
[GNU Bash variable reference](https://www.gnu.org/software/bash/manual/html_node/Bash-Variables.html).
Index zero is the physical line that invoked the
current function or sourced file; subsequent elements describe its callers.
A script file contributes the outer `0` frame. Top-level `-c` and stdin have
no caller frames, so their scalar expansion is empty.

The reader carries byte-to-source-line metadata through heredoc rewriting,
line continuations, compound statements and copied function bodies. Definitions
retain their origin after the defining source file returns. Function and source
frames are restored on return; subshells and command substitutions inherit the
active stack. No frame is invented from the number of evaluator invocations.

`test_bash_lineno.py` compares identical scripts against an explicitly selected
modern Bash (5.x or later) in
`-c`, stdin and file modes. It checks scalar, indexed, all-elements, count,
length, default and setness expansions; quoted array cardinality; physical
call sites; nested functions and sources; retained sourced definitions;
pipelines, subshells and command substitution; return/error unwinding;
heredocs, continuations and a function RETURN trap. The native CI workflow uses the system modern Bash on Linux and Homebrew Bash
on macOS, recording the selected executable and version. macOS additionally
runs stdin/file comparisons against its legacy system Bash.

## Deliberate limits

- Bash 3.2 uses a zero-based caller-line convention for `-c`, unlike modern
  Bash and Jash. This is a general `-c` difference, not shebang removal. The
  harness rejects that oracle for `-c`; legacy stdin/file checks remain active.

- The diagnostic view is authoritative: assigning or unsetting a shell variable
  named `BASH_LINENO` does not replace the native stack. This is not Bash's
  mutable-variable behavior.
- Arbitrary array transformations and negative arithmetic subscript expressions
  are not claimed as complete Bash compatibility.
- Within command substitutions (including backticks and case patterns), new
  function calls use their actual physical source line. Bash sometimes reports
  a parse-time line based on the closing delimiter or enclosing case header
  instead; these parser-specific offsets are intentionally not reproduced.
  Inherited caller frames still retain their original values.
- Generated `eval` source and arbitrary trap provenance are not fully covered
  by this contract. The existing absence of source RETURN-trap dispatch is not
  changed here; the tested RETURN trap is a function trap.
- `BASH_SOURCE` and `FUNCNAME` compatibility are separate from this change.
- Caller depth is bounded by `JASH_SOURCE_FRAME_MAX`; overflow is an explicit
  failure rather than a truncated or fabricated caller stack.

Run after building the shell and its native modules:

    python3 tests/test_bash_lineno.py --jash /path/to/jash

The test does not disable required module attestation and derives no expected
caller values from Jash's implementation.
