from __future__ import annotations

import argparse
import base64
import json
import os
import secrets
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence


_KEYCHAIN_NAME = "atlas-codesign.keychain-db"
_CERT_P12_NAME = "atlas-codesign-cert.p12"
_NOTARY_P8_NAME = "atlas-notary-key.p8"
_STATE_NAME = "state.json"
_REQUIRED_SETUP_ENV: tuple[str, ...] = (
    "MACOS_CODESIGN_IDENTITY",
    "MACOS_CODESIGN_P12_BASE64",
    "MACOS_CODESIGN_P12_PASSWORD",
    "MACOS_NOTARYTOOL_API_KEY_ID",
    "MACOS_NOTARYTOOL_API_ISSUER_ID",
    "MACOS_NOTARYTOOL_API_KEY_P8_BASE64",
)


@dataclass(frozen=True)
class SigningPaths:
    work_dir: Path
    keychain: Path
    cert_p12: Path
    notary_p8: Path
    state: Path


@dataclass(frozen=True)
class SigningState:
    paths: SigningPaths
    original_default_keychain: str
    original_keychains: tuple[str, ...]


def _default_work_dir() -> Path:
    return Path(tempfile.gettempdir()) / "atlas-macos-signing"


def _signing_paths(work_dir: Path) -> SigningPaths:
    work_dir = work_dir.resolve()
    return SigningPaths(
        work_dir=work_dir,
        keychain=work_dir / _KEYCHAIN_NAME,
        cert_p12=work_dir / _CERT_P12_NAME,
        notary_p8=work_dir / _NOTARY_P8_NAME,
        state=work_dir / _STATE_NAME,
    )


def _required_env(name: str) -> str:
    value = os.environ.get(name, "")
    if not value:
        raise RuntimeError(f"{name} is not set")
    return value


def _require_setup_env() -> None:
    missing = [name for name in _REQUIRED_SETUP_ENV if not os.environ.get(name)]
    if missing:
        raise RuntimeError(
            "macOS signing/notarization setup is missing required env vars: "
            + ", ".join(missing)
        )


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


