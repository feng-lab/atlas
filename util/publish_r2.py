import argparse
import logging
from pathlib import Path

import atlas_r2
import common_dirs
import download_utils
from atlas_deps_filelist import files_to_download as atlas_deps_files
from atlas_runtime_assets_filelist import (
    files_to_download as atlas_runtime_assets_files,
)
from atlas_test_data_filelist import files_to_download as atlas_test_data_files
from logger import setup_logger

logger = logging.getLogger(__name__)

_IFW_SCHEMA_FILES: tuple[str, ...] = (
    "animation3d.schema.json",
    "capabilities.json",
    "supported_file_formats.json",
)
_SKIP_FILENAMES: frozenset[str] = frozenset({".DS_Store", "Thumbs.db", "desktop.ini"})


def _normalize_relpath(value: str) -> str:
    rel = str(value).replace("\\", "/").lstrip("/")
    if not rel:
        raise ValueError("relative path must be non-empty")
    return rel


def _static_source_dir(target: str) -> Path:
    if target == "atlas_deps":
        return Path(common_dirs.src_package_dir())
    if target == "atlas_runtime_assets":
        return Path(common_dirs.atlas_runtime_assets_dir())
    if target == "atlas_test_data":
        return Path(common_dirs.atlas_test_data_dir())
    raise ValueError(f"Unsupported static target: {target}")


def _static_manifest(target: str) -> list[dict]:
    if target == "atlas_deps":
        return list(atlas_deps_files)
    if target == "atlas_runtime_assets":
        return list(atlas_runtime_assets_files)
    if target == "atlas_test_data":
        return list(atlas_test_data_files)
    raise ValueError(f"Unsupported static target: {target}")


def _validate_manifest_file(
    local_path: Path, *, expected_size: int, expected_sha256: str
) -> None:
    if not local_path.exists():
        raise FileNotFoundError(f"Expected local file does not exist: {local_path}")
    if not local_path.is_file():
        raise FileNotFoundError(
            f"Expected a file but found something else: {local_path}"
        )

    actual_size = local_path.stat().st_size
    if actual_size != expected_size:
        raise RuntimeError(
            f"Size mismatch for {local_path}: expected {expected_size}, got {actual_size}"
        )

    actual_sha256 = download_utils.calculate_checksum(str(local_path))
    if actual_sha256 != expected_sha256:
        raise RuntimeError(
            f"SHA256 mismatch for {local_path}: expected {expected_sha256}, got {actual_sha256}"
        )


def _remote_matches_expected(
    remote: atlas_r2.RemoteObjectInfo | None,
    *,
    expected_size: int,
    expected_sha256: str,
) -> bool:
    return (
        remote is not None
        and remote.size == expected_size
        and remote.sha256 == expected_sha256
    )


def _normalize_remote_dir(value: str) -> str:
    rel = _normalize_relpath(value).rstrip("/")
    parts = Path(rel).parts
    if any(part in ("", ".", "..") for part in parts):
        raise ValueError(
            f"remote directory must be a normal relative path under the R2 static prefix (got {value!r})"
        )
    return "/".join(parts)


def sync_static_target(*, target: str, dry_run: bool, allow_partial: bool) -> None:
    config = atlas_r2.get_r2_config_for_target("primary")
    client = atlas_r2.create_s3_client(config)
    source_dir = _static_source_dir(target)
    manifest = _static_manifest(target)
    prefix = atlas_r2.build_static_object_key(config, target)

    manifest_items: list[tuple[Path, str, int, str]] = []
    expected_keys: set[str] = set()
    for item in manifest:
        rel = _normalize_relpath(item["filename"])
        local_path = source_dir / Path(rel)
        object_key = atlas_r2.build_static_object_key(config, target, rel)
        expected_size = int(item["expected_size"])
        expected_sha256 = str(item["expected_sha256"])
        manifest_items.append((local_path, object_key, expected_size, expected_sha256))
        expected_keys.add(object_key)

    uploaded = 0
    unchanged = 0
    partial_skipped = 0

    deleted = atlas_r2.delete_objects_not_in_set(
        client,
        bucket=config.bucket,
        prefix=prefix + "/",
        expected_keys=expected_keys,
        dry_run=dry_run,
    )

    logger.info("Syncing static target '%s' from %s", target, source_dir)
    for local_path, object_key, expected_size, expected_sha256 in manifest_items:
        remote = atlas_r2.head_object(client, bucket=config.bucket, key=object_key)

        try:
            _validate_manifest_file(
                local_path,
                expected_size=expected_size,
                expected_sha256=expected_sha256,
            )
        except FileNotFoundError as e:
            if not allow_partial:
                raise
            partial_skipped += 1
            if _remote_matches_expected(
                remote,
                expected_size=expected_size,
                expected_sha256=expected_sha256,
            ):
                logger.info(
                    "Skipping missing local file %s. Remote object already matches the manifest and will be preserved: s3://%s/%s",
                    local_path,
                    config.bucket,
                    object_key,
                )
                continue

            if remote is not None:
                deleted += atlas_r2.delete_objects(
                    client,
                    bucket=config.bucket,
                    keys=[object_key],
                    dry_run=dry_run,
                )
                logger.warning(
                    "Skipping missing local file %s. Deleted stale remote object because it does not match the manifest: s3://%s/%s",
                    local_path,
                    config.bucket,
                    object_key,
                )
                continue

            logger.warning(
                "Skipping missing local file %s. Remote object is also missing, so this manifest entry remains unresolved in partial mode: s3://%s/%s",
                local_path,
                config.bucket,
                object_key,
            )
            continue

        changed = atlas_r2.upload_file_if_needed(
            client,
            bucket=config.bucket,
            local_path=local_path,
            object_key=object_key,
            expected_size=expected_size,
            expected_sha256=expected_sha256,
            dry_run=dry_run,
        )
        if changed:
            uploaded += 1
        else:
            unchanged += 1

    logger.info(
        "Static target '%s' complete: uploaded=%d unchanged=%d partial_skipped=%d deleted=%d",
        target,
        uploaded,
        unchanged,
        partial_skipped,
        deleted,
    )


