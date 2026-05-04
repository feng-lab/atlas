"""Python package for the zimg bindings.

This module exposes the compiled nanobind extension ``_imgpy`` built from the
repository’s C++ sources in ``src/python/imgpy.cpp``.
"""

import os
import sys

# Enforce supported Python versions early and fail with an import error.
if sys.version_info < (3, 12):
    raise ImportError("zimg requires Python >= 3.12")

# Configure resource locations for the native library so both the native install
# layout and the PyPI wheel layout behave consistently.
current_dir = os.path.dirname(os.path.abspath(__file__))
os.environ.setdefault("Resources_DIR", current_dir)
os.environ.setdefault("ZIMG_JARS_DIR", os.path.join(current_dir, "jars"))

try:
    from ._imgpy import *  # type: ignore[import-not-found]  # noqa: F401,F403
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

from . import neutube_json
