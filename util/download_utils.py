import os
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
def download_file_with_resume(url, backup_url, target_path, expected_size, expected_sha256, filename):
    if not is_correct_platform(filename):
        print(f"Skipping download of {filename} as it is not for the current platform.")
        return

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
            # Set the range header to resume download
            headers = {'Range': f'bytes={current_size}-'}
            response = requests.get(current_url, stream=True, proxies=proxies, headers=headers)
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
