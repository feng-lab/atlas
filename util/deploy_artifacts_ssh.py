from __future__ import annotations

import argparse
import base64
import os
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


_DEFAULT_ATTEMPTS = 5
_DEFAULT_BASE_DELAY_SECONDS = 5
_DEFAULT_MAX_DELAY_SECONDS = 60
# Bound each SSH/transfer subprocess so a stalled network copy does not hold a
# CI runner forever. This is configurable with --command-timeout.
_DEFAULT_COMMAND_TIMEOUT_SECONDS = 5 * 60
_SCHEMA_FILES: tuple[str, ...] = (
    "animation3d.schema.json",
    "capabilities.json",
    "supported_file_formats.json",
)


@dataclass(frozen=True)
class SshConfig:
    server: str
    port: str
    username: str
    deploy_path: str
    key_path: Path
    known_hosts_path: Path

    @property
    def remote(self) -> str:
        return f"{self.username}@{self.server}"


@dataclass(frozen=True)
class DeployInputs:
    repo_root: Path
    deploy_dir: Path
    os_name: str
    include_schema: bool
    schema_dir: Path


@dataclass(frozen=True)
class CommandPolicy:
    attempts: int
    base_delay: int
    max_delay: int
    timeout_seconds: int | None


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def _required_env(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        raise RuntimeError(
            f"{name} is not set. Provide it via the ATLAS_ENV_LOCAL dotenv secret."
        )
    return value


def _decode_base64_env(name: str) -> bytes:
    value = _required_env(name)
    value = value.replace("\r", "").replace("\n", "").strip()
    while value.endswith("%"):
        value = value[:-1]
    if not value:
        raise RuntimeError(f"{name} is empty after normalization")
    try:
        return base64.b64decode(value, validate=True)
    except Exception as e:
        raise RuntimeError(f"{name} is not valid base64") from e


def _path_arg(path: Path) -> str:
    # Use forward slashes on Windows so OpenSSH/Git-for-Windows do not parse drive
    # separators in local source paths as remote scp syntax.
    return str(path).replace("\\", "/")


def _local_arg(path: Path, *, cwd: Path) -> str:
    try:
        return path.resolve().relative_to(cwd.resolve()).as_posix()
    except ValueError:
        return _path_arg(path.resolve())


def _chmod_private(path: Path) -> None:
    try:
        path.chmod(stat.S_IRUSR | stat.S_IWUSR)
    except OSError:
        pass

    chmod = shutil.which("chmod")
    if chmod:
        subprocess.run(
            [chmod, "600", _path_arg(path)],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )


def _write_ssh_material(temp_dir: Path) -> tuple[Path, Path]:
    key_path = temp_dir / "deploy.key"
    known_hosts_path = temp_dir / "known_hosts"

    key_path.write_bytes(_decode_base64_env("SSH_PRIVATE_KEY_BASE64"))
    _chmod_private(key_path)

    known_hosts = _decode_base64_env("SSH_KNOWN_HOSTS_BASE64")
    known_hosts_path.write_bytes(known_hosts.rstrip(b"\r\n") + b"\n")
    return key_path, known_hosts_path


def _ssh_common_args(config: SshConfig) -> list[str]:
    return [
        "-i",
        _path_arg(config.key_path),
        "-o",
        f"UserKnownHostsFile={_path_arg(config.known_hosts_path)}",
        "-o",
        "StrictHostKeyChecking=yes",
        "-o",
        "LogLevel=ERROR",
    ]


def _ssh_command(config: SshConfig) -> list[str]:
    return ["ssh", "-p", config.port, *_ssh_common_args(config)]


def _scp_command(config: SshConfig) -> list[str]:
    return ["scp", "-P", config.port, *_ssh_common_args(config)]


def _rsync_ssh_command(config: SshConfig) -> str:
    return " ".join(shlex.quote(part) for part in _ssh_command(config))


def _quote_remote_path(path: str) -> str:
    return shlex.quote(path.rstrip("/"))


def _remote_join(base: str, *parts: str) -> str:
    joined = base.rstrip("/")
    for part in parts:
        joined += "/" + part.strip("/")
    return joined


def _remote_target(config: SshConfig, remote_dir: str) -> str:
    return f"{config.remote}:{remote_dir.rstrip('/')}/"


def _run_with_retry(
    command: Sequence[str],
    *,
    label: str,
    cwd: Path,
    dry_run: bool,
    policy: CommandPolicy,
) -> None:
    print(f"+ {label}", flush=True)
    if dry_run:
        return

    for attempt in range(1, policy.attempts + 1):
        try:
            completed = subprocess.run(
                command,
                cwd=cwd,
                check=False,
                timeout=policy.timeout_seconds,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        except subprocess.TimeoutExpired as e:
            if attempt >= policy.attempts:
                timeout = policy.timeout_seconds
                raise RuntimeError(
                    f"{label} timed out after {timeout}s and "
                    f"{policy.attempts} attempt(s)"
                ) from e

            sleep_for = min(
                policy.base_delay * (2 ** (attempt - 1)),
                policy.max_delay,
            )
            timeout = policy.timeout_seconds
            print(
                f"{label} timed out after {timeout}s; retrying in {sleep_for}s "
                f"({attempt}/{policy.attempts})",
                file=sys.stderr,
                flush=True,
            )
            time.sleep(sleep_for)
            continue

        if completed.returncode == 0:
            return
        if attempt >= policy.attempts:
            raise RuntimeError(
                f"{label} failed after {policy.attempts} attempt(s) "
                f"with exit code {completed.returncode}; command output was "
                "suppressed to avoid exposing SSH deployment details"
            )

        sleep_for = min(
            policy.base_delay * (2 ** (attempt - 1)),
            policy.max_delay,
        )
        print(
            f"{label} failed (exit {completed.returncode}); retrying in "
            f"{sleep_for}s ({attempt}/{policy.attempts})",
            file=sys.stderr,
            flush=True,
        )
        time.sleep(sleep_for)


def _ensure_remote_dirs(
    config: SshConfig,
    *,
    cwd: Path,
    dry_run: bool,
    policy: CommandPolicy,
) -> None:
    packages_dir = _remote_join(config.deploy_path, "packages")
    installers_dir = _remote_join(config.deploy_path, "installers")
    remote_command = "mkdir -p {} {}".format(
        _quote_remote_path(packages_dir),
        _quote_remote_path(installers_dir),
    )
    _run_with_retry(
        [*_ssh_command(config), config.remote, remote_command],
        label="create remote deploy directories",
        cwd=cwd,
        dry_run=dry_run,
        policy=policy,
    )


def _remove_remote_package_dir(
    config: SshConfig,
    *,
    os_name: str,
    cwd: Path,
    dry_run: bool,
    policy: CommandPolicy,
) -> None:
    package_dir = _remote_join(config.deploy_path, "packages", os_name)
    remote_command = f"rm -rf {_quote_remote_path(package_dir)}"
    _run_with_retry(
        [*_ssh_command(config), config.remote, remote_command],
        label=f"remove remote package directory for {os_name}",
        cwd=cwd,
        dry_run=dry_run,
        policy=policy,
    )


def _collect_inputs(args: argparse.Namespace) -> DeployInputs:
    repo_root = _repo_root()
    return DeployInputs(
        repo_root=repo_root,
        deploy_dir=repo_root / "deploy",
        os_name=args.os,
        include_schema=args.include_schema,
        schema_dir=repo_root / "src" / "atlas" / "Resources" / "json" / "atlas",
    )


def _installer_names_for_os(os_name: str) -> tuple[str, ...]:
    if os_name == "macOS":
        return ("AtlasInstaller-macOS-universal.zip", "AtlasInstaller-macOS.zip")
    return (f"AtlasInstaller-{os_name}.zip",)


def _validate_inputs(inputs: DeployInputs) -> tuple[Path, list[Path], list[Path]]:
    package_dir = inputs.deploy_dir / inputs.os_name
    if not package_dir.is_dir():
        raise FileNotFoundError(
            f"Expected package directory does not exist: {package_dir}"
        )

    installers: list[Path] = []
    for filename in _installer_names_for_os(inputs.os_name):
        installer = inputs.deploy_dir / filename
        if installer.is_file():
            installers.append(installer)
            break
    if not installers:
        existing = sorted(
            path.name for path in inputs.deploy_dir.glob("AtlasInstaller*.zip")
        )
        expected = ", ".join(_installer_names_for_os(inputs.os_name))
        found = ", ".join(existing) if existing else "<none>"
        raise FileNotFoundError(
            f"Expected installer for {inputs.os_name} was not found in "
            f"{inputs.deploy_dir}. "
            f"Expected one of: {expected}. Found: {found}"
        )

    schema_files: list[Path] = []
    if inputs.include_schema:
        for filename in _SCHEMA_FILES:
            path = inputs.schema_dir / filename
            if not path.is_file():
                raise FileNotFoundError(f"Expected schema file does not exist: {path}")
            schema_files.append(path)

    return package_dir, installers, schema_files


def _load_config(key_path: Path, known_hosts_path: Path) -> SshConfig:
    return SshConfig(
        server=_required_env("SSH_SERVER"),
        port=_required_env("SSH_PORT"),
        username=_required_env("SSH_USERNAME"),
        deploy_path=_required_env("SSH_DEPLOY_PATH").rstrip("/"),
        key_path=key_path,
        known_hosts_path=known_hosts_path,
    )


def _deploy_with_rsync(
    config: SshConfig,
    inputs: DeployInputs,
    package_dir: Path,
    installers: Sequence[Path],
    schema_files: Sequence[Path],
    *,
    dry_run: bool,
    policy: CommandPolicy,
) -> None:
    rsync_ssh = _rsync_ssh_command(config)
    packages_target = _remote_target(
        config, _remote_join(config.deploy_path, "packages")
    )
    installers_target = _remote_target(
        config, _remote_join(config.deploy_path, "installers")
    )

    _run_with_retry(
        [
            "rsync",
            "-a",
            "--delete",
            "-e",
            rsync_ssh,
            _local_arg(package_dir, cwd=inputs.repo_root),
            packages_target,
        ],
        label=f"upload {inputs.os_name} package directory with rsync",
        cwd=inputs.repo_root,
        dry_run=dry_run,
        policy=policy,
    )
    _run_with_retry(
        [
            "rsync",
            "-t",
            "-e",
            rsync_ssh,
            *[_local_arg(path, cwd=inputs.repo_root) for path in installers],
            installers_target,
        ],
        label=f"upload {inputs.os_name} installer artifact with rsync",
        cwd=inputs.repo_root,
        dry_run=dry_run,
        policy=policy,
    )
    if schema_files:
        _run_with_retry(
            [
                "rsync",
                "-t",
                "-e",
                rsync_ssh,
                *[_local_arg(path, cwd=inputs.repo_root) for path in schema_files],
                installers_target,
            ],
            label=f"upload {inputs.os_name} schema artifacts with rsync",
            cwd=inputs.repo_root,
            dry_run=dry_run,
            policy=policy,
        )


def _deploy_with_scp(
    config: SshConfig,
    inputs: DeployInputs,
    package_dir: Path,
    installers: Sequence[Path],
    schema_files: Sequence[Path],
    *,
    dry_run: bool,
    policy: CommandPolicy,
) -> None:
    packages_target = _remote_target(
        config, _remote_join(config.deploy_path, "packages")
    )
    installers_target = _remote_target(
        config, _remote_join(config.deploy_path, "installers")
    )

    _remove_remote_package_dir(
        config,
        os_name=inputs.os_name,
        cwd=inputs.repo_root,
        dry_run=dry_run,
        policy=policy,
    )
    _run_with_retry(
        [
            *_scp_command(config),
            "-r",
            _local_arg(package_dir, cwd=inputs.repo_root),
            packages_target,
        ],
        label=f"upload {inputs.os_name} package directory with scp",
        cwd=inputs.repo_root,
        dry_run=dry_run,
        policy=policy,
    )
    _run_with_retry(
        [
            *_scp_command(config),
            *[_local_arg(path, cwd=inputs.repo_root) for path in installers],
            installers_target,
        ],
        label=f"upload {inputs.os_name} installer artifact with scp",
        cwd=inputs.repo_root,
        dry_run=dry_run,
        policy=policy,
    )
    if schema_files:
        _run_with_retry(
            [
                *_scp_command(config),
                *[_local_arg(path, cwd=inputs.repo_root) for path in schema_files],
                installers_target,
            ],
            label=f"upload {inputs.os_name} schema artifacts with scp",
            cwd=inputs.repo_root,
            dry_run=dry_run,
            policy=policy,
        )


def _resolve_method(requested: str) -> str:
    if requested != "auto":
        if requested == "rsync" and not shutil.which("rsync"):
            raise RuntimeError("rsync was requested but is not available on PATH")
        if requested == "scp" and not shutil.which("scp"):
            raise RuntimeError("scp was requested but is not available on PATH")
        return requested
    if shutil.which("rsync"):
        return "rsync"
    if shutil.which("scp"):
        return "scp"
    raise RuntimeError("Neither rsync nor scp is available on PATH")


def _collect_command_policy(args: argparse.Namespace) -> CommandPolicy:
    if args.attempts < 1:
        raise ValueError("--attempts must be at least 1")
    if args.base_delay < 0:
        raise ValueError("--base-delay must be non-negative")
    if args.max_delay < 0:
        raise ValueError("--max-delay must be non-negative")
    if args.command_timeout < 0:
        raise ValueError("--command-timeout must be non-negative")

    timeout_seconds = args.command_timeout if args.command_timeout else None
    return CommandPolicy(
        attempts=args.attempts,
        base_delay=args.base_delay,
        max_delay=args.max_delay,
        timeout_seconds=timeout_seconds,
    )


def deploy(args: argparse.Namespace) -> None:
    inputs = _collect_inputs(args)
    package_dir, installers, schema_files = _validate_inputs(inputs)
    policy = _collect_command_policy(args)

    with tempfile.TemporaryDirectory(prefix="atlas-ssh-") as temp_dir:
        key_path, known_hosts_path = _write_ssh_material(Path(temp_dir))
        config = _load_config(key_path, known_hosts_path)
        method = _resolve_method(args.method)

        print(
            f"Deploying {inputs.os_name} artifacts with {method} "
            "to the SSH package server",
            flush=True,
        )
        _ensure_remote_dirs(
            config,
            cwd=inputs.repo_root,
            dry_run=args.dry_run,
            policy=policy,
        )
        if method == "rsync":
            _deploy_with_rsync(
                config,
                inputs,
                package_dir,
                installers,
                schema_files,
                dry_run=args.dry_run,
                policy=policy,
            )
        else:
            _deploy_with_scp(
                config,
                inputs,
                package_dir,
                installers,
                schema_files,
                dry_run=args.dry_run,
                policy=policy,
            )


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Publish Atlas CI deploy artifacts to the SSH package server."
    )
    parser.add_argument(
        "--os",
        required=True,
        choices=("Linux", "macOS", "Windows"),
        help="Artifact platform name and deploy/<os> package directory to publish.",
    )
    parser.add_argument(
        "--include-schema",
        action="store_true",
        help=(
            "Also upload generated Atlas JSON schema/capabilities files to installers/."
        ),
    )
    parser.add_argument(
        "--method",
        choices=("auto", "rsync", "scp"),
        default="auto",
        help="Transfer tool. auto prefers rsync and falls back to scp.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help=(
            "Validate inputs and print planned operations without contacting the "
            "remote server."
        ),
    )
    parser.add_argument("--attempts", type=int, default=_DEFAULT_ATTEMPTS)
    parser.add_argument("--base-delay", type=int, default=_DEFAULT_BASE_DELAY_SECONDS)
    parser.add_argument("--max-delay", type=int, default=_DEFAULT_MAX_DELAY_SECONDS)
    parser.add_argument(
        "--command-timeout",
        type=int,
        default=_DEFAULT_COMMAND_TIMEOUT_SECONDS,
        help=(
            "Timeout in seconds for each ssh/rsync/scp command before retrying. "
            "Use 0 to disable. Defaults to "
            f"{_DEFAULT_COMMAND_TIMEOUT_SECONDS}."
        ),
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    try:
        deploy(parse_args(sys.argv[1:] if argv is None else argv))
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