def _iter_publishable_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        if path.name in _SKIP_FILENAMES:
            continue
        files.append(path)
    return files


def sync_folder(
    *, source_dir: Path, remote_dir: str, dry_run: bool, r2_target: str
) -> None:
    config = atlas_r2.get_r2_config_for_target(r2_target)
    client = atlas_r2.create_s3_client(config)
    source_dir = source_dir.expanduser().resolve()
    if not source_dir.exists():
        raise RuntimeError(f"Source directory does not exist: {source_dir}")
    if not source_dir.is_dir():
        raise RuntimeError(f"Source path is not a directory: {source_dir}")

    remote_dir = _normalize_remote_dir(remote_dir)
    prefix = atlas_r2.build_static_object_key(config, remote_dir)

    files = _iter_publishable_files(source_dir)
    expected_keys: set[str] = set()
    uploaded = 0
    unchanged = 0

    for local_path in files:
        rel = local_path.relative_to(source_dir).as_posix()
        object_key = atlas_r2.build_static_object_key(config, remote_dir, rel)
        expected_keys.add(object_key)

    deleted = atlas_r2.delete_objects_not_in_set(
        client,
        bucket=config.bucket,
        prefix=prefix + "/",
        expected_keys=expected_keys,
        dry_run=dry_run,
    )

    logger.info(
        "Syncing folder %s to %s R2 bucket s3://%s/%s",
        source_dir,
        config.target,
        config.bucket,
        prefix,
    )
    for local_path in files:
        rel = local_path.relative_to(source_dir).as_posix()
        object_key = atlas_r2.build_static_object_key(config, remote_dir, rel)
        checksum = download_utils.calculate_checksum(str(local_path))
        changed = atlas_r2.upload_file_if_needed(
            client,
            bucket=config.bucket,
            local_path=local_path,
            object_key=object_key,
            expected_size=local_path.stat().st_size,
            expected_sha256=checksum,
            dry_run=dry_run,
        )
        if changed:
            uploaded += 1
        else:
            unchanged += 1

    logger.info(
        "Folder sync complete for %s -> %s R2 bucket s3://%s/%s: uploaded=%d unchanged=%d deleted=%d",
        source_dir,
        config.target,
        config.bucket,
        prefix,
        uploaded,
        unchanged,
        deleted,
    )


