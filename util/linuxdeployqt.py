import glob
import logging
import os
import shutil
import subprocess
import tempfile

logger = logging.getLogger(__name__)

_APPIMAGE_EXCLUDELIST_URL = (
    "https://raw.githubusercontent.com/probonopd/AppImages/master/excludelist"
)

# Some system-provided libraries are tightly coupled to the host graphics stack
# (Mesa vs NVIDIA, kernel/userspace ABI, etc.) and should never be bundled into
# an AppDir/AppImage. The upstream excludelist usually covers the common SONAMEs,
# but we add a prefix-based hardening layer so major-version bumps (e.g. .so.2)
# or vendor-specific variants do not accidentally get copied.
_HARD_EXCLUDE_PREFIXES = (
    "libGL.so",
    "libOpenGL.so",
    "libEGL.so",
    "libGLESv1_CM.so",
    "libGLESv2.so",
    "libGLX.so",
    "libGLdispatch.so",
    "libglapi.so",
    # X11 / XCB
    "libX11.so",
    "libXau.so",
    "libXdmcp.so",
    "libXext.so",
    "libXrender.so",
    "libXfixes.so",
    "libXcursor.so",
    "libXi.so",
    "libXrandr.so",
    "libXinerama.so",
    "libXcomposite.so",
    "libXdamage.so",
    "libXtst.so",
    "libXss.so",
    "libSM.so",
    "libICE.so",
    "libxcb.so",
    "libxcb-",
    # Wayland
    "libwayland-",
    # Vulkan (loader, ICDs, layers)
    "libvulkan.so",
    "libvulkan_",
    "libVkLayer_",
    "libvulkan_intel.so",
    "libvulkan_radeon.so",
    "libvulkan_lvp.so",
    "libvulkan_nouveau.so",
    "libnvidia-",
)


def is_blacklisted(soname_or_path, blacklist):
    if soname_or_path in blacklist:
        return True
    base = os.path.basename(soname_or_path)
    if base in blacklist:
        return True
    for prefix in _HARD_EXCLUDE_PREFIXES:
        if soname_or_path.startswith(prefix) or base.startswith(prefix):
            return True
    return False


