# Locates the built fleece_goap_native extension module and puts it on
# sys.path before any test imports it. The module isn't built by default (see
# the FLEECE_BUILD_PYTHON_BINDINGS CMake option) - point FLEECE_GOAP_NATIVE_DIR
# at the directory containing the .so if it isn't in one of the default build
# dirs this searches.
import os
import sys
from pathlib import Path

FLEECE_ROOT = Path(__file__).resolve().parents[2]

_search_dirs = []
if os.environ.get("FLEECE_GOAP_NATIVE_DIR"):
    _search_dirs.append(Path(os.environ["FLEECE_GOAP_NATIVE_DIR"]))
_search_dirs += [
    FLEECE_ROOT / "build",
    FLEECE_ROOT / "build-python",
    Path("/tmp/fleece_build_python"),
]

for d in _search_dirs:
    if d.is_dir() and any(d.glob("fleece_goap_native*.so")):
        sys.path.insert(0, str(d))
        break
