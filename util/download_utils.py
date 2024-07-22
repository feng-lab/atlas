import os
import sys
import hashlib
import requests
import urllib.request
import random
import time
from functools import wraps
import common_dirs


def retry_with_backoff(retries=5, backoff_in_seconds=1):
    def decorator(func):
        @wraps(func)
        def wrapper(*args, **kwargs):
            x = 0
            while True:
                try:
                    return func(*args, **kwargs)
                except Exception as e:
                    if x == retries:
                        raise
                    sleep = (backoff_in_seconds * 2 ** x + random.uniform(0, 1))
                    time.sleep(sleep)
                    x += 1
                    print(f"Retrying {func.__name__}... (attempt {x + 1})")

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
        print(f"Checksum validation successful for {file_path}")
        return True
    else:
        print(f"Checksum validation failed for {file_path}")
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
def download_file_with_resume(url, backup_url, target_path, expected_size, expected_sha256):
    http_proxy, https_proxy = get_system_proxy()
    proxies = {
        'http': http_proxy,
        'https': https_proxy
    }

    # Check if file exists and has correct size
    if os.path.exists(target_path):
        current_size = os.path.getsize(target_path)
        if current_size == expected_size:
            print(f"File {target_path} already exists with correct size. Skipping download.")
            return validate_checksum(target_path, expected_sha256)
        elif current_size > expected_size:
            print(f"File {target_path} is larger than expected. Re-downloading.")
            current_size = 0
        else:
            print(f"Resuming download for {target_path}")
    else:
        current_size = 0

    urls = [url, backup_url]
    for current_url in urls:
        try:
            print(f"Downloading from {current_url}")

            # Set up headers
            headers = {
                'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/58.0.3029.110 Safari/537.3',
                'Accept': '*/*',
                'Accept-Encoding': 'gzip, deflate',
                'Connection': 'keep-alive'
            }

            # Try to use range if file exists
            if current_size > 0:
                headers['Range'] = f'bytes={current_size}-'

            response = requests.get(current_url, stream=True, proxies=proxies, headers=headers)

            # If we get a 406 or 416 error, try again without range header
            if response.status_code in [406, 416]:
                print(f"Range request not supported or invalid. Downloading entire file.")
                headers.pop('Range', None)
                response = requests.get(current_url, stream=True, proxies=proxies, headers=headers)
                current_size = 0  # Reset current_size as we're downloading from the beginning

            response.raise_for_status()

            # Append to file if resuming, otherwise write new file
            mode = 'ab' if current_size > 0 else 'wb'
            with open(target_path, mode) as file:
                for chunk in response.iter_content(chunk_size=8192):
                    file.write(chunk)
                    current_size += len(chunk)
                    # You can add progress reporting here if desired

            if os.path.getsize(target_path) != expected_size:
                print(f"Downloaded file size does not match expected size. Trying next URL.")
                continue

            if validate_checksum(target_path, expected_sha256):
                print(f"File downloaded successfully: {target_path}")
                return True
            else:
                print(f"Checksum validation failed. Trying next URL.")
        except requests.RequestException as e:
            print(f"Error downloading from {current_url}: {e}")

    print(f"Failed to download file from all URLs.")
    return False


def sync_files(files_to_download, target_directory, check_os: bool = True):
    """
    Synchronize files in the target directory with the provided list of files to download.

    :param files_to_download: List of dictionaries containing file information
    :param target_directory: Directory to synchronize files to
    """
    # Create target directory if it doesn't exist
    os.makedirs(target_directory, exist_ok=True)

    # Keep track of files that should be in the directory
    expected_files = set()

    # Download or update files
    for file_info in files_to_download:
        if check_os and not is_correct_platform(file_info['filename']):
            continue
        target_path = os.path.join(target_directory, file_info['filename'])
        expected_files.add(file_info['filename'])

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
            sys.stderr.write(f"Failed to download {file_info['filename']}")
            sys.exit(1)

        print()

    # Remove files that are not in the download list
    for root, dirs, files in os.walk(target_directory):
        for file in files:
            file_path = os.path.join(root, file)
            relative_path = os.path.relpath(file_path, target_directory)
            if relative_path not in expected_files:
                print(f"Removing file not in download list: {relative_path}")
                os.remove(file_path)

    # Remove empty directories
    for root, dirs, files in os.walk(target_directory, topdown=False):
        for dir in dirs:
            dir_path = os.path.join(root, dir)
            if not os.listdir(dir_path):
                os.rmdir(dir_path)
                print(f"Removed empty directory: {dir_path}")
