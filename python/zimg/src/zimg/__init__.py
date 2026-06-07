"""Python package for the zimg bindings.

This module exposes the compiled nanobind extension ``_imgpy`` built from the
repository’s C++ sources in ``src/python/imgpy.cpp``.
"""

import os
import shutil
import sys
import atexit
from pathlib import Path

# Enforce supported Python versions early and fail with an import error.
if sys.version_info < (3, 12):
    raise ImportError("zimg requires Python >= 3.12")

current_dir = Path(__file__).resolve().parent


def _java_executable_path() -> str:
    java_name = "java.exe" if os.name == "nt" else "java"
    java_home = os.environ.get("JAVA_HOME")
    if java_home:
        candidate = Path(java_home).expanduser() / "bin" / java_name
        if candidate.is_file():
            return str(candidate.resolve())
    return shutil.which(java_name) or ""


try:
    from . import _imgpy as _native  # type: ignore[import-not-found]
except ModuleNotFoundError as exc:
    # Only rewrite the error if the compiled extension itself is missing.
    # If a dependency import fails (e.g., `numpy`), surface that original error.
    if exc.name != f"{__name__}._imgpy":
        raise
    raise ImportError(
        "zimg extension '_imgpy' not found. Ensure you built and installed "
        "the package via scikit-build-core and that all native dependencies "
        "for Atlas zimg are available."
    ) from exc

_native._initialize_runtime(
    str(current_dir),
    str(current_dir / "jars"),
    _java_executable_path(),
)
atexit.register(_native._shutdown_runtime)

from ._imgpy import *  # type: ignore[import-not-found]  # noqa: F401,F403,E402
from . import bioformats  # noqa: F401,E402
from . import neutube_json