def _write_private_file(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    try:
        path.chmod(0o600)
    except OSError:
        pass


def _remove_file(path: Path) -> None:
    try:
        path.unlink()
    except OSError:
        pass


def _security_output(args: Sequence[str]) -> str:
    completed = subprocess.run(
        ["security", *args],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        return ""
    return completed.stdout


def _run_security(args: Sequence[str], *, warning: bool = False) -> bool:
    completed = subprocess.run(
        ["security", *args],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if completed.returncode == 0:
        return True

    subcommand = args[0] if args else "<unknown>"
    severity = "Warning" if warning else "Error"
    print(
        f"{severity}: security {subcommand} failed "
        "(output suppressed for CI log safety).",
        file=sys.stderr,
        flush=True,
    )
    if warning:
        return False
    raise RuntimeError(f"security {subcommand} failed")


def _normalize_security_path(line: str) -> str:
    return line.strip().removeprefix('"').removesuffix('"')


def _current_default_keychain(paths: SigningPaths) -> str:
    default = _normalize_security_path(_security_output(["default-keychain"]))
    if default == str(paths.keychain):
        return ""
    return default


def _current_user_keychains(paths: SigningPaths) -> tuple[str, ...]:
    keychains: list[str] = []
    for line in _security_output(["list-keychains", "-d", "user"]).splitlines():
        keychain = _normalize_security_path(line)
        if keychain and keychain != str(paths.keychain):
            keychains.append(keychain)
    return tuple(keychains)


def _write_state(state: SigningState) -> None:
    state.paths.work_dir.mkdir(parents=True, exist_ok=True)
    payload = {
        "keychain": str(state.paths.keychain),
        "cert_p12": str(state.paths.cert_p12),
        "notary_p8": str(state.paths.notary_p8),
        "original_default_keychain": state.original_default_keychain,
        "original_keychains": list(state.original_keychains),
    }
    state.paths.state.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    try:
        state.paths.state.chmod(0o600)
    except OSError:
        pass


def _state_from_payload(work_dir: Path, payload: dict[str, Any]) -> SigningState:
    paths = _signing_paths(work_dir)
    return SigningState(
        paths=SigningPaths(
            work_dir=paths.work_dir,
            keychain=Path(str(payload.get("keychain", paths.keychain))),
            cert_p12=Path(str(payload.get("cert_p12", paths.cert_p12))),
            notary_p8=Path(str(payload.get("notary_p8", paths.notary_p8))),
            state=paths.state,
        ),
        original_default_keychain=str(payload.get("original_default_keychain", "")),
        original_keychains=tuple(
            str(value) for value in payload.get("original_keychains", [])
        ),
    )


def _load_state(work_dir: Path) -> SigningState | None:
    paths = _signing_paths(work_dir)
    try:
        payload = json.loads(paths.state.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None
    except (OSError, json.JSONDecodeError):
        return SigningState(
            paths=paths,
            original_default_keychain="",
            original_keychains=(),
        )
    if not isinstance(payload, dict):
        return SigningState(
            paths=paths,
            original_default_keychain="",
            original_keychains=(),
        )
    return _state_from_payload(work_dir, payload)


def _capture_state(work_dir: Path) -> SigningState:
    paths = _signing_paths(work_dir)
    return SigningState(
        paths=paths,
        original_default_keychain=_current_default_keychain(paths),
        original_keychains=_current_user_keychains(paths),
    )


def _setup(work_dir: Path) -> SigningState:
    _require_setup_env()
    state = _capture_state(work_dir)
    _write_state(state)

    _write_private_file(
        state.paths.cert_p12,
        _decode_base64_env("MACOS_CODESIGN_P12_BASE64"),
    )
    _write_private_file(
        state.paths.notary_p8,
        _decode_base64_env("MACOS_NOTARYTOOL_API_KEY_P8_BASE64"),
    )

    keychain_password = secrets.token_hex(16)
    _run_security(
        ["create-keychain", "-p", keychain_password, str(state.paths.keychain)]
    )
    _run_security(["set-keychain-settings", "-lut", "21600", str(state.paths.keychain)])
    _run_security(
        ["unlock-keychain", "-p", keychain_password, str(state.paths.keychain)]
    )
    _run_security(
        [
            "import",
            str(state.paths.cert_p12),
            "-k",
            str(state.paths.keychain),
            "-P",
            _required_env("MACOS_CODESIGN_P12_PASSWORD"),
            "-T",
            "/usr/bin/codesign",
            "-T",
            "/usr/bin/security",
        ]
    )
    _run_security(
        [
            "set-key-partition-list",
            "-S",
            "apple-tool:,apple:,codesign:",
            "-s",
            "-k",
            keychain_password,
            str(state.paths.keychain),
        ]
    )
    _run_security(
        [
            "list-keychains",
            "-d",
            "user",
            "-s",
            str(state.paths.keychain),
            *state.original_keychains,
        ]
    )
    _run_security(["default-keychain", "-s", str(state.paths.keychain)])

    identities = _security_output(
        ["find-identity", "-v", "-p", "codesigning", str(state.paths.keychain)]
    )
    if not identities:
        raise RuntimeError(
            "failed to query codesigning identities in the temporary keychain"
        )
    if _required_env("MACOS_CODESIGN_IDENTITY") not in identities:
        raise RuntimeError(
            "MACOS_CODESIGN_IDENTITY does not match any valid codesigning "
            "identity in the temporary keychain"
        )

    print("Codesigning identity available in temp keychain.", flush=True)
    print("Configured macOS signing and notarization assets.", flush=True)
    return state


def _fallback_default_keychain(state: SigningState) -> str:
    original = state.original_default_keychain
    if original and original != str(state.paths.keychain) and Path(original).is_file():
        return original

    for candidate in (
        Path.home() / "Library" / "Keychains" / "login.keychain-db",
        Path.home() / "Library" / "Keychains" / "login.keychain",
        Path("/Library/Keychains/System.keychain"),
    ):
        if str(candidate) != str(state.paths.keychain) and candidate.is_file():
            return str(candidate)
    return ""


def _cleanup_state(state: SigningState) -> None:
    restore_default = _fallback_default_keychain(state)
    if restore_default:
        _run_security(["default-keychain", "-s", restore_default], warning=True)

    original_keychains = [
        keychain
        for keychain in state.original_keychains
        if keychain and keychain != str(state.paths.keychain)
    ]
    if original_keychains:
        _run_security(
            ["list-keychains", "-d", "user", "-s", *original_keychains],
            warning=True,
        )
    elif restore_default:
        _run_security(
            ["list-keychains", "-d", "user", "-s", restore_default],
            warning=True,
        )

    if state.paths.keychain.is_file():
        _run_security(["delete-keychain", str(state.paths.keychain)], warning=True)
    _remove_file(state.paths.notary_p8)
    _remove_file(state.paths.cert_p12)
    _remove_file(state.paths.state)


def _cleanup(work_dir: Path) -> None:
    state = _load_state(work_dir)
    if state is None:
        paths = _signing_paths(work_dir)
        if not (
            paths.keychain.exists()
            or paths.cert_p12.exists()
            or paths.notary_p8.exists()
        ):
            print("Cleaned up macOS signing and notarization assets.", flush=True)
            return
        state = SigningState(
            paths=paths,
            original_default_keychain="",
            original_keychains=(),
        )
    _cleanup_state(state)
    print("Cleaned up macOS signing and notarization assets.", flush=True)


def _run_signed(work_dir: Path, command: Sequence[str]) -> int:
    if not command:
        raise RuntimeError("missing command after 'run --'")

    state: SigningState | None = None
    child_returncode = 1
    try:
        state = _setup(work_dir)
        child_env = os.environ.copy()
        child_env["MACOS_NOTARYTOOL_API_KEY_PATH"] = str(state.paths.notary_p8)
        child_env.pop("MACOS_CODESIGN_P12_BASE64", None)
        child_env.pop("MACOS_CODESIGN_P12_PASSWORD", None)
        child_env.pop("MACOS_NOTARYTOOL_API_KEY_P8_BASE64", None)
        completed = subprocess.run(command, check=False, env=child_env)
        child_returncode = completed.returncode
    finally:
        cleanup_state = state if state is not None else _load_state(work_dir)
        if cleanup_state is not None:
            _cleanup_state(cleanup_state)
            print("Cleaned up macOS signing and notarization assets.", flush=True)
    return child_returncode


def _add_work_dir_arg(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--work-dir",
        type=Path,
        default=_default_work_dir(),
        help=(
            "Directory for temporary signing assets and cleanup state. Defaults "
            "to a system temporary directory."
        ),
    )


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run a command with Atlas macOS CI signing assets."
    )
    subparsers = parser.add_subparsers(dest="command_name", required=True)

    run_parser = subparsers.add_parser(
        "run",
        help="Set up signing, run a command with signing env, then clean up.",
    )
    _add_work_dir_arg(run_parser)
    run_parser.add_argument(
        "command",
        nargs=argparse.REMAINDER,
        help="Command to run after '--'.",
    )

    cleanup_parser = subparsers.add_parser(
        "cleanup",
        help="Best-effort cleanup for a previous interrupted run.",
    )
    _add_work_dir_arg(cleanup_parser)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    command = getattr(args, "command", ())
    if command and command[0] == "--":
        command = command[1:]

    try:
        if args.command_name == "run":
            return _run_signed(args.work_dir, command)
        _cleanup(args.work_dir)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
