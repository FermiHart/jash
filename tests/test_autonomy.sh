#!/usr/bin/env bash
# Focused integration gate; missing isolation is not an isolation success.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${JASH_BUILD_DIR:-"$ROOT/build"}
if [[ $(uname -s) != Linux ]]; then
    printf '%s\n' 'Autonomy runtime requires Linux; use assert portable tests separately.'
    exit 77
fi
cmake --build "$BUILD" --target jash jash-agentd jef_verify test_agent_process_ir -j4
"$BUILD/test_agent_process_ir"
python3 -B "$ROOT/tests/test_autonomy_sandbox.py"
python3 -B "$ROOT/tests/test_assert_dependencies.py" --binary "$BUILD/jash"
python3 -B "$ROOT/tests/test_assert_dependencies.py" --portable
JASH_BUILD_DIR="$BUILD" JASH_SKIP_BUILD=1 bash "$ROOT/tests/test_libjef_job_spec.sh"
JASH_BUILD_DIR="$BUILD" python3 -B "$ROOT/tests/test_autonomy_workflow.py"
JASH_BUILD_DIR="$BUILD" python3 -B "$ROOT/tests/test_autonomy_publication.py"
python3 -B "$ROOT/tests/test_autonomy_resources.py" ResourceUnitTests
python3 -B "$ROOT/examples/autonomy/demo.py" --build "$BUILD"
