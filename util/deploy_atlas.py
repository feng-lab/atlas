import argparse
import datetime
import hashlib
import logging
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as eTree
import zipfile
from typing import AbstractSet, Optional

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

# Only import the deployment-related variables from dotenv files to avoid surprising
# side effects (e.g. altering subprocess behavior via unrelated env vars).
_DOTENV_KEYS: frozenset[str] = frozenset(
    {
        _DISABLE_SIGNING_ENV_VAR,
        _CODESIGN_IDENTITY_ENV_VAR,
        _ENTITLEMENTS_ENV_VAR,
        _NOTARY_API_KEY_PATH_ENV_VAR,
        _NOTARY_API_KEY_ID_ENV_VAR,
        _NOTARY_API_ISSUER_ID_ENV_VAR,
    }
)

_MACOS_NESTED_BUNDLE_SUFFIXES: tuple[str, ...] = (
    ".app",
    ".appex",
    ".bundle",
    ".framework",
    ".kext",
    ".plugin",
    ".qlgenerator",
    ".xpc",
)

_MACOS_MACHO_FILE_EXTENSIONS: frozenset[str] = frozenset({".dylib", ".jnilib", ".so"})
_MACOS_DISALLOWED_MACHO_ARCHS: frozenset[str] = frozenset({"i386", "ppc", "ppc64"})
_MACOS_JAR_ENTRY_REMOVE_LIST: frozenset[str] = frozenset(
    {
        # Notarytool rejects this vendored TurboJPEG build because it was built with an SDK
        # older than macOS 10.9. We don't ship a newer build, so remove it from the jar to
        # satisfy notarization.
        "META-INF/lib/osx_64/libturbojpeg.dylib",
    }
)


def _env_truthy(name: str) -> bool:
    value = os.environ.get(name, "").strip().lower()
    return value in {"1", "true", "yes", "y", "on"}


def _macos_signing_disabled() -> bool:
    if not common_dirs.is_mac():
        return False
    return _env_truthy(_DISABLE_SIGNING_ENV_VAR)


def _maybe_load_dotenv() -> None:
    if not common_dirs.is_mac():
        return

    repo_root = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
    env_path = os.path.join(repo_root, ".env")
    env_local_path = os.path.join(repo_root, ".env.local")

    keys_from_env: set[str] = set()
    _load_env_file(
        env_path,
        keys_from_env=keys_from_env,
        override_only_keys=None,
        allowed_keys=_DOTENV_KEYS,
    )
    _load_env_file(
        env_local_path,
        keys_from_env=None,
        override_only_keys=keys_from_env,
        allowed_keys=_DOTENV_KEYS,
    )


