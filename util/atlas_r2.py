import logging
import mimetypes
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Final, Iterable, Optional

import atlas_env
import download_utils

try:
    from boto3.exceptions import S3UploadFailedError
    import boto3
    from botocore.config import Config as BotoConfig
    from botocore.exceptions import BotoCoreError, ClientError
except ImportError as e:
    raise RuntimeError(
        "boto3 is required for R2 publishing. Install it in your Python environment, "
        "for example: python -m pip install boto3"
    ) from e

logger = logging.getLogger(__name__)

_R2_TARGET_PRIMARY: Final[str] = "primary"
_R2_TARGET_SECONDARY: Final[str] = "secondary"
_R2_TARGETS: Final[frozenset[str]] = frozenset(
    {_R2_TARGET_PRIMARY, _R2_TARGET_SECONDARY}
)

_BUCKET_ENV_VAR_BASE: Final[str] = "ATLAS_R2_BUCKET"
_ENDPOINT_ENV_VAR_BASE: Final[str] = "ATLAS_R2_S3_ENDPOINT"
_ACCESS_KEY_ID_ENV_VAR_BASE: Final[str] = "ATLAS_R2_ACCESS_KEY_ID"
_SECRET_ACCESS_KEY_ENV_VAR_BASE: Final[str] = "ATLAS_R2_SECRET_ACCESS_KEY"
_REGION_ENV_VAR_BASE: Final[str] = "ATLAS_R2_REGION"
_STATIC_PREFIX_ENV_VAR_BASE: Final[str] = "ATLAS_R2_STATIC_PREFIX"

_DEFAULT_REGION: Final[str] = "auto"
_DEFAULT_STATIC_PREFIX: Final[str] = "static"
_DEFAULT_CONNECT_TIMEOUT_SECONDS: Final[int] = 15
_DEFAULT_READ_TIMEOUT_SECONDS: Final[int] = 120
_R2_OPERATION_RETRIES: Final[int] = 5
_R2_BACKOFF_IN_SECONDS: Final[int] = 2
_R2_MAX_BACKOFF_SECONDS: Final[int] = 30
_R2_UPLOAD_VERIFY_RETRIES: Final[int] = 4
_R2_UPLOAD_VERIFY_BACKOFF_IN_SECONDS: Final[int] = 1
_R2_UPLOAD_VERIFY_MAX_BACKOFF_SECONDS: Final[int] = 8
_NOT_FOUND_ERROR_CODES: Final[frozenset[str]] = frozenset(
    {"404", "NoSuchKey", "NotFound"}
)
_RETRYABLE_ERROR_CODES: Final[frozenset[str]] = frozenset(
    {
        "429",
        "500",
        "502",
        "503",
        "504",
        "BandwidthLimitExceeded",
        "ConnectionAbortedError",
        "ConnectionClosedError",
        "ConnectionError",
        "InternalError",
        "RequestTimeout",
        "RequestTimeoutException",
        "ServiceUnavailable",
        "SlowDown",
        "Throttling",
        "TooManyRequests",
    }
)
_RETRYABLE_HTTP_STATUS_CODES: Final[frozenset[int]] = frozenset(
    {429, 500, 502, 503, 504}
)
_BYTE_PRESERVED_CONTENT_TYPE_BY_SUFFIX: Final[tuple[tuple[str, str], ...]] = (
    (".tar.gz", "application/gzip"),
    (".tgz", "application/gzip"),
    (".gz", "application/gzip"),
    (".tar.bz2", "application/x-bzip2"),
    (".tbz2", "application/x-bzip2"),
    (".tbz", "application/x-bzip2"),
    (".bz2", "application/x-bzip2"),
    (".tar.xz", "application/x-xz"),
    (".txz", "application/x-xz"),
    (".xz", "application/x-xz"),
    (".tar.zst", "application/zstd"),
    (".tzst", "application/zstd"),
    (".zst", "application/zstd"),
)


def _normalize_target(target: str) -> str:
    target_norm = str(target).strip().lower()
    if target_norm not in _R2_TARGETS:
        choices = ", ".join(sorted(_R2_TARGETS))
        raise ValueError(
            f"Unsupported R2 target {target!r}. Expected one of: {choices}"
        )
    return target_norm


def _target_env_var(base_name: str, *, target: str) -> str:
    return f"{base_name}_{_normalize_target(target).upper()}"


