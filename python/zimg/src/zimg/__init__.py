"""Python package for the zimg bindings.

This module exposes the compiled extension `_imgpy` built by the repository’s
nanobind project (src/python/imgpy.cpp).
"""

try:
    from ._imgpy import *  # noqa: F401,F403
except Exception as e:
    raise ImportError(
        "zimg extension not found. Ensure you built/install via scikit-build-core, "
        "or that your environment includes the compiled extension (_imgpy)."
    ) from e

