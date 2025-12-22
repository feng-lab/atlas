import argparse
import datetime
import logging
import os
import shutil
import subprocess
import sys
import xml.etree.ElementTree as eTree
import zipfile
from typing import Optional

import build_ext_libs
import common_dirs
import linuxdeployqt
from logger import setup_logger

logger = logging.getLogger(__name__)

_DISABLE_SIGNING_ENV_VAR = "ATLAS_MACOS_DISABLE_SIGNING"
_CODESIGN_IDENTITY_ENV_VAR = "MACOS_CODESIGN_IDENTITY"
_ENTITLEMENTS_ENV_VAR = "ATLAS_MACOS_CODESIGN_ENTITLEMENTS"
_NOTARY_API_KEY_PATH_ENV_VAR = "MACOS_NOTARYTOOL_API_KEY_PATH"
_NOTARY_API_KEY_ID_ENV_VAR = "MACOS_NOTARYTOOL_API_KEY_ID"
_NOTARY_API_ISSUER_ID_ENV_VAR = "MACOS_NOTARYTOOL_API_ISSUER_ID"


def _env_truthy(name: str) -> bool:
    value = os.environ.get(name, "").strip().lower()
    return value in {"1", "true", "yes", "y", "on"}


def _macos_signing_disabled() -> bool:
    if not common_dirs.is_mac():
        return False
    return _env_truthy(_DISABLE_SIGNING_ENV_VAR)


def _maybe_load_dotenv() -> None:
    repo_root = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
    env_path = os.path.join(repo_root, ".env")
    env_local_path = os.path.join(repo_root, ".env.local")

    keys_from_env: set[str] = set()
    _load_env_file(env_path, keys_from_env=keys_from_env, override_only_keys=None)
    _load_env_file(env_local_path, keys_from_env=None, override_only_keys=keys_from_env)


def _load_env_file(
    path: str,
    *,
    keys_from_env: Optional[set[str]],
    override_only_keys: Optional[set[str]],
) -> None:
    if not os.path.exists(path):
        return

    try:
        with open(path, "r", encoding="utf-8") as f:
            for raw_line in f:
                line = raw_line.strip()
                if not line or line.startswith("#"):
                    continue
                if line.startswith("export "):
                    line = line[len("export ") :].lstrip()
                if "=" not in line:
                    continue
                key, value = line.split("=", 1)
                key = key.strip()
                value = value.strip()
                if not key:
                    continue
                if (
                    value.startswith(("'", '"'))
                    and len(value) >= 2
                    and value[-1] == value[0]
                ):
                    value = value[1:-1]

                if key in os.environ:
                    if override_only_keys is None or key not in override_only_keys:
                        continue

                os.environ[key] = value
                if keys_from_env is not None:
                    keys_from_env.add(key)
    except Exception as e:
        raise RuntimeError(f"Failed reading env file: {path}: {e}")


_maybe_load_dotenv()


def _macos_codesign_identity() -> str:
    identity = os.environ.get(_CODESIGN_IDENTITY_ENV_VAR)
    if not identity:
        raise RuntimeError(
            "macOS codesigning is enabled by default but no signing identity is configured. "
            f"Set {_CODESIGN_IDENTITY_ENV_VAR} (e.g. in `.env.local` in the repo root), or temporarily revert to ad-hoc signing by "
            f"setting {_DISABLE_SIGNING_ENV_VAR}=1."
        )
    return identity


