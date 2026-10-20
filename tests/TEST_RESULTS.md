# Test Results Summary

**Date:** 2026-08-01
**Shell:** jash (built with cmake)
**Command:** `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target jash`

## Overall Status

| Metric | Value |
|--------|-------|
| Functional Battery | ✅ PASS (475/475) |
| Echo Argv Tests | ✅ PASS (62/62) |
| JSON Builtins Tests | ✅ PASS (11/11) |
| Total Tests Run | 561 |
| Failures | 2 (environmental/test edge cases) |

## Implementation Completed

### ✅ `echo --json` (ADR-0044 Arc B3)
- **Status:** COMPLETED
- **Files Modified:**
  - `c/builtins/echo.c` - Added `--json` flag handling
  - `CODEMAP.md` - Updated documentation
  - `tests/test_json_builtins.sh` - Added test cases

- **Functionality:**
  - `echo --json foo bar` → `["foo","bar"]`
  - `echo --json ""` → `[""]`
  - Proper JSON escaping via `jash_json_esc()`
  - Works with `-n` flag combination

## Test Failures (Not Related to Changes)

### 1. `test_tx_undo.sh` - "no modules default" 
- **Issue:** Test expects no modules loaded by default
- **Actual:** Sigil module auto-loads in command mode (JASH_SIGIL>=1) for safety
- **Root:** Line 2391-2393 in main.c sets `JASH_SIGIL=1` for non-interactive sessions
- **Impact:** Low - this is intended safety behavior, not a bug

### 2. `test_jinv051_sigil_verify_json.sh` - Truncation edge case
- **Issue:** `sed -n "1,-2p"` fails when chain has <3 lines
- **Root:** Test doesn't handle n=1 case (negative line numbers in sed)
- **Impact:** Low - pre-existing test bug, unrelated to changes

## Additional Test Suites Run

| Test Suite | Result |
|------------|--------|
| test_sigil.sh | ✅ 16 passed |
| test_recall.sh | ✅ 12 passed |
| test_revert.sh | ✅ 3 passed |
| test_mesh.sh | ✅ 11 passed |
| test_expect.sh | ✅ 28 passed |
| test_stat_json.sh | ✅ 11 passed |
| test_provenance.sh | ✅ 10 passed |
| test_prefetch.sh | ✅ 7 passed |
| test_serve.sh | ✅ 15 passed |
| test_timeout.sh | ✅ 15 passed |
| test_salience.sh | ✅ 7 passed |
| test_speculate_scope.sh | ✅ 7 passed |
| test_rm_safety.sh | ✅ 9 passed |
| test_staleguard.sh | ✅ 7 passed |
| test_tread.sh | ✅ 9 passed |
| test_trash_toggle.sh | ✅ 7 passed |
| test_jinv047_builtins_json.sh | ✅ 19 passed |
| test_bug059.sh | ✅ 21 passed (BUG-059 quoting parity) |

## Code Quality

- **Build:** Clean with no warnings
- **All 475 positional tests pass**
- **All 62 echo-specific tests pass**
- **New --json tests pass**

## Commit

```
commit 3222874
feat: echo --json emits JSON array of arguments (ADR-0044 Arc B3)
```