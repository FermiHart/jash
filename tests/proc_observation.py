"""Import the repository-owned process observer used by tools and tests."""

import importlib.util
from pathlib import Path
import sys

_name = "_jash_proc_observation"
if _name not in sys.modules:
    _spec = importlib.util.spec_from_file_location(
        _name, Path(__file__).resolve().parents[1] / "tools/proc_observation.py")
    _module = importlib.util.module_from_spec(_spec)
    sys.modules[_name] = _module
    _spec.loader.exec_module(_module)
sys.modules[__name__] = sys.modules[_name]
