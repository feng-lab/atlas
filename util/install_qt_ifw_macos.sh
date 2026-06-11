#!/usr/bin/env bash
set -euo pipefail

if [ "$(uname -s)" != "Darwin" ]; then
  echo "Error: install_qt_ifw_macos.sh is only supported on macOS." >&2
  exit 1
fi

QT_ROOT="${QT_ROOT:-${HOME}/Qt}"
IFW_VERSION="${ATLAS_QT_IFW_VERSION:-4.11}"
IFW_PACKAGE_NAME="${ATLAS_QT_IFW_PACKAGE_NAME:-qt.tools.ifw.411}"
IFW_PACKAGE_VERSION="${ATLAS_QT_IFW_PACKAGE_VERSION:-4.11.0-0-202603311245}"
IFW_ARCHIVE="${ATLAS_QT_IFW_ARCHIVE:-ifw-mac-universal.7z}"
IFW_SHA256="${ATLAS_QT_IFW_SHA256:-140397cb4776a00a8efb76e5b8a362f8e6adc9a213da7a074e81b9b912f0c8f3}"
IFW_REPOSITORY_URL="${ATLAS_QT_IFW_REPOSITORY_URL:-https://download.qt.io/online/qtsdkrepository/mac_x64/ifw/tools_ifw_411}"
IFW_ROOT="${ATLAS_QT_IFW_ROOT:-${QT_ROOT}/Tools/QtInstallerFramework/${IFW_VERSION}}"

required_tools=(
  binarycreator
  archivegen
  installerbase
  maintenanceToolUpdater
)

ifw_has_required_tools() {
  local root="$1"
  local tool
  for tool in "${required_tools[@]}"; do
    if [ ! -x "${root}/bin/${tool}" ]; then
      return 1
    fi
  done
  return 0
}

if ifw_has_required_tools "${IFW_ROOT}"; then
  echo "Qt IFW ${IFW_VERSION} already available at ${IFW_ROOT}"
  exit 0
fi

if ! command -v 7z >/dev/null 2>&1; then
  echo "Error: 7z is required to extract Qt IFW. Install p7zip first." >&2
  exit 1
fi

tmp_parent="${RUNNER_TEMP:-${TMPDIR:-/tmp}}"
tmp_dir="$(mktemp -d "${tmp_parent%/}/atlas-qt-ifw.XXXXXX")"
cleanup() {
  rm -rf "${tmp_dir}"
}
trap cleanup EXIT

archive_path="${tmp_dir}/${IFW_ARCHIVE}"
download_url="${IFW_REPOSITORY_URL}/${IFW_PACKAGE_NAME}/${IFW_PACKAGE_VERSION}${IFW_ARCHIVE}"
stage_root="${tmp_dir}/QtInstallerFramework-${IFW_VERSION}"

echo "Downloading Qt IFW ${IFW_VERSION} from ${download_url}"
curl -fL --retry 3 --retry-delay 2 "${download_url}" -o "${archive_path}"

printf "%s  %s\n" "${IFW_SHA256}" "${archive_path}" | LC_ALL=C shasum -a 256 -c -

mkdir -p "${stage_root}"
7z x -y "${archive_path}" "-o${stage_root}"

if ! ifw_has_required_tools "${stage_root}"; then
  echo "Error: extracted Qt IFW archive is missing required tools under ${stage_root}/bin." >&2
  find "${stage_root}" -maxdepth 3 -type f | sort >&2
  exit 1
fi

mkdir -p "$(dirname "${IFW_ROOT}")"
rm -rf "${IFW_ROOT}"
mv "${stage_root}" "${IFW_ROOT}"

echo "Qt IFW ${IFW_VERSION} installed at ${IFW_ROOT}"
