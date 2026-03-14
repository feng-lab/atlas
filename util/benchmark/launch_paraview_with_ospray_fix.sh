#!/bin/zsh

set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <paraview|pvpython|/path/to/executable> [args...]" >&2
  exit 1
fi

app_root="/Applications/ParaView-6.1.0-RC1.app"
libraries_dir="$app_root/Contents/Libraries"
shim_dir="${TMPDIR:-/tmp}/paraview_openvkl_fix"

mkdir -p "$shim_dir"
ln -sf "$libraries_dir/libopenvkl_module_cpu_device.1.0.1.dylib" \
  "$shim_dir/libopenvkl_module_cpu_device.dylib"

target="$1"
shift

case "$target" in
  paraview)
    executable="$app_root/Contents/MacOS/paraview"
    ;;
  pvpython)
    executable="$app_root/Contents/bin/pvpython"
    ;;
  *)
    executable="$target"
    ;;
esac

if [[ ! -x "$executable" ]]; then
  echo "executable not found or not executable: $executable" >&2
  exit 1
fi

if [[ -n "${DYLD_LIBRARY_PATH:-}" ]]; then
  export DYLD_LIBRARY_PATH="$shim_dir:$DYLD_LIBRARY_PATH"
else
  export DYLD_LIBRARY_PATH="$shim_dir"
fi

exec "$executable" "$@"