def _macos_notarytool_auth_args() -> list[str]:
    api_key_path = os.environ.get(_NOTARY_API_KEY_PATH_ENV_VAR)
    api_key_id = os.environ.get(_NOTARY_API_KEY_ID_ENV_VAR)
    api_issuer_id = os.environ.get(_NOTARY_API_ISSUER_ID_ENV_VAR)

    if not (api_key_path and api_key_id and api_issuer_id):
        raise RuntimeError(
            "Notarization is enabled by default but notarytool API key auth is not configured. Set all of:\n"
            f"  - {_NOTARY_API_KEY_PATH_ENV_VAR}\n"
            f"  - {_NOTARY_API_KEY_ID_ENV_VAR}\n"
            f"  - {_NOTARY_API_ISSUER_ID_ENV_VAR}\n"
            f"Or disable signing via {_DISABLE_SIGNING_ENV_VAR}=1."
        )

    api_key_path = os.path.expanduser(api_key_path)
    if not os.path.isabs(api_key_path):
        api_key_path = os.path.join(common_dirs.atlas_repository_dir(), api_key_path)
    if not os.path.exists(api_key_path):
        raise RuntimeError(
            "Notarytool API key file not found.\n"
            f"Configured {_NOTARY_API_KEY_PATH_ENV_VAR}: {api_key_path}\n"
            f"Disable signing via {_DISABLE_SIGNING_ENV_VAR}=1 to use ad-hoc signing."
        )

    return ["--key", api_key_path, "--key-id", api_key_id, "--issuer", api_issuer_id]


def _macos_entitlements_path() -> Optional[str]:
    entitlements = os.environ.get(_ENTITLEMENTS_ENV_VAR)
    if not entitlements:
        return None
    if os.path.isabs(entitlements):
        return entitlements
    return os.path.join(common_dirs.atlas_repository_dir(), entitlements)


def _macos_run_checked(args: list[str], *, cwd: Optional[str] = None) -> None:
    logger.info("Running: %s", " ".join(args))
    subprocess.run(args, cwd=cwd, shell=False, check=True)


def _macos_codesign_bundle(bundle_path: str) -> None:
    if not common_dirs.is_mac():
        raise RuntimeError("_macos_codesign_bundle called on non-macOS")
    if not os.path.exists(bundle_path):
        raise RuntimeError(f"Bundle path does not exist: {bundle_path}")

    identity = _macos_codesign_identity()
    entitlements = _macos_entitlements_path()

    _macos_run_checked(["xattr", "-cr", bundle_path])

    cmd = [
        "codesign",
        "--force",
        "--options",
        "runtime",
        "--timestamp",
        "--sign",
        identity,
    ]
    if os.path.isdir(bundle_path):
        cmd.append("--deep")
    if entitlements:
        cmd.extend(["--entitlements", entitlements])
    cmd.append(bundle_path)
    _macos_run_checked(cmd)

    verify_cmd = ["codesign", "--verify", "--strict", "--verbose=2"]
    if os.path.isdir(bundle_path):
        verify_cmd.append("--deep")
    verify_cmd.append(bundle_path)
    _macos_run_checked(verify_cmd)


def _macos_notarize_and_staple_bundle(bundle_path: str) -> None:
    if not common_dirs.is_mac():
        raise RuntimeError("_macos_notarize_and_staple_bundle called on non-macOS")
    if not os.path.exists(bundle_path):
        raise RuntimeError(f"Bundle path does not exist: {bundle_path}")

    auth_args = _macos_notarytool_auth_args()

    bundle_dir = os.path.dirname(bundle_path)
    bundle_name = os.path.basename(bundle_path)
    notarize_zip = os.path.join(bundle_dir, f"{bundle_name}.notarize.zip")
    if os.path.exists(notarize_zip):
        os.remove(notarize_zip)

    _macos_run_checked(
        ["ditto", "-c", "-k", "--keepParent", bundle_name, notarize_zip], cwd=bundle_dir
    )
    _macos_run_checked(
        [
            "xcrun",
            "notarytool",
            "submit",
            notarize_zip,
            "--wait",
            *auth_args,
        ]
    )
    _macos_run_checked(["xcrun", "stapler", "staple", "-v", bundle_path])
    _macos_run_checked(["xcrun", "stapler", "validate", "-v", bundle_path])
    _macos_run_checked(
        ["spctl", "--assess", "--type", "execute", "--verbose=4", bundle_path]
    )
    try:
        os.remove(notarize_zip)
    except OSError:
        pass