_DOTENV_KEYS: Final[frozenset[str]] = frozenset(
    {_target_env_var(_BUCKET_ENV_VAR_BASE, target=target) for target in _R2_TARGETS}
    | {_target_env_var(_ENDPOINT_ENV_VAR_BASE, target=target) for target in _R2_TARGETS}
    | {
        _target_env_var(_ACCESS_KEY_ID_ENV_VAR_BASE, target=target)
        for target in _R2_TARGETS
    }
    | {
        _target_env_var(_SECRET_ACCESS_KEY_ENV_VAR_BASE, target=target)
        for target in _R2_TARGETS
    }
    | {_target_env_var(_REGION_ENV_VAR_BASE, target=target) for target in _R2_TARGETS}
    | {
        _target_env_var(_STATIC_PREFIX_ENV_VAR_BASE, target=target)
        for target in _R2_TARGETS
    }
)


@dataclass(frozen=True)
class R2Config:
    target: str
    bucket: str
    endpoint_url: str
    access_key_id: str
    secret_access_key: str
    region: str
    static_prefix: str


@dataclass(frozen=True)
class RemoteObjectInfo:
    key: str
    size: int
    sha256: Optional[str]
    content_type: Optional[str] = None
    content_encoding: Optional[str] = None


class UploadVerificationError(RuntimeError):
    pass


def dotenv_keys() -> frozenset[str]:
    return _DOTENV_KEYS


def load_r2_env_from_dotenv() -> None:
    atlas_env.load_repo_dotenv(allowed_keys=_DOTENV_KEYS)


def get_r2_config() -> R2Config:
    return get_r2_config_for_target(_R2_TARGET_PRIMARY)


def get_r2_config_for_target(target: str) -> R2Config:
    target_norm = _normalize_target(target)
    load_r2_env_from_dotenv()
    return R2Config(
        target=target_norm,
        bucket=_required_env(_target_env_var(_BUCKET_ENV_VAR_BASE, target=target_norm)),
        endpoint_url=_normalize_endpoint(
            _required_env(_target_env_var(_ENDPOINT_ENV_VAR_BASE, target=target_norm)),
            env_var=_target_env_var(_ENDPOINT_ENV_VAR_BASE, target=target_norm),
        ),
        access_key_id=_required_env(
            _target_env_var(_ACCESS_KEY_ID_ENV_VAR_BASE, target=target_norm)
        ),
        secret_access_key=_required_env(
            _target_env_var(_SECRET_ACCESS_KEY_ENV_VAR_BASE, target=target_norm)
        ),
        region=os.environ.get(
            _target_env_var(_REGION_ENV_VAR_BASE, target=target_norm), _DEFAULT_REGION
        ).strip()
        or _DEFAULT_REGION,
        static_prefix=_normalize_path_prefix(
            os.environ.get(
                _target_env_var(_STATIC_PREFIX_ENV_VAR_BASE, target=target_norm),
                _DEFAULT_STATIC_PREFIX,
            ),
            env_var=_target_env_var(_STATIC_PREFIX_ENV_VAR_BASE, target=target_norm),
        ),
    )


def create_s3_client(config: R2Config):
    return boto3.client(
        "s3",
        endpoint_url=config.endpoint_url,
        aws_access_key_id=config.access_key_id,
        aws_secret_access_key=config.secret_access_key,
        region_name=config.region,
        config=BotoConfig(
            signature_version="s3v4",
            connect_timeout=_DEFAULT_CONNECT_TIMEOUT_SECONDS,
            read_timeout=_DEFAULT_READ_TIMEOUT_SECONDS,
            retries={"max_attempts": 10, "mode": "standard"},
        ),
    )


def build_static_object_key(config: R2Config, *parts: str) -> str:
    normalized_parts = [config.static_prefix]
    for part in parts:
        part_norm = str(part).replace("\\", "/").strip("/")
        if not part_norm:
            continue
        normalized_parts.append(part_norm)
    return "/".join(normalized_parts)


def list_object_keys(client, *, bucket: str, prefix: str) -> list[str]:
    def _list_keys() -> list[str]:
        paginator = client.get_paginator("list_objects_v2")
        keys: list[str] = []
        for page in paginator.paginate(Bucket=bucket, Prefix=prefix):
            for item in page.get("Contents", []):
                key = item.get("Key")
                if key:
                    keys.append(str(key))
        return keys

    return _call_with_retry(
        f"list R2 objects under s3://{bucket}/{prefix}",
        _list_keys,
    )