def _load_env_file(
    path: str,
    *,
    keys_from_env: Optional[set[str]],
    override_only_keys: Optional[set[str]],
    allowed_keys: Optional[AbstractSet[str]],
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
                if allowed_keys is not None and key not in allowed_keys:
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
    entitlements = os.path.expanduser(os.path.expandvars(entitlements))
    if not os.path.isabs(entitlements):
        entitlements = os.path.join(common_dirs.atlas_repository_dir(), entitlements)
    if not os.path.exists(entitlements):
        raise RuntimeError(
            f"{_ENTITLEMENTS_ENV_VAR} is set but the entitlements file does not exist: {entitlements}"
        )
    return entitlements


def _macos_run_checked(args: list[str], *, cwd: Optional[str] = None) -> None:
    logger.info("Running: %s", " ".join(args))
    subprocess.run(args, cwd=cwd, shell=False, check=True)

def _macos_is_signable_bundle_dir(path: str) -> bool:
    if not os.path.isdir(path):
        return False
    lower = path.lower()
    return any(lower.endswith(suffix) for suffix in _MACOS_NESTED_BUNDLE_SUFFIXES)


def _macos_file_description(path: str) -> Optional[str]:
    try:
        proc = subprocess.run(
            ["file", "-b", path],
            shell=False,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
    except Exception as e:
        logger.debug("file(1) failed for %s: %s", path, e)
        return None

    if proc.returncode != 0:
        logger.debug(
            "file(1) returned %s for %s: %s", proc.returncode, path, proc.stdout
        )
        return None

    return (proc.stdout or "").strip()


def _macos_lipo_archs(path: str) -> Optional[list[str]]:
    try:
        proc = subprocess.run(
            ["lipo", "-archs", path],
            shell=False,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
    except FileNotFoundError:
        return None
    except Exception as e:
        logger.debug("lipo(1) failed for %s: %s", path, e)
        return None

    if proc.returncode != 0:
        logger.debug(
            "lipo(1) returned %s for %s: %s", proc.returncode, path, proc.stdout
        )
        return None

    archs = (proc.stdout or "").strip()
    if not archs:
        return None
    return archs.split()


def _macos_strip_disallowed_macho_archs_in_place(path: str) -> None:
    archs = _macos_lipo_archs(path)
    if not archs:
        return

    to_remove = [arch for arch in archs if arch in _MACOS_DISALLOWED_MACHO_ARCHS]
    if not to_remove:
        return

    keep = [arch for arch in archs if arch not in _MACOS_DISALLOWED_MACHO_ARCHS]
    if not keep:
        raise RuntimeError(
            "Mach-O binary contains only unsupported architectures "
            f"({', '.join(sorted(_MACOS_DISALLOWED_MACHO_ARCHS))}): {path}\n"
            f"archs: {' '.join(archs)}"
        )

    logger.info(
        "Stripping unsupported architectures from %s: remove=%s keep=%s",
        path,
        " ".join(to_remove),
        " ".join(keep),
    )

    input_path = path
    tmp_path = f"{path}.lipo"
    for arch in to_remove:
        _macos_run_checked(["lipo", "-remove", arch, input_path, "-output", tmp_path])
        if input_path != path:
            try:
                os.remove(input_path)
            except OSError:
                pass
        input_path = tmp_path
        tmp_path = f"{tmp_path}.next"

    os.replace(input_path, path)


def _macos_zipinfo_clone(info: zipfile.ZipInfo) -> zipfile.ZipInfo:
    # `ZipInfo` contains read-time offsets that aren't relevant for writing. Recreate a
    # fresh object but preserve metadata so the rewritten jar stays well-formed.
    cloned = zipfile.ZipInfo(filename=info.filename, date_time=info.date_time)
    cloned.compress_type = info.compress_type
    cloned.comment = info.comment
    cloned.extra = info.extra
    cloned.create_system = info.create_system
    cloned.create_version = info.create_version
    cloned.extract_version = info.extract_version
    cloned.flag_bits = info.flag_bits
    cloned.volume = info.volume
    cloned.internal_attr = info.internal_attr
    cloned.external_attr = info.external_attr
    return cloned


def _macos_codesign_macho_files_in_jar(
    jar_path: str, *, identity: str, tmp_parent_dir: str
) -> None:
    """Codesign Mach-O binaries embedded inside a .jar file.

    Apple's notarization service scans nested archives (like .jar) and requires any
    embedded Mach-O binaries to be signed with the same Developer ID identity and a
    secure timestamp.
    """
    if identity == "-":
        return
    if not os.path.exists(jar_path):
        raise RuntimeError(f"Jar path does not exist: {jar_path}")

    replacements: dict[str, bytes] = {}
    excluded_entries: set[str] = set()

    with tempfile.TemporaryDirectory(
        prefix="atlas-jar-codesign-", dir=tmp_parent_dir
    ) as tmp_dir:
        with zipfile.ZipFile(jar_path, "r") as zin:
            for info in zin.infolist():
                if info.is_dir():
                    continue

                if info.filename in _MACOS_JAR_ENTRY_REMOVE_LIST:
                    logger.warning(
                        "Removing unsupported binary from %s: %s",
                        os.path.basename(jar_path),
                        info.filename,
                    )
                    excluded_entries.add(info.filename)
                    continue

                _, ext = os.path.splitext(info.filename)
                if ext.lower() not in _MACOS_MACHO_FILE_EXTENSIONS:
                    continue

                tmp_file_path = os.path.join(tmp_dir, *info.filename.split("/"))
                os.makedirs(os.path.dirname(tmp_file_path), exist_ok=True)
                with open(tmp_file_path, "wb") as f:
                    f.write(zin.read(info.filename))

                desc = _macos_file_description(tmp_file_path)
                if not desc or "Mach-O" not in desc:
                    continue

                _macos_run_checked(["chmod", "u+rw", tmp_file_path])
                if any(arch in desc.lower() for arch in _MACOS_DISALLOWED_MACHO_ARCHS):
                    _macos_strip_disallowed_macho_archs_in_place(tmp_file_path)
                _macos_codesign_target(
                    tmp_file_path,
                    identity=identity,
                    entitlements=None,
                    file_descriptions={},
                )

                with open(tmp_file_path, "rb") as f:
                    replacements[info.filename] = f.read()

        if not replacements and not excluded_entries:
            return

        logger.info(
            "Codesigning %d Mach-O files embedded in %s",
            len(replacements),
            os.path.basename(jar_path),
        )

        tmp_jar_path = f"{jar_path}.tmp"
        if os.path.exists(tmp_jar_path):
            os.remove(tmp_jar_path)

        with (
            zipfile.ZipFile(jar_path, "r") as zin,
            zipfile.ZipFile(tmp_jar_path, "w") as zout,
        ):
            zout.comment = zin.comment
            for info in zin.infolist():
                cloned = _macos_zipinfo_clone(info)
                if info.is_dir():
                    zout.writestr(cloned, b"")
                    continue
                if info.filename in excluded_entries:
                    continue
                payload = replacements.get(info.filename)
                if payload is None:
                    payload = zin.read(info.filename)
                zout.writestr(cloned, payload)

        os.replace(tmp_jar_path, jar_path)
        _macos_run_checked(["xattr", "-c", jar_path])


def _macos_codesign_macho_files_in_jars_under_dir(
    root_dir: str, *, identity: str, tmp_parent_dir: str
) -> None:
    if identity == "-":
        return
    for dirpath, dirnames, filenames in os.walk(root_dir, followlinks=False):
        dirnames[:] = [
            name for name in dirnames if not os.path.islink(os.path.join(dirpath, name))
        ]
        for filename in filenames:
            if not filename.lower().endswith(".jar"):
                continue
            jar_path = os.path.join(dirpath, filename)
            if os.path.islink(jar_path):
                continue
            _macos_codesign_macho_files_in_jar(
                jar_path, identity=identity, tmp_parent_dir=tmp_parent_dir
            )


def _macos_collect_signable_targets(root_path: str) -> tuple[list[str], dict[str, str]]:
    """Return (targets, file_descriptions) for staged signing.

    Targets include:
      - Mach-O executables/dylibs (regular files only; symlinks skipped)
      - Nested bundles (.app/.framework/...) as directories

    The returned list excludes root_path itself and is sorted deepest-first (inside-out).
    """
    if not os.path.isdir(root_path):
        return ([], {})

    targets: list[str] = []
    file_descriptions: dict[str, str] = {}

    for dirpath, dirnames, filenames in os.walk(root_path, followlinks=False):
        # Avoid traversing symlinked directories (common in .framework layouts).
        dirnames[:] = [
            name for name in dirnames if not os.path.islink(os.path.join(dirpath, name))
        ]

        for dirname in dirnames:
            child_dir = os.path.join(dirpath, dirname)
            if _macos_is_signable_bundle_dir(child_dir) and child_dir != root_path:
                targets.append(child_dir)

        for filename in filenames:
            file_path = os.path.join(dirpath, filename)
            if os.path.islink(file_path):
                continue

            _, ext = os.path.splitext(filename)
            ext = ext.lower()
            is_candidate = ext in _MACOS_MACHO_FILE_EXTENSIONS
            if not is_candidate:
                try:
                    mode = os.stat(file_path).st_mode
                except OSError:
                    continue
                is_candidate = bool(mode & (stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH))
            if not is_candidate:
                continue

            desc = _macos_file_description(file_path)
            if not desc or "Mach-O" not in desc:
                continue

            targets.append(file_path)
            file_descriptions[file_path] = desc

    def depth_within_root(path: str) -> int:
        rel = os.path.relpath(path, root_path)
        if rel == ".":
            return 0
        return rel.count(os.sep)

    # De-dupe by realpath to avoid double-signing through symlinked paths.
    unique_targets: list[str] = []
    seen_realpaths: set[str] = set()
    for target in targets:
        rp = os.path.realpath(target)
        if rp in seen_realpaths:
            continue
        seen_realpaths.add(rp)
        unique_targets.append(target)

    unique_targets.sort(key=lambda p: (-depth_within_root(p), p))
    return (unique_targets, file_descriptions)


def _macos_should_apply_entitlements(
    target_path: str, *, entitlements: Optional[str], file_descriptions: dict[str, str]
) -> bool:
    if not entitlements:
        return False
    if os.path.isdir(target_path):
        lower = target_path.lower()
        return lower.endswith((".app", ".appex", ".xpc"))

    desc = file_descriptions.get(target_path)
    if not desc:
        return False
    return "executable" in desc.lower()


def _macos_codesign_target(
    target_path: str,
    *,
    identity: str,
    entitlements: Optional[str],
    file_descriptions: dict[str, str],
) -> None:
    if identity != "-" and not os.path.isdir(target_path):
        desc = file_descriptions.get(target_path)
        if (
            desc
            and "Mach-O" in desc
            and any(arch in desc.lower() for arch in _MACOS_DISALLOWED_MACHO_ARCHS)
        ):
            _macos_strip_disallowed_macho_archs_in_place(target_path)

    cmd = ["codesign", "--force", "--sign", identity]
    if identity != "-":
        cmd = [
            "codesign",
            "--force",
            "--options",
            "runtime",
            "--timestamp",
            "--sign",
            identity,
        ]
        if _macos_should_apply_entitlements(
            target_path, entitlements=entitlements, file_descriptions=file_descriptions
        ):
            cmd.extend(["--entitlements", entitlements])

    cmd.append(target_path)
    _macos_run_checked(cmd)


def _macos_codesign_bundle(
    bundle_path: str, *, identity_override: Optional[str] = None
) -> None:
    if not common_dirs.is_mac():
        raise RuntimeError("_macos_codesign_bundle called on non-macOS")
    if not os.path.exists(bundle_path):
        raise RuntimeError(f"Bundle path does not exist: {bundle_path}")

    # Some bundled artifacts (notably JRE legal files) are shipped read-only or even
    # non-readable. `xattr -cr` needs write permission to clear extended attributes and
    # `ditto` needs read permission later when packaging for notarization. Make the bundle
    # user-readable/writable before stripping xattrs so these steps don't fail mid-tree.
    if os.path.isdir(bundle_path):
        _macos_run_checked(["chmod", "-R", "-P", "u+rwX", bundle_path])
    else:
        _macos_run_checked(["chmod", "u+rwX", bundle_path])
    _macos_run_checked(["xattr", "-cr", bundle_path])

    identity = (
        identity_override
        if identity_override is not None
        else _macos_codesign_identity()
    )
    entitlements = None if identity == "-" else _macos_entitlements_path()

    file_descriptions: dict[str, str] = {}
    if os.path.isdir(bundle_path):
        # Notary scans nested archives like .jar and requires embedded Mach-O binaries
        # to be signed. Fix these before signing the enclosing bundle.
        _macos_codesign_macho_files_in_jars_under_dir(
            bundle_path, identity=identity, tmp_parent_dir=os.path.dirname(bundle_path)
        )

        targets, file_descriptions = _macos_collect_signable_targets(bundle_path)
        if targets:
            logger.info(
                "Staged signing %d nested items (inside-out): %s",
                len(targets),
                os.path.basename(bundle_path),
            )
        for target in targets:
            _macos_codesign_target(
                target,
                identity=identity,
                entitlements=entitlements,
                file_descriptions=file_descriptions,
            )

    _macos_codesign_target(
        bundle_path,
        identity=identity,
        entitlements=entitlements,
        file_descriptions=file_descriptions,
    )

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

    # ditto can emit per-file permission errors while still returning success; treat any
    # such output as fatal so we don't notarize an incomplete zip.
    ditto_cmd = ["ditto", "-c", "-k", "--keepParent", bundle_name, notarize_zip]
    logger.info("Running: %s", " ".join(ditto_cmd))
    proc = subprocess.run(
        ditto_cmd,
        cwd=bundle_dir,
        shell=False,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    ditto_output = (proc.stdout or "").strip()
    if ditto_output:
        for line in ditto_output.splitlines():
            logger.error("%s", line)
    if proc.returncode != 0:
        raise subprocess.CalledProcessError(proc.returncode, ditto_cmd)
    if ditto_output:
        raise RuntimeError(
            "Failed creating notarization zip; `ditto` reported errors (see log above). "
            "This usually means some files inside the bundle are not readable by the current user.\n"
            f"bundle: {bundle_path}\n"
            "Fix by ensuring the bundle is user-readable before retrying, e.g.:\n"
            f"  chmod -R -P u+rwX {bundle_path}"
        )
    submit_cmd = ["xcrun", "notarytool", "submit", notarize_zip, "--wait", *auth_args]
    logger.info("Running: %s", " ".join(submit_cmd))
    proc = subprocess.Popen(
        submit_cmd,
        shell=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    id_regex = re.compile(r"^\s*id:\s*([0-9a-fA-F-]{36})\s*$")
    status_regex = re.compile(r"^\s*status:\s*(\S+)\s*$")
    submission_id: Optional[str] = None
    status: Optional[str] = None

    assert proc.stdout is not None
    for raw_line in proc.stdout:
        line = raw_line.rstrip("\n")
        if line:
            logger.info("%s", line.rstrip())
        match = id_regex.match(line)
        if match:
            submission_id = match.group(1)
            continue
        match = status_regex.match(line)
        if match:
            status = match.group(1)
            continue

    proc.wait()
    if proc.returncode != 0:
        raise subprocess.CalledProcessError(proc.returncode, submit_cmd)
    if not submission_id:
        raise RuntimeError("notarytool submit did not report a submission id")
    if not status:
        raise RuntimeError("notarytool submit did not report a final status")
    logger.info("Notarytool result: id=%s status=%s", submission_id, status)
    if status != "Accepted":
        log_path = os.path.join(bundle_dir, f"{bundle_name}.notarytool.log.json")
        try:
            _macos_run_checked(
                ["xcrun", "notarytool", "log", submission_id, log_path, *auth_args]
            )
        except subprocess.CalledProcessError:
            raise RuntimeError(
                "Notarization failed and fetching the notarization log also failed.\n"
                f"bundle: {bundle_path}\n"
                f"id: {submission_id}\n"
                f"status: {status}\n"
                "Re-run:\n"
                f"  xcrun notarytool log {submission_id} <output-path> {' '.join(auth_args)}"
            )
        try:
            with open(log_path, "r", encoding="utf-8") as f:
                log_text = f.read()
            logger.error("notarytool log (%s):\n%s", log_path, log_text)
        except Exception as e:
            logger.error("Failed printing notarytool log %s: %s", log_path, e)
        raise RuntimeError(
            "Notarization failed.\n"
            f"bundle: {bundle_path}\n"
            f"id: {submission_id}\n"
            f"status: {status}\n"
            f"log: {log_path}\n"
            "Open the log file above to see which embedded file failed validation."
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


def _macos_qtifw_archivegen_path() -> str:
    archivegen = os.path.join(common_dirs.qt_installer_framework_bin_dir(), "archivegen")
    if not os.path.exists(archivegen):
        raise RuntimeError(
            "Qt Installer Framework `archivegen` was not found at the configured QtIFW location.\n"
            f"Expected: {archivegen}\n"
            "Install QtIFW (required for building the installer), or fix your Qt installation path."
        )
    return archivegen


def _macos_sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _macos_relocate_non_code_entries_from_bundle_macos_to_resources(
    bundle_path: str,
) -> None:
    """Move non-code entries out of Contents/MacOS.

    Some third-party app bundles place data files under `Contents/MacOS/`, and
    `codesign` can treat those as nested code components (failing the sign
    operation). Relocate entries that contain no executables and no Mach-O code
    to `Contents/Resources/` and leave a symlink behind to preserve runtime
    paths.
    """
    contents_dir = os.path.join(bundle_path, "Contents")
    macos_dir = os.path.join(contents_dir, "MacOS")
    resources_dir = os.path.join(contents_dir, "Resources")
    if not os.path.isdir(macos_dir):
        return

    def is_code_file(file_path: str) -> bool:
        if os.path.islink(file_path):
            return False
        try:
            mode = os.stat(file_path).st_mode
        except OSError:
            return False
        if mode & (stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH):
            return True
        _, ext = os.path.splitext(file_path)
        if ext.lower() in _MACOS_MACHO_FILE_EXTENSIONS:
            return True
        desc = _macos_file_description(file_path)
        return bool(desc and "Mach-O" in desc)

    relocations: list[tuple[str, str, str]] = []
    for entry in sorted(os.listdir(macos_dir)):
        src_path = os.path.join(macos_dir, entry)
        if os.path.islink(src_path):
            continue
        if os.path.isdir(src_path):
            if _macos_is_signable_bundle_dir(src_path):
                continue

            contains_code = False
            for dirpath, dirnames, filenames in os.walk(src_path, followlinks=False):
                dirnames[:] = [
                    name
                    for name in dirnames
                    if not os.path.islink(os.path.join(dirpath, name))
                ]
                for filename in filenames:
                    file_path = os.path.join(dirpath, filename)
                    if is_code_file(file_path):
                        contains_code = True
                        break
                if contains_code:
                    break
            if contains_code:
                continue
        elif os.path.isfile(src_path):
            if is_code_file(src_path):
                continue
        else:
            continue

        dst_path = os.path.join(resources_dir, entry)
        if os.path.exists(dst_path):
            raise RuntimeError(
                "Cannot relocate non-code entry from Contents/MacOS because the target already exists:\n"
                f"bundle: {bundle_path}\n"
                f"src: {src_path}\n"
                f"dst: {dst_path}"
            )
        relocations.append((entry, src_path, dst_path))

    if not relocations:
        return

    os.makedirs(resources_dir, exist_ok=True)
    for entry, src_path, dst_path in relocations:
        logger.info(
            "Relocating non-code entry for codesign: %s -> %s (symlink kept)",
            src_path,
            dst_path,
        )
        shutil.move(src_path, dst_path)
        os.symlink(os.path.join("..", "Resources", entry), src_path)


def _macos_notarize_qtifw_package_dir(package_dir: str) -> None:
    """Notarize/staple .app bundles inside a QtIFW package's data archives.

    Qt Installer Framework packages typically store payloads under `data/*.7z`. Apple
    can't notarize a `.7z` directly, so we extract, sign+notarize the contained `.app`,
    then repack the archive so the installer can ship the stapled ticket.
    """
    if not common_dirs.is_mac():
        raise RuntimeError("_macos_notarize_qtifw_package_dir called on non-macOS")
    if not os.path.isdir(package_dir):
        raise RuntimeError(f"QtIFW package dir not found: {package_dir}")

    data_dir = os.path.join(package_dir, "data")
    if not os.path.isdir(data_dir):
        raise RuntimeError(f"QtIFW package missing data/ dir: {package_dir}")

    archivegen = _macos_qtifw_archivegen_path()
    archives = sorted(
        f
        for f in os.listdir(data_dir)
        if f.lower().endswith(".7z") and os.path.isfile(os.path.join(data_dir, f))
    )
    if not archives:
        raise RuntimeError(f"No .7z archives found under: {data_dir}")

    for archive_name in archives:
        archive_path = os.path.join(data_dir, archive_name)
        logger.info("Processing QtIFW payload: %s", archive_path)
        before_hash = _macos_sha256(archive_path)

        with tempfile.TemporaryDirectory(prefix="atlas-qtifw-", dir=data_dir) as tmp_dir:
            extract_dir = os.path.join(tmp_dir, "extract")
            os.makedirs(extract_dir, exist_ok=True)

            common_dirs.unpack_file_to_folder(archive_path, extract_dir)

            app_paths: list[str] = []
            for dirpath, dirnames, _filenames in os.walk(extract_dir, followlinks=False):
                dirnames[:] = [
                    name
                    for name in dirnames
                    if not os.path.islink(os.path.join(dirpath, name))
                ]
                for dirname in dirnames:
                    if dirname.lower().endswith(".app"):
                        app_paths.append(os.path.join(dirpath, dirname))

            if not app_paths:
                raise RuntimeError(
                    "QtIFW archive did not contain any .app bundles to notarize: "
                    f"{archive_path}"
                )

            # Notarize each embedded app; this is what Gatekeeper will evaluate after install.
            for app_path in sorted(app_paths):
                logger.info("Signing/notarizing embedded app: %s", app_path)
                _macos_relocate_non_code_entries_from_bundle_macos_to_resources(
                    app_path
                )
                _macos_codesign_bundle(app_path)
                _macos_notarize_and_staple_bundle(app_path)

            root_items = [
                name
                for name in sorted(os.listdir(extract_dir))
                if name not in {".DS_Store", "__MACOSX"} and not name.startswith("._")
            ]
            if not root_items:
                raise RuntimeError(f"QtIFW archive extracted empty: {archive_path}")

            archive_stem, _archive_ext = os.path.splitext(archive_name)
            tmp_archive_path = os.path.join(tmp_dir, f"{archive_stem}.tmp.7z")
            cmd = [archivegen, "--format", "7z", "--compression", "5", tmp_archive_path, *root_items]
            logger.info("Running: %s", " ".join(cmd))
            subprocess.run(cmd, cwd=extract_dir, shell=False, check=True)
            if not os.path.exists(tmp_archive_path):
                # `archivegen` appends the format extension when the output path doesn't end
                # with it. Always expect `.7z` output but guard with a clear error.
                appended = f"{tmp_archive_path}.7z"
                if os.path.exists(appended):
                    tmp_archive_path = appended
                else:
                    raise RuntimeError(
                        "QtIFW `archivegen` reported success but did not create the expected archive.\n"
                        f"expected: {tmp_archive_path}\n"
                        f"also checked: {appended}\n"
                        f"cwd: {extract_dir}"
                    )

            backup_path = f"{archive_path}.bak"
            try:
                os.remove(backup_path)
            except OSError:
                pass
            os.replace(archive_path, backup_path)
            os.replace(tmp_archive_path, archive_path)

        after_hash = _macos_sha256(archive_path)
        logger.info(
            "Updated %s: sha256 %s -> %s", archive_name, before_hash, after_hash
        )


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
    tree.find(
        "Version"
    ).text = "4.7.1"  # todo: get version and date from qt components.xml
    tree.find("ReleaseDate").text = "2024-02-16"
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
            _macos_codesign_bundle(deployed_app_path, identity_override="-")
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
        mt_app_name = 'MaintenanceTool.app'
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
    if common_dirs.is_mac():
        if os.path.exists(mt_app_path):
            shutil.rmtree(mt_app_path, ignore_errors=False, onexc=common_dirs.handleRemoveReadonly)

        subprocess.run(
            [
                os.path.join(common_dirs.qt_installer_framework_bin_dir(), "binarycreator"),
                "-c",
                os.path.join("config", "config-macOS.xml"),
                "--mt",
            ],
            cwd=common_dirs.deploy_target_dir(),
            shell=False,
            check=True,
        )
        if not os.path.isdir(mt_app_path):
            raise RuntimeError(
                "QtIFW did not create the expected maintenance tool bundle.\n"
                f"Expected: {mt_app_path}\n"
                "Check <MaintenanceToolName> in config/config-macOS.xml and your QtIFW installation."
            )

        if _macos_signing_disabled():
            _macos_codesign_bundle(mt_app_path, identity_override="-")
        else:
            _macos_codesign_bundle(mt_app_path)
            _macos_notarize_and_staple_bundle(mt_app_path)

        subprocess.run(
            [
                os.path.join(common_dirs.qt_installer_framework_bin_dir(), "archivegen"),
                "--compression",
                "5",
                mt_repo_package_name,
                mt_app_name,
            ],
            cwd=common_dirs.deploy_target_dir(),
            shell=False,
            check=True,
        )
        shutil.rmtree(mt_app_path, ignore_errors=False, onexc=common_dirs.handleRemoveReadonly)
    else:
        if os.path.exists(mt_app_path):
            os.remove(mt_app_path)
        if common_dirs.is_windows():
            shutil.copy(
                os.path.join(common_dirs.qt_installer_framework_bin_dir(), "installerbase.exe"),
                os.path.join(common_dirs.deploy_target_dir(), mt_app_name),
            )
        else:
            shutil.copy(
                os.path.join(common_dirs.qt_installer_framework_bin_dir(), "installerbase"),
                mt_app_path,
            )
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
            _macos_codesign_bundle(installer_app_path, identity_override="-")
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
            shutil.copytree(
                os.path.join(common_dirs.deploy_target_dir(), suffix),
                os.path.join(target_folder, suffix),
                symlinks=True,
            )
        elif common_dirs.is_windows():
            out_folder = common_dirs.static_deploy_folder()
            shutil.copy2(os.path.join(common_dirs.deploy_target_dir(), installer_zip_name),
                         os.path.join(out_folder, 'installers', installer_zip_name))
            target_folder = os.path.join(out_folder, 'packages')
            if os.path.exists(os.path.join(target_folder, suffix)):
                shutil.rmtree(os.path.join(target_folder, suffix), ignore_errors=False,
                              onexc=common_dirs.handleRemoveReadonly)
            shutil.copytree(
                os.path.join(common_dirs.deploy_target_dir(), suffix),
                os.path.join(target_folder, suffix),
                symlinks=True,
            )


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
    parser.add_argument(
        "--notarize-qtifw-package",
        metavar="DIR",
        help="Notarize/staple .app payloads inside a Qt Installer Framework package dir (rewrites data/*.7z in-place).",
    )
    args = parser.parse_args()

    if args.notarize_qtifw_package:
        _macos_notarize_qtifw_package_dir(args.notarize_qtifw_package)
        sys.exit(0)

    deploy_atlas(is_debug_version=args.debug_version)