def _read_git_version_from_header() -> str:
    """Read GIT_VERSION from generated src/version/version.h.

    Returns the raw C string content without surrounding quotes.
    Raises RuntimeError if the header is missing or malformed.
    """
    repo_root = common_dirs.atlas_repository_dir()
    vh_path = os.path.join(repo_root, "src", "version", "version.h")
    if not os.path.exists(vh_path):
        raise RuntimeError(f"version.h not found: {vh_path}")
    try:
        with open(vh_path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line.startswith("#define GIT_VERSION"):
                    # line format: #define GIT_VERSION "..."
                    start = line.find('"')
                    end = line.rfind('"')
                    if start != -1 and end != -1 and end > start:
                        return line[start + 1 : end]
        raise RuntimeError("GIT_VERSION define not found in version.h")
    except Exception as e:
        raise RuntimeError(f"Failed reading version.h: {e}")


def get_version_token_for_filename() -> str:
    """Return a filesystem-friendly version token for filenames.

    Uses the git-describe part of GIT_VERSION from version.h (before " build ").
    """
    raw = _read_git_version_from_header()

    # Strip the trailing build timestamp appended by CMake if present
    # e.g., "v0.9.0-1342-g5fecdae3 build 2025-11-15T04:34:33GMT"
    parts = raw.split(" build ", 1)
    token = parts[0].strip()

    # Replace spaces/slashes if any remain (defensive)
    token = token.replace(" ", "_").replace("/", "-")
    if not token:
        raise RuntimeError("Empty version token derived from GIT_VERSION")
    return token


def get_bak_file_name(orig_file: str):
    return orig_file + '.bak'


def update_pacakge_xml_version(template_file: str, file: str):
    tree = eTree.parse(template_file)
    tree.find('Version').text = '{0:%Y.%m.%d.%H}'.format(datetime.datetime.now())
    tree.find('ReleaseDate').text = '{0:%Y-%m-%d}'.format(datetime.datetime.now())
    # Write back to file
    tree.write(file, encoding="utf-8", xml_declaration=True)


def update_maintenance_pacakge_xml_version(template_file: str, file: str):
    tree = eTree.parse(template_file)
    tree.find('Version').text = '4.7.0'  # todo: get version and date from qt components.xml
    tree.find('ReleaseDate').text = '2024-02-15'
    # Write back to file
    tree.write(file, encoding="utf-8", xml_declaration=True)


def build_atlas_package(is_debug_version: bool = False):
    logger.info(f'current interpreter: {sys.executable}')

    binary_dir = common_dirs.atlas_binary_dir()
    logger.info(f'binaryDIR: {binary_dir}')
    logger.info(f'deployTargetDIR: {common_dirs.deploy_target_dir()}')
    logger.info(f'qtBaseDIR: {common_dirs.qt_base_dir()}')

    if common_dirs.is_mac():
        app_name = 'Atlas.app'

        shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), app_name), ignore_errors=True)

        if os.path.exists(os.path.join(binary_dir, app_name)):
            shutil.copytree(os.path.join(binary_dir, app_name),
                            os.path.join(common_dirs.deploy_target_dir(), app_name),
                            symlinks=True)
            subprocess.run([os.path.join(common_dirs.qt_bin_dir(), 'macdeployqt'), app_name],
                           cwd=common_dirs.deploy_target_dir(), shell=False, check=True)
        else:
            logger.critical('atlas is not built yet')

        binary_dir = common_dirs.atlas_binary_dir(arm64=True)
        logger.info(f'arm64 binaryDIR: {binary_dir}')
        arm64_app_name = 'Atlas_arm64.app'

        shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), arm64_app_name), ignore_errors=True)

        if os.path.exists(os.path.join(binary_dir, app_name)):
            shutil.copytree(os.path.join(binary_dir, app_name),
                            os.path.join(common_dirs.deploy_target_dir(), arm64_app_name),
                            symlinks=True)
            subprocess.run([os.path.join(common_dirs.qt_bin_dir(), 'macdeployqt'), arm64_app_name],
                           cwd=common_dirs.deploy_target_dir(), shell=False, check=True)
        else:
            logger.critical('arm64 atlas is not built yet')

        filename = os.path.join(common_dirs.deploy_target_dir(), arm64_app_name, 'Contents', 'MacOS', 'Atlas')
        target_filename = os.path.join(common_dirs.deploy_target_dir(), app_name, 'Contents', 'MacOS', 'Atlas')
        logger.info(f'merge {filename} to {target_filename}')
        subprocess.run(['lipo', '-create', filename, target_filename, '-output', target_filename],
                       shell=False, check=True)

        # Ensure LLM docs are generated in repo and copied into the .app
        repo_root = common_dirs.atlas_repository_dir()
        repo_llm_dir = os.path.join(repo_root, 'src', 'atlas', 'Resources', 'json', 'atlas')
        os.makedirs(repo_llm_dir, exist_ok=True)
        # Generate missing docs using the agent CLI; use deploy .app as --atlas-dir
        subprocess.run(
            [
                sys.executable,
                "-m",
                "python.atlas_agent.src.atlas_agent",
                "--prepare-llm-docs-only",
                "--atlas-dir",
                os.path.join(common_dirs.deploy_target_dir(), app_name),
            ],
            cwd=repo_root,
            shell=False,
            check=True,
        )
        # Copy repo docs into the deployed app
        target_llm_dir = os.path.join(common_dirs.deploy_target_dir(), app_name, 'Contents', 'Resources', 'json', 'atlas')
        os.makedirs(target_llm_dir, exist_ok=True)
        shutil.copytree(repo_llm_dir, target_llm_dir, dirs_exist_ok=True)

        deployed_app_path = os.path.join(common_dirs.deploy_target_dir(), app_name)
        if _macos_signing_disabled():
            subprocess.run(
                ["codesign", "--force", "--deep", "--sign", "-", deployed_app_path],
                shell=False,
                check=True,
            )
        else:
            _macos_codesign_bundle(deployed_app_path)
            if not is_debug_version:
                _macos_notarize_and_staple_bundle(deployed_app_path)
    elif common_dirs.is_linux():
        app_name = "Atlas"

        shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), 'Atlas.AppDir'), ignore_errors=True)

        if os.path.exists(os.path.join(binary_dir, app_name)):
            linuxdeployqt.linuxdeployqt(os.path.join(binary_dir, app_name),
                                        os.path.join(common_dirs.deploy_target_dir(), 'Atlas.AppDir'),
                                        common_dirs.qt_base_dir(),
                                        is_debug_version=is_debug_version)
            # Ensure LLM docs are generated in repo and copied into AppDir
            repo_root = common_dirs.atlas_repository_dir()
            repo_llm_dir = os.path.join(repo_root, 'src', 'atlas', 'Resources', 'json', 'atlas')
            os.makedirs(repo_llm_dir, exist_ok=True)
            subprocess.run(
                [
                    sys.executable,
                    "-m",
                    "python.atlas_agent.src.atlas_agent",
                    "--prepare-llm-docs-only",
                    "--atlas-dir",
                    os.path.join(common_dirs.deploy_target_dir(), "Atlas.AppDir"),
                ],
                cwd=repo_root,
                shell=False,
                check=True,
            )
            target_llm_dir = os.path.join(common_dirs.deploy_target_dir(), 'Atlas.AppDir', 'Resources', 'json', 'atlas')
            os.makedirs(target_llm_dir, exist_ok=True)
            shutil.copytree(repo_llm_dir, target_llm_dir, dirs_exist_ok=True)
        else:
            logger.critical('atlas is not built yet')
    else:
        app_name = 'Atlas.exe'

        shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), 'Atlas'), ignore_errors=True)

        if os.path.exists(os.path.join(binary_dir, app_name)):
            os.mkdir(os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            shutil.copy2(os.path.join(binary_dir, app_name),
                         os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            shutil.copytree(os.path.join(binary_dir, 'Resources'),
                            os.path.join(common_dirs.deploy_target_dir(), 'Atlas', 'Resources'),
                            symlinks=True)
            shutil.copytree(os.path.join(binary_dir, 'vulkan'),
                            os.path.join(common_dirs.deploy_target_dir(), 'Atlas', 'vulkan'),
                            symlinks=True)
            # build_ext_libs.glob_copy(os.path.join(common_dirs.assimp_redist_dir(), 'assimp*.dll'),
            #                          os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            build_ext_libs.glob_copy(os.path.join(common_dirs.freeimage_redist_dir(), '*.dll'),
                                     os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            # build_ext_libs.glob_copy(os.path.join(common_dirs.ext_build_dir(), 'bin', 'vtktoken-*.dll'),
            #                          os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            shutil.copy2(os.path.join(common_dirs.tbb_redist_dir(), 'tbb12.dll'),
                         os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            build_ext_libs.glob_copy(
                os.path.join(common_dirs.vc_CRT_redist_dir(), '*.dll'),
                os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            build_ext_libs.glob_copy(
                os.path.join(common_dirs.vc_CXXAMP_redist_dir(), '*.dll'),
                os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            build_ext_libs.glob_copy(
                os.path.join(common_dirs.vc_OpenMP_redist_dir(), '*.dll'),
                os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))

            env = build_ext_libs.get_vcvars_environment()
            subprocess.run([os.path.join(common_dirs.qt_bin_dir(), 'windeployqt'), '--no-translations', app_name],
                           cwd=os.path.join(common_dirs.deploy_target_dir(), 'Atlas'), shell=False, check=True, env=env)
            # Ensure LLM docs are generated in repo and copied into deploy folder
            repo_root = common_dirs.atlas_repository_dir()
            repo_llm_dir = os.path.join(repo_root, 'src', 'atlas', 'Resources', 'json', 'atlas')
            os.makedirs(repo_llm_dir, exist_ok=True)
            subprocess.run(
                [
                    sys.executable,
                    "-m",
                    "python.atlas_agent.src.atlas_agent",
                    "--prepare-llm-docs-only",
                    "--atlas-dir",
                    os.path.join(common_dirs.deploy_target_dir(), "Atlas"),
                ],
                cwd=repo_root,
                shell=False,
                check=True,
            )
            target_llm_dir = os.path.join(common_dirs.deploy_target_dir(), 'Atlas', 'Resources', 'json', 'atlas')
            os.makedirs(target_llm_dir, exist_ok=True)
            shutil.copytree(repo_llm_dir, target_llm_dir, dirs_exist_ok=True)
        else:
            logger.critical('atlas is not built yet')


def pack_atlas_package():
    version_token = get_version_token_for_filename()

    if common_dirs.is_mac():
        app_name = 'Atlas.app'
        suffix = "macOS"
    elif common_dirs.is_linux():
        app_name = "Atlas.AppDir"
        suffix = "Linux"
    else:
        app_name = 'Atlas'
        suffix = "Windows"

    zip_name = f"atlas-{suffix}-{version_token}.zip"

    # Remove any previous atlas-{suffix}-*.zip to keep deploy dir tidy
    deploy_dir = common_dirs.deploy_target_dir()
    try:
        for fname in os.listdir(deploy_dir):
            if fname.startswith(f"atlas-{suffix}-") and fname.endswith(".zip"):
                try:
                    os.remove(os.path.join(deploy_dir, fname))
                except Exception:
                    pass
    except FileNotFoundError:
        os.makedirs(deploy_dir, exist_ok=True)

    if common_dirs.is_windows():
        shutil.make_archive(
            os.path.join(deploy_dir, zip_name[:-4]),
            "zip",
            root_dir=deploy_dir,
            base_dir=app_name,
        )
    else:
        subprocess.run(
            ["zip", "--quiet", "--recurse-paths", "--symlinks", zip_name, app_name],
            cwd=deploy_dir,
            shell=False,
            check=True,
        )


def build_atlas_installer():

    if common_dirs.is_mac():
        suffix = 'macOS'
        app_name = 'Atlas.app'
        repo_package_name = 'atlas.7z'
        mt_app_name = '.tempMaintenanceTool'
        mt_repo_package_name = 'MaintenanceTool.7z'
        installer_base_name = 'AtlasInstaller'
        installer_app_name = 'AtlasInstaller.app'
        installer_zip_name = f"AtlasInstaller-{suffix}.zip"
    elif common_dirs.is_linux():
        suffix = 'Linux'
        app_name = 'Atlas.AppDir'
        repo_package_name = 'atlas.7z'
        mt_app_name = '.tempMaintenanceTool'
        mt_repo_package_name = 'MaintenanceTool.7z'
        installer_base_name = 'AtlasInstaller'
        installer_app_name = 'AtlasInstaller'
        installer_zip_name = f"AtlasInstaller-{suffix}.zip"
    else:
        suffix = 'Windows'
        app_name = 'Atlas'
        repo_package_name = 'atlas.7z'
        mt_app_name = 'tempMaintenanceTool.exe'
        mt_repo_package_name = 'MaintenanceTool.7z'
        installer_base_name = 'AtlasInstaller'
        installer_app_name = 'AtlasInstaller.exe'
        installer_zip_name = f"AtlasInstaller-{suffix}.zip"

    if os.path.exists(os.path.join(common_dirs.deploy_target_dir(), repo_package_name)):
        os.remove(os.path.join(common_dirs.deploy_target_dir(), repo_package_name))
    shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), suffix), ignore_errors=True)
    if os.path.exists(
        os.path.join(common_dirs.deploy_target_dir(), installer_zip_name)
    ):
        os.remove(os.path.join(common_dirs.deploy_target_dir(), installer_zip_name))

    if common_dirs.is_mac():
        shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), installer_app_name), ignore_errors=True)
    elif os.path.exists(os.path.join(common_dirs.deploy_target_dir(), installer_app_name)):
        os.remove(os.path.join(common_dirs.deploy_target_dir(), installer_app_name))

    if os.path.exists(os.path.join(common_dirs.deploy_target_dir(), 'packages', 'fenglab.neutube')):
        shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), 'packages', 'fenglab.neutube'),
                      ignore_errors=False, onexc=common_dirs.handleRemoveReadonly)
    shutil.copytree(os.path.join(common_dirs.ext_build_dir(), 'packages-' + suffix, 'fenglab.neutube'),
                    os.path.join(common_dirs.deploy_target_dir(), 'packages', 'fenglab.neutube'))

    mt_app_path = os.path.join(common_dirs.deploy_target_dir(), mt_app_name)
    if os.path.exists(mt_app_path):
        os.remove(mt_app_path)
    if common_dirs.is_windows():
        shutil.copy(os.path.join(common_dirs.qt_installer_framework_bin_dir(), 'installerbase.exe'),
                    os.path.join(common_dirs.deploy_target_dir(), mt_app_name))
    else:
        shutil.copy(
            os.path.join(common_dirs.qt_installer_framework_bin_dir(), "installerbase"),
            mt_app_path,
        )
        if common_dirs.is_mac():
            if _macos_signing_disabled():
                subprocess.run(
                    ["codesign", "--force", "--sign", "-", mt_app_path],
                    shell=False,
                    check=True,
                )
            else:
                _macos_codesign_bundle(mt_app_path)
    subprocess.run(
        [
            os.path.join(common_dirs.qt_installer_framework_bin_dir(), "archivegen"),
            mt_repo_package_name,
            mt_app_path,
        ],
        cwd=common_dirs.deploy_target_dir(),
        shell=False,
        check=True,
    )
    os.remove(mt_app_path)
    repo_package_folder = os.path.join(common_dirs.deploy_target_dir(),
                                       'packages', 'fenglab.maintenance', 'data')
    if os.path.exists(os.path.join(repo_package_folder, mt_repo_package_name)):
        os.remove(os.path.join(repo_package_folder, mt_repo_package_name))
    shutil.move(os.path.join(common_dirs.deploy_target_dir(), mt_repo_package_name), repo_package_folder)
    update_maintenance_pacakge_xml_version(os.path.join(common_dirs.deploy_target_dir(), 'maintenance_package.xml'),
                                           os.path.join(common_dirs.deploy_target_dir(),
                                                        'packages', 'fenglab.maintenance', 'meta', 'package.xml'))

    subprocess.run([os.path.join(common_dirs.qt_installer_framework_bin_dir(), 'archivegen'),
                    '--compression', '5',
                    repo_package_name, app_name],
                   cwd=common_dirs.deploy_target_dir(), shell=False, check=True)

    repo_package_folder = os.path.join(common_dirs.deploy_target_dir(),
                                       'packages', 'fenglab.atlas', 'data')
    if os.path.exists(os.path.join(repo_package_folder, repo_package_name)):
        os.remove(os.path.join(repo_package_folder, repo_package_name))
    shutil.move(os.path.join(common_dirs.deploy_target_dir(), repo_package_name), repo_package_folder)
    update_pacakge_xml_version(os.path.join(common_dirs.deploy_target_dir(), 'atlas_package.xml'),
                               os.path.join(common_dirs.deploy_target_dir(),
                                            'packages', 'fenglab.atlas', 'meta', 'package.xml'))

    subprocess.run([os.path.join(common_dirs.qt_installer_framework_bin_dir(), 'repogen'),
                    '-p', 'packages', './' + suffix],
                   cwd=common_dirs.deploy_target_dir(), shell=False, check=True)

    subprocess.run([os.path.join(common_dirs.qt_installer_framework_bin_dir(), 'binarycreator'),
                    '--online-only', '-c', 'config/config-' + suffix + '.xml', '-p', 'packages',
                    installer_base_name],
                   cwd=common_dirs.deploy_target_dir(), shell=False, check=True)

    if common_dirs.is_mac():
        installer_app_path = os.path.join(common_dirs.deploy_target_dir(), installer_app_name)
        if _macos_signing_disabled():
            subprocess.run(
                ["codesign", "--force", "--deep", "--sign", "-", installer_app_path],
                shell=False,
                check=True,
            )
        else:
            _macos_codesign_bundle(installer_app_path)
            _macos_notarize_and_staple_bundle(installer_app_path)

    if common_dirs.is_windows():
        zipfile.ZipFile(os.path.join(common_dirs.deploy_target_dir(), installer_zip_name), mode='w') \
            .write(os.path.join(common_dirs.deploy_target_dir(), installer_app_name), arcname=installer_app_name)
    else:
        subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', installer_zip_name, installer_app_name],
                       cwd=common_dirs.deploy_target_dir(), shell=False, check=True)

    if common_dirs.is_my_computer():
        if common_dirs.is_mac():
            out_folder = common_dirs.static_deploy_folder()
            shutil.copy2(os.path.join(common_dirs.deploy_target_dir(), installer_zip_name),
                         os.path.join(out_folder, 'installers', installer_zip_name))
            target_folder = os.path.join(out_folder, 'packages')
            if os.path.exists(os.path.join(target_folder, suffix)):
                shutil.rmtree(os.path.join(target_folder, suffix), ignore_errors=False)
            shutil.move(os.path.join(common_dirs.deploy_target_dir(), suffix), target_folder)
        elif common_dirs.is_windows():
            out_folder = common_dirs.static_deploy_folder()
            shutil.copy2(os.path.join(common_dirs.deploy_target_dir(), installer_zip_name),
                         os.path.join(out_folder, 'installers', installer_zip_name))
            target_folder = os.path.join(out_folder, 'packages')
            if os.path.exists(os.path.join(target_folder, suffix)):
                shutil.rmtree(os.path.join(target_folder, suffix), ignore_errors=False,
                              onexc=common_dirs.handleRemoveReadonly)
            shutil.move(os.path.join(common_dirs.deploy_target_dir(), suffix), target_folder)


def deploy_atlas(is_debug_version: bool = False):
    build_atlas_package(is_debug_version=is_debug_version)
    if not is_debug_version:
        pack_atlas_package()
        build_atlas_installer()


if __name__ == "__main__":
    logger = setup_logger()

    parser = argparse.ArgumentParser(
        epilog="""
Examples:

python deploy_atlas.py [--debug-version]
""",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--debug-version", action='store_true', help="debug version")
    args = parser.parse_args()

    deploy_atlas(is_debug_version=args.debug_version)
