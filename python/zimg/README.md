zimg (Python bindings)

This package builds and installs the `zimg` Python module backed by the repo’s
pybind11 extension under `src/python`.

Requirements
- A working C++ toolchain and dependencies used by the main Atlas build
- CMake >= 3.24, Ninja, pybind11 headers

Build
- `pip install .` from this directory, or `pip wheel .`

Note: this scikit-build wrapper reuses the repository’s CMake logic; it expects
third-party libraries to be prepared as they are for the conda recipe.

