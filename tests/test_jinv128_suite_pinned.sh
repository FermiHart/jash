#!/usr/bin/env bash
# JINV-128: exercise the actual runner with trusted miniature registries.
# No replica of the pinning implementation, no master execution or build.
set -euo pipefail
exec python3 -B "$(dirname -- "${BASH_SOURCE[0]}")/test_suite_runner.py" \
    SuiteRunnerTests.test_registry_mutation_is_pinned \
    SuiteRunnerTests.test_predictable_old_symlink_unfollowed \
    SuiteRunnerTests.test_master_list_preserves_reviewed_registry_and_multiline_gate