def head_object(client, *, bucket: str, key: str) -> Optional[RemoteObjectInfo]:
    missing = object()

    def _head():
        try:
            return client.head_object(Bucket=bucket, Key=key)
        except ClientError as e:
            error_code = _client_error_code(e)
            if error_code in _NOT_FOUND_ERROR_CODES:
                return missing
            raise

    response = _call_with_retry(f"head R2 object s3://{bucket}/{key}", _head)
    if response is missing:
        return None

    metadata = response.get("Metadata") or {}
    sha256 = metadata.get("sha256")
    size = _response_content_length(response)
    if size is None:
        size = _lookup_object_size_via_list(client, bucket=bucket, key=key)
    return RemoteObjectInfo(
        key=key,
        size=size,
        sha256=sha256 if sha256 else None,
        content_type=_response_header_value(response, "ContentType", "content-type"),
        content_encoding=_response_header_value(
            response, "ContentEncoding", "content-encoding"
        ),
    )


def upload_file_if_needed(
    client,
    *,
    bucket: str,
    local_path: Path,
    object_key: str,
    expected_size: int,
    expected_sha256: str,
    dry_run: bool = False,
) -> bool:
    extra_args = _upload_extra_args(local_path, expected_sha256=expected_sha256)
    remote = head_object(client, bucket=bucket, key=object_key)
    if (
        remote is not None
        and remote.size == expected_size
        and remote.sha256 == expected_sha256
    ):
        if _remote_upload_headers_match(remote, extra_args):
            logger.info("Skipping unchanged object: %s", object_key)
            return False
        if dry_run:
            logger.info("Would update object metadata: s3://%s/%s", bucket, object_key)
            return True

        logger.info(
            "Object bytes unchanged but upload metadata differs: %s", object_key
        )
        _replace_object_upload_metadata(
            client,
            bucket=bucket,
            object_key=object_key,
            extra_args=extra_args,
        )
        _verify_uploaded_object(
            client,
            bucket=bucket,
            object_key=object_key,
            expected_size=expected_size,
            expected_sha256=expected_sha256,
            expected_content_type=extra_args.get("ContentType"),
            expected_content_encoding=extra_args.get("ContentEncoding"),
        )
        return True

    if dry_run:
        logger.info("Would upload: %s -> s3://%s/%s", local_path, bucket, object_key)
        return True

    logger.info("Uploading: %s -> s3://%s/%s", local_path, bucket, object_key)
    _call_with_retry(
        f"upload {local_path} to s3://{bucket}/{object_key}",
        lambda: client.upload_file(
            str(local_path), bucket, object_key, ExtraArgs=extra_args
        ),
    )
    _verify_uploaded_object(
        client,
        bucket=bucket,
        object_key=object_key,
        expected_size=expected_size,
        expected_sha256=expected_sha256,
        expected_content_type=extra_args.get("ContentType"),
        expected_content_encoding=extra_args.get("ContentEncoding"),
    )
    return True


def _upload_extra_args(local_path: Path, *, expected_sha256: str) -> dict:
    extra_args = {"Metadata": {"sha256": expected_sha256}}
    content_type = _guess_upload_content_type(local_path)
    if content_type:
        extra_args["ContentType"] = content_type
    return extra_args


def _guess_upload_content_type(local_path: Path) -> Optional[str]:
    # The mimetypes module reports .tar.gz as ("application/x-tar", "gzip").
    # In this repository those suffixes are archive bytes, not HTTP transfer
    # encodings, so publish them without Content-Encoding.
    path = local_path.as_posix()
    path_lower = path.lower()
    for suffix, content_type in _BYTE_PRESERVED_CONTENT_TYPE_BY_SUFFIX:
        if path_lower.endswith(suffix):
            return content_type

    content_type, _content_encoding = mimetypes.guess_type(path)
    return content_type


def _remote_upload_headers_match(remote: RemoteObjectInfo, extra_args: dict) -> bool:
    expected_content_type = extra_args.get("ContentType")
    if expected_content_type and remote.content_type != expected_content_type:
        return False

    expected_content_encoding = extra_args.get("ContentEncoding")
    return remote.content_encoding == expected_content_encoding


