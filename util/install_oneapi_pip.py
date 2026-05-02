import subprocess
import sys
from pathlib import Path

import common_dirs
from logger import setup_logger

logger = setup_logger(name=__name__)


PACKAGES = ("mkl-static",)


def target_dir() -> Path:
    return Path(common_dirs.oneapi_pip_dir())


def layout_dir() -> Path:
    return Path(common_dirs.oneapi_pip_intel_dir())


def required_files() -> tuple[str, ...]:
    if common_dirs.is_windows():
        return (
            "include/mkl.h",
            "include/fftw/fftw3.h",
            "lib/mkl_intel_lp64.lib",
            "lib/mkl_tbb_thread.lib",
            "lib/mkl_core.lib",
            "lib/cmake/tbb/TBBConfig.cmake",
            "bin/tbb12.dll",
            "bin/tbbmalloc.dll",
            "bin/tbbmalloc_proxy.dll",
        )
    if common_dirs.is_linux():
        return (
            "include/mkl.h",
            "include/fftw/fftw3.h",
            "lib/libmkl_intel_lp64.a",
            "lib/libmkl_tbb_thread.a",
            "lib/libmkl_core.a",
            "lib/cmake/tbb/TBBConfig.cmake",
            "lib/libtbb.so",
            "lib/libtbb.so.12",
            "lib/libtbbmalloc.so",
            "lib/libtbbmalloc_proxy.so",
        )
    raise AssertionError("oneAPI pip packages are only used on Windows and Linux.")


def missing_files(root: Path) -> list[str]:
    return [path for path in required_files() if not (root / path).exists()]


def install_oneapi_pip() -> None:
    if not common_dirs.is_windows() and not common_dirs.is_linux():
        raise AssertionError("oneAPI pip packages are only used on Windows and Linux.")

    try:
        existing_dir = common_dirs.intel_sw_dir()
    except AssertionError:
        existing_dir = None
    if existing_dir is not None:
        logger.info("Intel oneAPI already available at %s", existing_dir)
        return

    target = target_dir()
    layout = layout_dir()
    missing = missing_files(layout)
    if not missing:
        logger.info("oneAPI pip packages already installed at %s", layout)
        return

    target.mkdir(parents=True, exist_ok=True)
    cmd = [
        sys.executable,
        "-m",
        "pip",
        "install",
        "--upgrade",
        "--no-cache-dir",
        "--only-binary=:all:",
        "--prefix" if common_dirs.is_linux() else "--target",
        str(target),
        *PACKAGES,
    ]
    logger.info("Installing oneAPI pip packages into %s", target)
    subprocess.run(cmd, shell=False, check=True)

    missing = missing_files(layout)
    if missing:
        raise AssertionError(
            "oneAPI pip install is incomplete. Missing under "
            + str(layout)
            + ": "
            + ", ".join(missing)
        )


if __name__ == "__main__":
    install_oneapi_pip()
