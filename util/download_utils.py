import hashlib
import logging
import os
import random
import sys
import time
import urllib.request
from functools import wraps

import requests

import common_dirs

logger = logging.getLogger(__name__)

# Progress reporting policy for non-interactive environments (e.g., CI)
PROGRESS_LOG_INTERVAL_SEC = 5   # minimum seconds between progress log lines
PROGRESS_LOG_PERCENT_STEP = 10  # log on each additional N% completion


def retry_with_backoff(retries=5, backoff_in_seconds=1):
    def decorator(func):
        @wraps(func)
        def wrapper(*args, **kwargs):
            x = 0
            while True:
                try:
                    return func(*args, **kwargs)
                except Exception:
                    if x == retries:
                        raise
                    sleep = backoff_in_seconds * 2**x + random.uniform(0, 1)
                    time.sleep(sleep)
                    x += 1
                    logger.info(f"Retrying {func.__name__}... (attempt {x + 1})")

        return wrapper

    return decorator


def get_system_proxy():
    proxy = urllib.request.getproxies()
    http_proxy = proxy.get('http')
    https_proxy = proxy.get('https')
    return http_proxy, https_proxy


def calculate_checksum(file_path):
    sha256_hash = hashlib.sha256()
    with open(file_path, "rb") as f:
        for byte_block in iter(lambda: f.read(4096), b""):
            sha256_hash.update(byte_block)
    return sha256_hash.hexdigest()


def validate_checksum(file_path, expected_sha256):
    calculated_hash = calculate_checksum(file_path)
    if calculated_hash == expected_sha256:
        logger.info(f"Checksum validation successful for {file_path}")
        return True
    else:
        logger.warning(f"Checksum validation failed for {file_path}")
        return False


def is_correct_platform(filename):
    if common_dirs.is_linux():
        return not ('win' in filename.lower() or '.exe' in filename.lower() or '.msi' in filename.lower()
                    or 'mac' in filename.lower())
    elif common_dirs.is_windows():
        return not ('linux' in filename.lower() or 'mac' in filename.lower())
    elif common_dirs.is_mac():
        return not ('linux' in filename.lower() or 'win' in filename.lower() or '.exe' in filename.lower()
                    or '.msi' in filename.lower())
    return True