def _publish_ifw_to_r2_target(*, os_name: str, dry_run: bool, r2_target: str) -> None:
    config = atlas_r2.get_r2_config_for_target(r2_target)
    client = atlas_r2.create_s3_client(config)
    deploy_dir = Path(common_dirs.deploy_target_dir())
    repo_dir = deploy_dir / os_name
    if not repo_dir.is_dir():
        raise RuntimeError(
            f"Expected generated IFW repository directory does not exist: {repo_dir}"
        )

    expected_package_keys: set[str] = set()
    uploaded = 0
    unchanged = 0
    files = _iter_publishable_files(repo_dir)
    # Publish Updates.xml last so clients do not observe a new IFW index before
    # all referenced package payloads and metadata are already available.
    non_updates = [path for path in files if path.name != "Updates.xml"]
    updates = [path for path in files if path.name == "Updates.xml"]

    logger.info(
        "Publishing IFW repository for %s to %s R2 bucket from %s",
        os_name,
        config.target,
        repo_dir,
    )
    for local_path in non_updates + updates:
        rel = local_path.relative_to(repo_dir).as_posix()
        checksum = download_utils.calculate_checksum(str(local_path))
        size = local_path.stat().st_size
        # Preserve the generated IFW repository tree under static/packages/<OS>/...
        object_key = atlas_r2.build_static_object_key(config, "packages", os_name, rel)
        expected_package_keys.add(object_key)
        changed = atlas_r2.upload_file_if_needed(
            client,
            bucket=config.bucket,
            local_path=local_path,
            object_key=object_key,
            expected_size=size,
            expected_sha256=checksum,
            dry_run=dry_run,
        )
        if changed:
            uploaded += 1
        else:
            unchanged += 1

    deleted = atlas_r2.delete_objects_not_in_set(
        client,
        bucket=config.bucket,
        prefix=atlas_r2.build_static_object_key(config, "packages", os_name) + "/",
        expected_keys=expected_package_keys,
        dry_run=dry_run,
    )

    installer_name = f"AtlasInstaller-{os_name}.zip"
    installer_path = deploy_dir / installer_name
    installer_object_key = atlas_r2.build_static_object_key(
        config, "installers", installer_name
    )
    if not installer_path.is_file():
        raise RuntimeError(
            f"Expected installer archive does not exist: {installer_path}"
        )
    installer_checksum = download_utils.calculate_checksum(str(installer_path))
    changed = atlas_r2.upload_file_if_needed(
        client,
        bucket=config.bucket,
        local_path=installer_path,
        object_key=installer_object_key,
        expected_size=installer_path.stat().st_size,
        expected_sha256=installer_checksum,
        dry_run=dry_run,
    )
    if changed:
        uploaded += 1
    else:
        unchanged += 1

    schema_dir = (
        Path(common_dirs.atlas_repository_dir())
        / "src"
        / "atlas"
        / "Resources"
        / "json"
        / "atlas"
    )
    for name in _IFW_SCHEMA_FILES:
        schema_path = schema_dir / name
        object_key = atlas_r2.build_static_object_key(config, "installers", name)
        if not schema_path.is_file():
            raise RuntimeError(f"Expected schema file does not exist: {schema_path}")
        checksum = download_utils.calculate_checksum(str(schema_path))
        changed = atlas_r2.upload_file_if_needed(
            client,
            bucket=config.bucket,
            local_path=schema_path,
            object_key=object_key,
            expected_size=schema_path.stat().st_size,
            expected_sha256=checksum,
            dry_run=dry_run,
        )
        if changed:
            uploaded += 1
        else:
            unchanged += 1

    logger.info(
        "IFW publish for %s to %s R2 bucket complete: uploaded=%d unchanged=%d deleted=%d",
        os_name,
        config.target,
        uploaded,
        unchanged,
        deleted,
    )


def publish_ifw(*, os_name: str, dry_run: bool) -> None:
    for r2_target in atlas_r2.target_choices():
        _publish_ifw_to_r2_target(
            os_name=os_name,
            dry_run=dry_run,
            r2_target=r2_target,
        )


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Publish Atlas static assets and IFW repositories to Cloudflare R2."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    sync_static = subparsers.add_parser(
        "sync-static", help="Sync manifest-driven developer assets to R2."
    )
    sync_static.add_argument(
        "target",
        choices=("atlas_deps", "atlas_runtime_assets", "atlas_test_data", "all"),
        help="Static asset target to sync.",
    )
    sync_static.add_argument(
        "--dry-run",
        action="store_true",
        help="Log planned uploads/deletes without changing R2.",
    )
    sync_static.add_argument(
        "--allow-partial",
        action="store_true",
        help="Warn and skip missing local files instead of aborting the sync.",
    )

    sync_folder_parser = subparsers.add_parser(
        "sync-folder",
        help="Mirror an arbitrary local folder into static/<remote-dir>/ on R2.",
    )
    sync_folder_parser.add_argument(
        "source_dir",
        help="Local directory to mirror to R2.",
    )
    sync_folder_parser.add_argument(
        "remote_dir",
        help="Remote directory name under the R2 static prefix, for example 'neutube'.",
    )
    sync_folder_parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Log planned uploads/deletes without changing R2.",
    )
    sync_folder_parser.add_argument(
        "--r2-target",
        choices=atlas_r2.target_choices(),
        default="primary",
        help="R2 bucket target to sync to. Defaults to primary.",
    )

    publish_ifw_parser = subparsers.add_parser(
        "publish-ifw",
        help="Publish generated Qt IFW repositories and installers to R2.",
    )
    publish_ifw_parser.add_argument(
        "--os",
        dest="os_name",
        required=True,
        choices=("Linux", "Windows", "macOS"),
        help="Generated deploy OS directory to publish.",
    )
    publish_ifw_parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Log planned uploads/deletes without changing R2.",
    )

    return parser


def main() -> None:
    setup_logger()
    parser = _build_parser()
    args = parser.parse_args()

    if args.command == "sync-static":
        targets = (
            ("atlas_deps", "atlas_runtime_assets", "atlas_test_data")
            if args.target == "all"
            else (args.target,)
        )
        for target in targets:
            sync_static_target(
                target=target,
                dry_run=args.dry_run,
                allow_partial=args.allow_partial,
            )
        return

    if args.command == "publish-ifw":
        publish_ifw(
            os_name=args.os_name,
            dry_run=args.dry_run,
        )
        return

    if args.command == "sync-folder":
        sync_folder(
            source_dir=Path(args.source_dir),
            remote_dir=args.remote_dir,
            dry_run=args.dry_run,
            r2_target=args.r2_target,
        )
        return

    raise RuntimeError(f"Unhandled command: {args.command}")


if __name__ == "__main__":
    main()