def _replace_object_upload_metadata(
    client,
    *,
    bucket: str,
    object_key: str,
    extra_args: dict,
) -> None:
    logger.info("Updating object metadata: s3://%s/%s", bucket, object_key)
    _call_with_retry(
        f"update metadata for s3://{bucket}/{object_key}",
        lambda: client.copy_object(
            Bucket=bucket,
            Key=object_key,
            CopySource={"Bucket": bucket, "Key": object_key},
            MetadataDirective="REPLACE",
            **extra_args,
        ),
    )


def delete_objects(
    client, *, bucket: str, keys: Iterable[str], dry_run: bool = False
) -> int:
    key_list = sorted({key for key in keys if key})
    if not key_list:
        return 0

    if dry_run:
        for key in key_list:
            logger.info("Would delete: s3://%s/%s", bucket, key)
        return len(key_list)

    deleted = 0
    for start in range(0, len(key_list), 1000):
        batch = key_list[start : start + 1000]
        logger.info("Deleting %d object(s) from s3://%s", len(batch), bucket)
        response = _call_with_retry(
            f"delete {len(batch)} R2 object(s) from s3://{bucket}",
            lambda batch=batch: client.delete_objects(
                Bucket=bucket,
                Delete={"Objects": [{"Key": key} for key in batch], "Quiet": True},
            ),
        )
        errors = response.get("Errors", [])
        if errors:
            raise RuntimeError(
                "R2 delete_objects reported per-key failures for "
                f"s3://{bucket}: {_format_delete_errors(errors)}"
            )
        deleted += len(batch)
    return deleted


def delete_objects_not_in_set(
    client,
    *,
    bucket: str,
    prefix: str,
    expected_keys: Iterable[str],
    dry_run: bool = False,
) -> int:
    expected = {key for key in expected_keys if key}
    to_delete = [
        key
        for key in list_object_keys(client, bucket=bucket, prefix=prefix)
        if key not in expected
    ]
    return delete_objects(client, bucket=bucket, keys=to_delete, dry_run=dry_run)


def normalize_target(target: str) -> str:
    return _normalize_target(target)


def target_choices() -> tuple[str, ...]:
    return (_R2_TARGET_PRIMARY, _R2_TARGET_SECONDARY)


