import json
import logging
import os
from ftplib import FTP

import common_dirs
import download_utils
from logger import setup_logger

logger = logging.getLogger(__name__)


@download_utils.retry_with_backoff()
def ftp_store(ftp, cmd, file):
    ftp.storbinary(cmd, file)


@download_utils.retry_with_backoff()
def ftp_delete(ftp, filename):
    ftp.delete(filename)


def sync_via_ftp(local_folder, hostname, username, password, remote_folder):
    logger.info(f'{username}@{hostname}')
    ftp = FTP(hostname)
    ftp.login(user=username, passwd=password)

    try:
        ftp.cwd(remote_folder)
    except:
        ftp.mkd(remote_folder)

    remote_files = {}
    ftp.retrlines('LIST', lambda x: remote_files.update({x.split()[-1]: int(x.split()[4])}))

    for root, dirs, files in os.walk(local_folder):
        for filename in files:
            local_path = os.path.join(root, filename)
            relative_path = os.path.relpath(local_path, local_folder)
            remote_path = os.path.join(remote_folder, relative_path).replace('\\', '/')

            remote_dir = os.path.dirname(remote_path)
            try:
                ftp.cwd(remote_dir)
            except:
                ftp.mkd(remote_dir)

            local_size = os.path.getsize(local_path)
            if filename in remote_files:
                if local_size == remote_files[filename]:
                    logger.info(f"Skipping {filename} (already exists with correct size on FTP server)")
                    remote_files.pop(filename)
                    continue
                remote_files.pop(filename)

            with open(local_path, 'rb') as file:
                ftp_store(ftp, f'STOR {os.path.basename(remote_path)}', file)
            logger.info(f"Uploaded {filename} to FTP server")

    for old_file in remote_files:
        ftp_delete(ftp, old_file)
        logger.info(f"Removed old file {old_file} from FTP server")

    ftp.quit()


def process_files(folder_path, base_url, backup_base_url):
    files_info = []

    for root, dirs, files in os.walk(folder_path):
        for filename in files:
            if filename.startswith('.'):
                continue
            local_path = os.path.join(root, filename)
            relative_path = os.path.relpath(local_path, folder_path)

            logger.info(f"Processing {relative_path}...")

            checksum = download_utils.calculate_checksum(local_path)
            size = os.path.getsize(local_path)

            url = f"{base_url}/{relative_path.replace(os.sep, '/')}"
            backup_url = f"{backup_base_url}/{relative_path.replace(os.sep, '/')}"

            files_info.append({
                'filename': relative_path,
                'url': url,
                'backup_url': backup_url,
                'size': size,
                'checksum': checksum
            })

    return files_info


def generate_atlas_deps_filelist(files_info, output_file):
    """Write a Python file defining files_to_download using double-quoted strings.

    The emitted structure is valid Python (also valid JSON) with only strings and numbers,
    ensuring consistent quoting across platforms/tools.
    """
    out = []
    for fi in files_info:
        out.append(
            {
                "url": fi["url"],
                "backup_url": fi["backup_url"],
                "expected_size": fi["size"],
                "expected_sha256": fi["checksum"],
                "filename": fi["filename"],
            }
        )
    with open(output_file, "w", newline="\n") as f:
        f.write("files_to_download = ")
        json.dump(out, f, indent=4)
        f.write("\n")


if __name__ == "__main__":
    logger = setup_logger()

    ftp_info = {
        'hostname': os.getenv('FTP_HOSTNAME'),
        'username': os.getenv('FTP_USERNAME'),
        'password': os.getenv('FTP_PASSWORD'),
        'remote_folder': "/public_html/static/atlas_deps",
    }

    base_url = "https://neutracing.com/static/atlas_deps"
    backup_base_url = "https://fenglab.xyz/static/atlas_deps"
    folder_path = common_dirs.static_src_package_dir()

    files_info = process_files(folder_path, base_url, backup_base_url)
    generate_atlas_deps_filelist(files_info, os.path.join(common_dirs.atlas_util_dir(), 'atlas_deps_filelist.py'))