@retry_with_backoff()
def download_file_with_resume(
    url,
    backup_url,
    target_path,
    expected_size=None,
    expected_sha256=None,
):
    has_expected_size = expected_size is not None and expected_size > 0
    has_expected_sha256 = bool(expected_sha256)

    # Check if file exists and has correct size
    if os.path.exists(target_path):
        current_size = os.path.getsize(target_path)
        if has_expected_size and current_size == expected_size:
            logger.info(f"File {target_path} already exists with correct size. Skipping download.")
            if has_expected_sha256:
                return validate_checksum(target_path, expected_sha256)
            return True

        if not has_expected_size:
            if current_size == 0:
                logger.info(f"File {target_path} exists but is empty. Re-downloading.")
            elif has_expected_sha256:
                logger.info(f"File {target_path} already exists. Validating checksum.")
                if validate_checksum(target_path, expected_sha256):
                    logger.info(
                        f"File {target_path} already exists with matching checksum. Skipping download."
                    )
                    return True
                logger.warning(
                    f"File {target_path} checksum mismatch. Re-downloading."
                )
                current_size = 0
            else:
                logger.info(f"File {target_path} already exists. Skipping download.")
                return True
        elif current_size > expected_size:
            logger.warning(f"File {target_path} is larger than expected. Re-downloading.")
            current_size = 0
        else:
            logger.info(f"Resuming download for {target_path}")
    else:
        current_size = 0

    http_proxy, https_proxy = get_system_proxy()
    proxies = None
    if http_proxy or https_proxy:
        proxies = {}
        if http_proxy:
            proxies["http"] = http_proxy
        if https_proxy:
            proxies["https"] = https_proxy
        logger.info("Using proxy:")
        if http_proxy:
            logger.info(f"  HTTP Proxy: {http_proxy}")
        if https_proxy:
            logger.info(f"  HTTPS Proxy: {https_proxy}")

    # Requests will consult environment/system proxy settings by default; explicitly
    # pass a "no proxy" mapping to force a direct connection when needed.
    no_proxy = {"http": None, "https": None}
    if proxies:
        proxy_options = [(proxies, "with proxy"), (no_proxy, "without proxy")]
    else:
        proxy_options = [(no_proxy, "")]

    urls = [u for u in (url, backup_url, url, backup_url) if u]
    for current_url in urls:
        for current_proxies, proxy_label in proxy_options:
            try:
                proxy_suffix = f" ({proxy_label})" if proxy_label else ""
                logger.info(f"Downloading from {current_url}{proxy_suffix}")

                # Refresh current_size from disk in case a previous attempt partially wrote the file.
                if has_expected_size and os.path.exists(target_path):
                    current_size = os.path.getsize(target_path)
                    if current_size > expected_size:
                        logger.warning(
                            f"File {target_path} is larger than expected. Re-downloading."
                        )
                        current_size = 0
                else:
                    current_size = 0

                # Set up headers
                headers = {
                    'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/58.0.3029.110 Safari/537.3',
                    'Accept': '*/*',
                    'Accept-Encoding': 'gzip, deflate',
                    'Connection': 'keep-alive'
                }

                # Try to use range if file exists and we know the expected size.
                if current_size > 0 and has_expected_size:
                    headers['Range'] = f'bytes={current_size}-'

                response = requests.get(current_url, stream=True, proxies=current_proxies, headers=headers)

                if 'Range' in headers and response.status_code != 206:
                    logger.info(
                        "Server ignored range request. Downloading entire file."
                    )
                    headers.pop('Range', None)
                    response = requests.get(current_url, stream=True, proxies=current_proxies, headers=headers)
                    current_size = 0

                # If we get a 406 or 416 error, try again without range header
                if response.status_code in [406, 416]:
                    logger.info(
                        "Range request not supported or invalid. Downloading entire file."
                    )
                    headers.pop('Range', None)
                    response = requests.get(current_url, stream=True, proxies=current_proxies, headers=headers)
                    current_size = 0  # Reset current_size as we're downloading from the beginning

                response.raise_for_status()

                start_time = time.time()
                # Determine whether we have an interactive terminal. In CI (e.g., GitHub Actions),
                # stdout is not a TTY, and carriage-return updates will spam the log. Fall back to
                # rate-limited logger updates in that case.
                interactive = hasattr(sys.stdout, "isatty") and sys.stdout.isatty()
                last_log_time = start_time
                last_logged_pct = -1  # integer percent last logged in non-interactive mode
                # Append to file if resuming, otherwise write new file
                mode = 'ab' if current_size > 0 else 'wb'
                with open(target_path, mode) as file:
                    downloaded_size = current_size
                    for chunk in response.iter_content(chunk_size=8192):
                        if chunk:
                            file.write(chunk)
                            downloaded_size += len(chunk)
                            elapsed_time = time.time() - start_time
                            # Avoid division by zero if the first chunk arrives too quickly or
                            # if the clock resolution reports a zero/negative elapsed time.
                            if elapsed_time > 0:
                                speed = downloaded_size / (1024 * 1024 * elapsed_time)  # MB/s
                            else:
                                speed = 0.0
                            progress = (downloaded_size / expected_size) * 100 if has_expected_size else None

                            if interactive:
                                # Update in-place on a single terminal line
                                sys.stdout.write('\033[K')  # clear to end of line
                                if progress is not None:
                                    sys.stdout.write(
                                        f"\rProgress: {progress:.2f}% | Speed: {speed:.2f} MB/s"
                                    )
                                else:
                                    downloaded_mb = downloaded_size / (1024 * 1024)
                                    sys.stdout.write(
                                        f"\rDownloaded: {downloaded_mb:.1f} MB | Speed: {speed:.2f} MB/s"
                                    )
                                sys.stdout.flush()
                            else:
                                # Non-interactive (CI): log infrequently to avoid flooding logs
                                now = time.time()
                                if progress is not None:
                                    pct_int = int(progress)
                                    should_log = (
                                        (now - last_log_time) >= PROGRESS_LOG_INTERVAL_SEC
                                        or pct_int >= last_logged_pct + PROGRESS_LOG_PERCENT_STEP
                                        or pct_int == 100
                                    )
                                    if should_log:
                                        logger.info(
                                            f"Progress: {progress:.1f}% | Speed: {speed:.2f} MB/s"
                                        )
                                        last_log_time = now
                                        last_logged_pct = pct_int
                                else:
                                    should_log = (now - last_log_time) >= PROGRESS_LOG_INTERVAL_SEC
                                    if should_log:
                                        downloaded_mb = downloaded_size / (1024 * 1024)
                                        logger.info(
                                            f"Downloaded: {downloaded_mb:.1f} MB | Speed: {speed:.2f} MB/s"
                                        )
                                        last_log_time = now

                if interactive:
                    # Ensure a newline after the in-place progress line
                    sys.stdout.write('\n')
                    sys.stdout.flush()
                else:
                    # Keep a blank line separation minimal in logs
                    logger.info('')

                if has_expected_size and os.path.getsize(target_path) != expected_size:
                    logger.warning(
                        "Downloaded file size does not match expected size. Trying next URL."
                    )
                    continue

                if os.path.getsize(target_path) == 0:
                    logger.warning("Downloaded empty file. Trying next URL.")
                    continue

                if not has_expected_sha256:
                    logger.info(f"File downloaded successfully: {target_path}")
                    return True

                if validate_checksum(target_path, expected_sha256):
                    logger.info(f"File downloaded successfully: {target_path}")
                    return True

                logger.warning("Checksum validation failed. Trying next URL.")
                os.remove(target_path)
            except requests.RequestException as e:
                logger.error(f"Error downloading from {current_url}{proxy_suffix}: {e}")

    logger.error("Failed to download file from all URLs.")
    return False