def _required_env(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        raise RuntimeError(
            f"{name} is not set. Define it in the environment or in the repo-root `.env.local`."
        )
    return value


def _normalize_endpoint(value: str, *, env_var: str) -> str:
    raw = str(value).strip().rstrip("/")
    if not raw.startswith("https://"):
        raise RuntimeError(f"{env_var} must start with https:// (got {value!r})")
    return raw


def _normalize_path_prefix(value: str, *, env_var: str) -> str:
    raw = str(value).replace("\\", "/").strip().strip("/")
    if not raw:
        raise RuntimeError(f"{env_var} must be non-empty")
    return raw


def _response_content_length(response: dict) -> Optional[int]:
    content_length = response.get("ContentLength")
    if content_length is not None:
        return int(content_length)

    metadata = response.get("ResponseMetadata") or {}
    headers = metadata.get("HTTPHeaders") or {}
    header_content_length = headers.get("content-length")
    if header_content_length is not None:
        return int(header_content_length)
    return None


def _response_header_value(
    response: dict, response_key: str, http_header_key: str
) -> Optional[str]:
    value = response.get(response_key)
    if value is None:
        metadata = response.get("ResponseMetadata") or {}
        headers = metadata.get("HTTPHeaders") or {}
        value = headers.get(http_header_key)
    if value is None:
        return None
    value = str(value).strip()
    return value if value else None


def _format_delete_errors(errors: Iterable[dict]) -> str:
    formatted: list[str] = []
    for error in errors:
        key = error.get("Key", "<unknown key>")
        code = error.get("Code", "<unknown code>")
        message = error.get("Message", "<no message>")
        formatted.append(f"{key} [{code}]: {message}")
    return "; ".join(formatted)


def _lookup_object_size_via_list(client, *, bucket: str, key: str) -> int:
    logger.debug(
        "HeadObject for s3://%s/%s did not include ContentLength; falling back to ListObjectsV2 for size resolution.",
        bucket,
        key,
    )

    response = _call_with_retry(
        f"list R2 object metadata for s3://{bucket}/{key}",
        lambda: client.list_objects_v2(Bucket=bucket, Prefix=key, MaxKeys=2),
    )
    matches = [item for item in response.get("Contents", []) if item.get("Key") == key]
    if len(matches) == 1:
        size = matches[0].get("Size")
        if size is None:
            break_glass = response.get("Contents", [])
            raise RuntimeError(
                f"ListObjectsV2 found s3://{bucket}/{key} but did not return Size. Response contents: {break_glass!r}"
            )
        return int(size)

    response_keys = [item.get("Key") for item in response.get("Contents", [])]
    raise RuntimeError(
        "HeadObject response did not include ContentLength and fallback size lookup failed for "
        f"s3://{bucket}/{key}. Matching keys returned by ListObjectsV2: {response_keys!r}"
    )


def _verify_uploaded_object(
    client,
    *,
    bucket: str,
    object_key: str,
    expected_size: int,
    expected_sha256: str,
    expected_content_type: Optional[str] = None,
    expected_content_encoding: Optional[str] = None,
) -> None:
    def _verify_once() -> None:
        remote = head_object(client, bucket=bucket, key=object_key)
        if remote is None:
            raise UploadVerificationError(
                f"uploaded object is not visible yet: s3://{bucket}/{object_key}"
            )
        if remote.size != expected_size:
            raise UploadVerificationError(
                f"uploaded object size mismatch for s3://{bucket}/{object_key}: expected {expected_size}, got {remote.size}"
            )
        if remote.sha256 != expected_sha256:
            raise UploadVerificationError(
                "uploaded object sha256 metadata mismatch for "
                f"s3://{bucket}/{object_key}: expected {expected_sha256}, got {remote.sha256!r}"
            )
        if expected_content_type and remote.content_type != expected_content_type:
            raise UploadVerificationError(
                "uploaded object content-type mismatch for "
                f"s3://{bucket}/{object_key}: expected {expected_content_type!r}, got {remote.content_type!r}"
            )
        if remote.content_encoding != expected_content_encoding:
            raise UploadVerificationError(
                "uploaded object content-encoding mismatch for "
                f"s3://{bucket}/{object_key}: expected {expected_content_encoding!r}, got {remote.content_encoding!r}"
            )

    download_utils.call_with_backoff(
        _verify_once,
        retries=_R2_UPLOAD_VERIFY_RETRIES,
        backoff_in_seconds=_R2_UPLOAD_VERIFY_BACKOFF_IN_SECONDS,
        max_backoff_seconds=_R2_UPLOAD_VERIFY_MAX_BACKOFF_SECONDS,
        retry_exceptions=(
            UploadVerificationError,
            BotoCoreError,
            ClientError,
            S3UploadFailedError,
            ConnectionError,
            TimeoutError,
        ),
        should_retry=_is_retryable_post_upload_verification_exception,
        operation_name=f"verify uploaded object s3://{bucket}/{object_key}",
    )
    logger.info("Verified uploaded object: s3://%s/%s", bucket, object_key)


def _call_with_retry(operation_name: str, func):
    return download_utils.call_with_backoff(
        func,
        retries=_R2_OPERATION_RETRIES,
        backoff_in_seconds=_R2_BACKOFF_IN_SECONDS,
        max_backoff_seconds=_R2_MAX_BACKOFF_SECONDS,
        retry_exceptions=(
            BotoCoreError,
            ClientError,
            S3UploadFailedError,
            ConnectionError,
            TimeoutError,
        ),
        should_retry=_is_retryable_r2_exception,
        operation_name=operation_name,
    )


def _client_error_code(error: ClientError) -> str:
    return str(
        error.response.get("Error", {}).get("Code", "") if error.response else ""
    ).strip()


def _client_http_status(error: ClientError) -> Optional[int]:
    if not error.response:
        return None
    metadata = error.response.get("ResponseMetadata", {})
    status = metadata.get("HTTPStatusCode")
    return int(status) if status is not None else None


def _is_retryable_r2_exception(error: BaseException) -> bool:
    if isinstance(error, ClientError):
        error_code = _client_error_code(error)
        if error_code in _NOT_FOUND_ERROR_CODES:
            return False
        if error_code in _RETRYABLE_ERROR_CODES:
            return True
        status = _client_http_status(error)
        return status in _RETRYABLE_HTTP_STATUS_CODES
    return isinstance(
        error,
        (BotoCoreError, S3UploadFailedError, ConnectionError, TimeoutError),
    )


def _is_retryable_post_upload_verification_exception(error: BaseException) -> bool:
    if isinstance(error, UploadVerificationError):
        return True
    return _is_retryable_r2_exception(error)
