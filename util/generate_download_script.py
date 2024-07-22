import os
import hashlib
import paramiko
from paramiko import SSHClient
from ftplib import FTP
import requests
import socket
import socks
from download_utils import *


def create_proxy_aware_ssh_client(proxy_host, proxy_port):
    class ProxySock(socket.socket):
        def __init__(self, *args, **kwargs):
            super(ProxySock, self).__init__(*args, **kwargs)
            self.proxy_host = proxy_host
            self.proxy_port = proxy_port

        def connect(self, hostport, **kwargs):
            self.settimeout(30)
            socks.set_default_proxy(socks.HTTP, self.proxy_host, self.proxy_port)
            socket.socket = socks.socksocket
            super(ProxySock, self).connect((self.proxy_host, self.proxy_port))

    return ProxySock


@retry_with_backoff()
def ssh_put(sftp, local_path, remote_path):
    sftp.put(local_path, remote_path)


@retry_with_backoff()
def ssh_remove(sftp, path):
    sftp.remove(path)


def sync_via_ssh(local_folder, remote_folder, hostname, username):
    http_proxy, https_proxy = get_system_proxy()
    ssh = SSHClient()
    ssh.load_system_host_keys()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    try:
        if http_proxy:
            proxy_host, proxy_port = http_proxy.split('://')[1].split(':')
            proxy_port = int(proxy_port)
            proxy_sock = create_proxy_aware_ssh_client(proxy_host, proxy_port)
            ssh.connect(hostname, username=username, sock=proxy_sock())
        else:
            ssh.connect(hostname, username=username)

        sftp = ssh.open_sftp()

        remote_files = {}
        try:
            for filename in sftp.listdir(remote_folder):
                remote_path = os.path.join(remote_folder, filename)
                remote_files[filename] = sftp.stat(remote_path).st_size
        except IOError:
            sftp.mkdir(remote_folder)

        for root, dirs, files in os.walk(local_folder):
            for filename in files:
                local_path = os.path.join(root, filename)
                relative_path = os.path.relpath(local_path, local_folder)
                remote_path = os.path.join(remote_folder, relative_path)

                remote_dir = os.path.dirname(remote_path)
                try:
                    sftp.stat(remote_dir)
                except IOError:
                    sftp.mkdir(remote_dir)

                local_size = os.path.getsize(local_path)
                if filename in remote_files:
                    if local_size == remote_files[filename]:
                        print(f"Skipping {filename} (already exists with correct size on SSH server)")
                        remote_files.pop(filename)
                        continue
                    remote_files.pop(filename)

                ssh_put(sftp, local_path, remote_path)
                print(f"Uploaded {filename} to SSH server")

        for old_file, _ in remote_files.items():
            old_path = os.path.join(remote_folder, old_file)
            ssh_remove(sftp, old_path)
            print(f"Removed old file {old_file} from SSH server")

    except Exception as e:
        print(f"Error during SSH sync: {str(e)}")
    finally:
        sftp.close()
        ssh.close()


@retry_with_backoff()
def ftp_store(ftp, cmd, file):
    ftp.storbinary(cmd, file)


@retry_with_backoff()
def ftp_delete(ftp, filename):
    ftp.delete(filename)


def sync_via_ftp(local_folder, remote_folder, hostname, username, password):
    http_proxy, https_proxy = get_system_proxy()
    if http_proxy:
        proxy_host, proxy_port = http_proxy.split('://')[1].split(':')
        proxy_port = int(proxy_port)
        ftp = FTP(hostname, proxy_host=proxy_host, proxy_port=proxy_port)
    else:
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
                    print(f"Skipping {filename} (already exists with correct size on FTP server)")
                    remote_files.pop(filename)
                    continue
                remote_files.pop(filename)

            with open(local_path, 'rb') as file:
                ftp_store(ftp, f'STOR {os.path.basename(remote_path)}', file)
            print(f"Uploaded {filename} to FTP server")

    for old_file in remote_files:
        ftp_delete(ftp, old_file)
        print(f"Removed old file {old_file} from FTP server")

    ftp.quit()


def process_files(folder_path, ssh_info, ftp_info, base_url, backup_base_url):
    files_info = []

    ssh_remote_folder = "/path/on/ssh/server"
    ftp_remote_folder = "/path/on/ftp/server"
    sync_via_ssh(folder_path, ssh_remote_folder, **ssh_info)
    sync_via_ftp(folder_path, ftp_remote_folder, **ftp_info)

    for root, dirs, files in os.walk(folder_path):
        for filename in files:
            local_path = os.path.join(root, filename)
            relative_path = os.path.relpath(local_path, folder_path)

            print(f"Processing {relative_path}...")

            checksum = calculate_checksum(local_path)
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


def generate_download_script(files_info, output_file):
    with open(output_file, 'w') as f:
        f.write("import os\n")
        f.write("from download_utils import download_file_with_resume\n\n")
        f.write("files_to_download = [\n")
        for file_info in files_info:
            f.write(f"    {{\n")
            f.write(f"        'url': '{file_info['url']}',\n")
            f.write(f"        'backup_url': '{file_info['backup_url']}',\n")
            f.write(f"        'target_path': os.path.join('downloads', '{file_info['filename']}'),\n")
            f.write(f"        'expected_size': {file_info['size']},\n")
            f.write(f"        'expected_sha256': '{file_info['checksum']}'\n")
            f.write(f"    }},\n")
        f.write("]\n\n")
        f.write("for file_info in files_to_download:\n")
        f.write("    os.makedirs(os.path.dirname(file_info['target_path']), exist_ok=True)\n")
        f.write("    success = download_file_with_resume(**file_info)\n")
        f.write("    print(f\"Download of {file_info['url']} {'succeeded' if success else 'failed'}\")\n")


if __name__ == "__main__":
    # Example usage
    folder_path = "/path/to/your/files"
    ssh_info = {
        'hostname': 'ssh.example.com',
        'username': 'your_username'
    }
    ftp_info = {
        'hostname': 'ftp.example.com',
        'username': 'your_username',
        'password': 'your_password'
    }
    base_url = "https://example.com/files"
    backup_base_url = "https://backup.example.com/files"

    files_info = process_files(folder_path, ssh_info, ftp_info, base_url, backup_base_url)
    generate_download_script(files_info, 'download_script.py')