def sync_files(files_to_download, target_directory: str, check_os: bool = True):
    """
    Synchronize files in the target directory with the provided list of files to download.

    :param files_to_download: List of dictionaries containing file information
    :param target_directory: Directory to synchronize files to
    """
    # Create target directory if it doesn't exist
    os.makedirs(target_directory, exist_ok=True)

    # Keep track of files that should be in the directory
    # Normalize paths for comparison across OSes
    def _normalize_rel(p: str) -> str:
        # Use OS-native separators, and lower-case on Windows for case-insensitive FS
        np = os.path.normpath(p)
        return np.lower() if common_dirs.is_windows() else np

    expected_files = set()

    # Download or update files
    for file_info in files_to_download:
        if check_os and not is_correct_platform(file_info['filename']):
            continue
        # Store normalized relative path for comparison during cleanup
        rel = file_info['filename']
        target_path = os.path.join(target_directory, rel)
        expected_files.add(_normalize_rel(rel))

        # Create subdirectories if necessary
        os.makedirs(os.path.dirname(target_path), exist_ok=True)

        success = download_file_with_resume(
            file_info['url'],
            file_info['backup_url'],
            target_path,
            file_info['expected_size'],
            file_info['expected_sha256']
        )

        if not success:
            logger.critical(f"Failed to download {file_info['filename']}")

        logger.info('')

    # Remove files that are not in the download list
    for root, dirs, files in os.walk(target_directory):
        for file in files:
            file_path = os.path.join(root, file)
            relative_path = os.path.relpath(file_path, target_directory)
            if _normalize_rel(relative_path) not in expected_files:
                logger.info(f"Removing file not in download list: {relative_path}")
                os.remove(file_path)

    # Remove empty directories
    for root, dirs, files in os.walk(target_directory, topdown=False):
        for dir in dirs:
            dir_path = os.path.join(root, dir)
            if not os.listdir(dir_path):
                os.rmdir(dir_path)
                logger.info(f"Removed empty directory: {dir_path}")