def load_appimage_excludelist():
    """Fetch the AppImage excludelist used to decide which system libs not to bundle.

    We rely on this list (rather than file-system location heuristics) to avoid bundling
    base system libraries (glibc, GPU driver libs, etc.) while still bundling non-base
    dependencies that live in /usr/lib on most distros (e.g. libzstd).
    """
    wget_path = which("wget")
    if wget_path:
        cp = subprocess.run(
            [wget_path, "--quiet", _APPIMAGE_EXCLUDELIST_URL, "-O", "-"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            encoding="utf-8",
            check=False,
        )
        if cp.returncode == 0 and cp.stdout:
            return cp.stdout
        logger.warning(
            "Failed to fetch AppImage excludelist with wget (rc=%s): %s",
            cp.returncode,
            (cp.stderr or "").strip(),
        )

    curl_path = which("curl")
    if curl_path:
        cp = subprocess.run(
            [curl_path, "-L", "--fail", "--silent", _APPIMAGE_EXCLUDELIST_URL],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            encoding="utf-8",
            check=False,
        )
        if cp.returncode == 0 and cp.stdout:
            return cp.stdout
        logger.warning(
            "Failed to fetch AppImage excludelist with curl (rc=%s): %s",
            cp.returncode,
            (cp.stderr or "").strip(),
        )

    raise RuntimeError(
        "Unable to fetch AppImage excludelist; ensure 'wget' or 'curl' is available and "
        "network access to raw.githubusercontent.com is working."
    )


def build_blacklist():
    """Build the blacklist (a.k.a. excludelist) used to filter ldd results."""
    blacklist = [
        "linux-vdso.so.1",
        "ld-linux-x86-64.so.2",
        "/lib64/ld-linux-x86-64.so.2",
    ]

    raw_excludelist = load_appimage_excludelist()
    excludelist_entries = 0
    for line in raw_excludelist.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        excludelist_entries += 1
        blacklist.append(line)

    blacklist = sorted(set(blacklist))
    # We intentionally ship libstdc++ and libgcc_s with the AppDir to avoid ABI drift.
    blacklist = [
        p
        for p in blacklist
        if p and not p.startswith("libstdc++.so") and not p.startswith("libgcc_s.so")
    ]
    logger.info(
        "Loaded AppImage excludelist (%d entries), blacklist size=%d",
        excludelist_entries,
        len(blacklist),
    )
    return blacklist


def prepend_search_path(env, key, dir_path):
    existing = env.get(key, "")
    prefix = dir_path
    if not existing:
        env[key] = prefix
        return
    if existing.split(os.pathsep)[0] == prefix:
        return
    env[key] = prefix + os.pathsep + existing


def merge_dicts(x, y):
    """Given two dicts, merge them into a new dict as a shallow copy."""
    z = x.copy()
    z.update(y)
    return z


def which(program):
    """Determine if argument is an executable and return the full path. Return None if none of either."""

    def is_exe(ffpath):
        return os.path.isfile(ffpath) and os.access(ffpath, os.X_OK)

    fpath, fname = os.path.split(program)
    if fpath:
        if is_exe(program):
            return program
    else:
        for path in os.environ["PATH"].split(os.pathsep):
            path = path.strip('"')
            exe_file = os.path.join(path, program)
            if is_exe(exe_file):
                return exe_file

    return None


def resolve_dependencies(executable, blacklist, env=None):
    # NOTE Use 'ldd' method for now.
    # TODO Use non-ldd method for cross-compiled apps
    return ldd(executable, blacklist, env=env)
    # objdump(executable)
    # return {}


def objdump(executable, blacklist):
    """Get all library dependencies (recursive) of 'executable' using objdump"""
    libs = {}
    return objdumpr(executable, libs, blacklist)


def objdumpr(executable, libs, blacklist):
    """Get all library dependencies (recursive) of 'executable' using objdump"""
    output = subprocess.check_output(["objdump", "-x", executable]).decode('utf-8')
    output = output.split('\n')

    accepted_columns = ['NEEDED', 'RPATH', 'RUNPATH']

    for line in output:
        split = line.split()
        if len(split) == 0:
            continue

        if split[1] not in accepted_columns:
            continue

        if split[1] in blacklist or os.path.basename(split[0]) in blacklist:
            logger.debug("'%s' is blacklisted. Skipping...", (split[0]))
            continue

        so = split[1]
        path = split[2]
        realpath = os.path.realpath(path)

        if not os.path.exists(path):
            logger.debug("Can't find path for %s (resolved to %s). Skipping...", so, path)
            continue

        if so not in libs:
            details = {'so': so, 'path': path, 'realpath': realpath, 'dependants': {executable}, 'type': 'lib'}
            libs[so] = details

            logger.debug("Resolved %s to %s", so, realpath)

            libs = merge_dicts(libs, lddr(realpath, libs, blacklist))
        else:
            libs[so]['dependants'].add(executable)

    return libs


def ldd(executable, blacklist, env=None):
    """Get all library dependencies (recursive) of 'executable' """
    libs = {}
    return lddr(executable, libs, blacklist, env=env)


def lddr(executable, libs, blacklist, env=None):
    """Get all library dependencies (recursive) of 'executable' """
    output = subprocess.check_output(["ldd", "-r", executable], env=env).decode("utf-8")
    output = output.split('\n')

    for line in output:
        if line.startswith('undefined symbol:'):
            continue

        split = line.split()
        if len(split) == 0:
            continue

        if is_blacklisted(split[0], blacklist):
            # debug("'%s' is blacklisted. Skipping..." % (split[0]))
            continue

        if split[0] == 'statically' and split[1] == 'linked':
            logger.debug("'%s' is statically linked. Skipping...", (os.path.basename(executable)))
            continue

        if len(split) < 3:
            logger.warning("Could not determine path of %s %s for ldd output line '%s'. Skipping...",
                           os.path.basename(executable), split, line)
            continue

        so = split[0]
        path = split[2]
        realpath = os.path.realpath(path)

        if not os.path.exists(path):
            logger.debug("Can't find path for %s (resolved to %s). Skipping...", so, path)
            continue

        if so not in libs:
            details = {'so': so, 'path': path, 'realpath': realpath, 'dependants': {executable}, 'type': 'lib'}
            libs[so] = details

            logger.debug("Resolved %s to %s", so, realpath)

            libs = merge_dicts(libs, lddr(realpath, libs, blacklist, env=env))
        else:
            libs[so]['dependants'].add(executable)

    return libs


def strip(f):
    cp = subprocess.run(['file', f], stdout=subprocess.PIPE, encoding='utf-8')
    if 'not stripped' in cp.stdout:
        res = subprocess.call(('strip', "-x", f))
        logger.debug("Stripping '%s'", f)
        if res > 0:
            logger.warning("'strip' command failed with return code '%s' on file '%s'", res, f)
        return res
    else:
        return 0


def patch_elf(options, f):
    arguments = ['patchelf'] + options + [f]
    res = subprocess.call(arguments)
    logger.debug("Running patchelf '%s'", arguments)
    if res > 0:
        logger.warning("'patchelf' command failed with return code '%s' on file '%s'", res, f)
    return res


def create_qt_conf(conf_path):
    # Set Plugins and imports paths
    qt_conf = "[Paths]\n"
    qt_conf += "Plugins = plugins\n"
    qt_conf += "Imports = qml\n"
    qt_conf += "Qml2Imports = qml\n"

    qc = conf_path + os.sep + "qt.conf"
    logger.debug("Writing qt.conf to '%s'", qc)
    text_file = open(qc, "w")
    text_file.write(qt_conf)
    text_file.close()


def create_desktop_file(path):
    d = "[Desktop Entry]\n"
    d += "Encoding=UTF-8\n"
    d += "Version=1.0\n"
    d += "Type=Application\n"
    d += "Name=Atlas\n"
    d += "Exec=AppRun %F\n"
    d += "Icon=Atlas.png\n"
    d += "Comment=Image Analysis\n"
    d += "Categories=Graphics;Science;Utility;\n"
    d += "Terminal=false\n"
    d += "StartupWMClass=Atlas\n"

    f = path + os.sep + "atlas.desktop"
    logger.debug("Writing desktop to '%s'", f)
    text_file = open(f, "w")
    text_file.write(d)
    text_file.close()


def build_appdir(dest_dir, executable, dependencies, qt_plugin_dir, qt_qml_dir, qt_lib_dir,
                 is_debug_version: bool = False):
    if not os.path.exists(dest_dir):
        os.makedirs(dest_dir)

    shutil.copytree(os.path.join(os.path.dirname(executable), 'lib'), os.path.join(dest_dir, 'lib'))

    appdir_libs = 'lib'
    appdir_qml = 'qml'
    appdir_plugins = 'plugins'
    # appdir_libs = 'lib'

    if not os.path.exists(dest_dir + os.sep + appdir_libs):
        os.makedirs(dest_dir + os.sep + appdir_libs)
    # if not os.path.exists(dest_dir + os.sep + appdir_plugins):
    #     os.makedirs(dest_dir + os.sep + appdir_plugins)
    if not os.path.exists(dest_dir + os.sep + appdir_qml):
        os.makedirs(dest_dir + os.sep + appdir_qml)

    dest_file = dest_dir + os.sep + os.path.basename(executable)

    # TODO overwrite option/awareness

    # Handle executable
    shutil.copyfile(executable, dest_file)  # overrides dest
    shutil.copytree(os.path.join(os.path.dirname(executable), 'Resources'), os.path.join(dest_dir, 'Resources'))
    shutil.copy2(os.path.join(os.path.dirname(executable), 'Atlas.png'), dest_dir)

    # Strip executable
    if not is_debug_version:
        strip(dest_file)
    # https://github.com/NixOS/patchelf/issues/94
    # todo: check if it is needed as we set it in cmake already
    patch_elf(['--remove-rpath'], dest_file)
    patch_elf(['--force-rpath', "--set-rpath", "$ORIGIN" + os.sep + appdir_libs], dest_file)

    for dep in dependencies:
        details = dependencies[dep]

        if details['type'] == 'lib':
            src = details['realpath']
            # Dependency filtering is performed in resolve_dependencies() via the AppImage
            # excludelist; copy every remaining DT_NEEDED entry even if it resides in
            # /usr/lib on the build machine (e.g. libzstd.so.1 on Ubuntu).
            dst = dest_dir + os.sep + appdir_libs + os.sep + dep
            logger.debug("Copying library " + dep + ": " + src + " -> " + dst)
            shutil.copyfile(src, dst)  # overrides dest no questions asked
            strip(dst)

        elif details['type'] == 'qml plugin':
            src = details['realpath']
            dst = dest_dir + os.sep + appdir_qml + os.sep + src.replace(qt_qml_dir + os.sep, '', 1)
            if not os.path.exists(os.path.dirname(dst)):
                os.makedirs(os.path.dirname(dst))
            dst_dir = os.path.dirname(dst)
            src_dir = os.path.dirname(src)

            logger.debug("Copying qml plugin dir " + dep + ": " + src_dir + ' -> ' + dst_dir)
            shutil.copytree(src_dir, dst_dir, dirs_exist_ok=True)
            strip(dst)
        else:
            src = details['realpath']
            logger.debug("Unhandled type '%s' (%s)", details['type'], src)

    for dep in glob.glob(os.path.join(qt_lib_dir, 'libQt6XcbQpa.so*')):
        dst = dest_dir + os.sep + appdir_libs
        logger.debug("Copying library " + dep + ": " + dep + ' -> ' + dst)
        shutil.copy2(dep, dst)  # overrides dest no questions asked

    for dep in glob.glob(os.path.join(qt_lib_dir, 'libQt6WaylandClient.so*')):
        dst = dest_dir + os.sep + appdir_libs
        logger.debug("Copying library " + dep + ": " + dep + ' -> ' + dst)
        shutil.copy2(dep, dst)  # overrides dest no questions asked

    for dep in glob.glob(os.path.join(qt_lib_dir, 'libQt6WaylandEglClientHwIntegration.so*')):
        dst = dest_dir + os.sep + appdir_libs
        logger.debug("Copying library " + dep + ": " + dep + ' -> ' + dst)
        shutil.copy2(dep, dst)  # overrides dest no questions asked

    # for dep in glob.glob(os.path.join(qt_lib_dir, 'libQt6Egl*')):
    #     dst = dest_dir + os.sep + appdir_libs
    #     debug("Copying library " + dep + ": " + dep + ' -> ' + dst)
    #     shutil.copy2(dep, dst)  # overrides dest no questions asked

    shutil.copytree(qt_plugin_dir, os.path.join(dest_dir, appdir_plugins))
    for dst in glob.glob(os.path.join(dest_dir, appdir_plugins, '**', '*.so.debug')):
        os.remove(dst)
    for dst in glob.glob(os.path.join(dest_dir, appdir_plugins, '**', '*.so')):
        strip(dst)

    # Make qt.conf file
    create_qt_conf(dest_dir)

    # Make default desktop file
    create_desktop_file(dest_dir)

    # Make AppRun symlink
    os.system('cd "' + dest_dir + '" && ln -s ' + os.path.basename(executable) + ' AppRun')

    # Make AppRun executable
    os.system('cd "' + dest_dir + '" && chmod +x AppRun')


def build_appimage(appdir, appimage):
    logger.debug("Building AppImage %s from %s", appimage, appdir)
    res = subprocess.call(('AppImageAssistant', appdir, appimage))
    return res


def linuxdeployqt(binary_name: str, deploy_dir: str, qt_base_dir: str, is_debug_version: bool = False):
    blacklist = build_blacklist()

    qt_qml_dir = qt_base_dir + os.sep + 'qml'
    qt_bin_dir = qt_base_dir + os.sep + 'bin'
    qt_plugin_dir = qt_base_dir + os.sep + 'plugins'
    qt_lib_dir = qt_base_dir + os.sep + 'lib'

    # temporary directory to work in
    tmp_dir = os.path.join(tempfile.gettempdir(), "linuxdeployqt.py.tmp")
    if os.path.exists(tmp_dir):
        shutil.rmtree(tmp_dir)
    os.makedirs(tmp_dir)

    logger.info('Executable: ' + binary_name)
    logger.info('Qt install directory: ' + qt_base_dir)
    logger.info('Qt QML directory: ' + qt_qml_dir)
    logger.info('Qt bin directory: ' + qt_bin_dir)
    logger.info('Qt plugin directory: ' + qt_plugin_dir)
    logger.info('Qt lib directory: ' + qt_lib_dir)

    dependencies = {}

    logger.info("Resolving shared object dependencies for '%s'", os.path.basename(binary_name))
    env = os.environ.copy()
    binary_lib_dir = os.path.join(os.path.dirname(binary_name), "lib")
    if os.path.isdir(binary_lib_dir):
        prepend_search_path(env, "LD_LIBRARY_PATH", binary_lib_dir)
    if os.path.isdir(qt_lib_dir):
        prepend_search_path(env, "LD_LIBRARY_PATH", qt_lib_dir)
    else:
        logger.warning(
            "Qt lib dir not found; dependency scanning may be incomplete (qt_lib_dir=%s)",
            qt_lib_dir,
        )
    logger.info("ldd: using LD_LIBRARY_PATH=%s", env.get("LD_LIBRARY_PATH", ""))
    exedeps = resolve_dependencies(binary_name, blacklist, env=env)

    dependencies = merge_dicts(dependencies, exedeps)

    logger.info("Building AppDir in '%s'", deploy_dir)
    build_appdir(deploy_dir, binary_name, dependencies, qt_plugin_dir, qt_qml_dir, qt_lib_dir,
                 is_debug_version=is_debug_version)


def linux_deploy_deps_to_lib_dir(binary_name: str, lib_dir: str):
    """Copy non-system shared-library dependencies of `binary_name` into `lib_dir`.

    This helper relies on `ldd -r` to resolve DT_NEEDED entries. For correct results,
    `binary_name` must be scanned in an environment where its current DT_RPATH/DT_RUNPATH
    (and any loader-related env vars such as LD_LIBRARY_PATH) can still resolve its
    dependencies.

    In particular, running this on a relocated binary can yield incomplete results because the original
    runtime search paths may no longer point at the dependency directories.
    """
    blacklist = build_blacklist()

    # temporary directory to work in
    tmp_dir = os.path.join(tempfile.gettempdir(), "linuxdeployqt.py.tmp")
    if os.path.exists(tmp_dir):
        shutil.rmtree(tmp_dir)
    os.makedirs(tmp_dir)

    logger.info('Executable: ' + binary_name)

    dependencies = {}

    logger.info("Resolving shared object dependencies for '%s'", os.path.basename(binary_name))
    exedeps = resolve_dependencies(binary_name, blacklist)

    dependencies = merge_dicts(dependencies, exedeps)

    logger.info("To Dir in '%s'", lib_dir)
    if not os.path.exists(lib_dir):
        os.makedirs(lib_dir)
    
    for dep in dependencies:
        details = dependencies[dep]

        if details['type'] == 'lib':
            src = details["realpath"]
            dst = lib_dir + os.sep + dep
            logger.debug("Copying library " + dep + ": " + src + " -> " + dst)
            shutil.copyfile(src, dst)  # overrides dest no questions asked
            strip(dst)
