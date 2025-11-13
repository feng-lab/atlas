import glob
import logging
import os
import shutil
import subprocess
import tempfile

logger = logging.getLogger(__name__)


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


def resolve_dependencies(executable, blacklist):
    # NOTE Use 'ldd' method for now.
    # TODO Use non-ldd method for cross-compiled apps
    return ldd(executable, blacklist)
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


def ldd(executable, blacklist):
    """Get all library dependencies (recursive) of 'executable' """
    libs = {}
    return lddr(executable, libs, blacklist)


def lddr(executable, libs, blacklist):
    """Get all library dependencies (recursive) of 'executable' """
    output = subprocess.check_output(["ldd", "-r", executable]).decode('utf-8')
    output = output.split('\n')

    for line in output:
        if line.startswith('undefined symbol:'):
            continue

        split = line.split()
        if len(split) == 0:
            continue

        if split[0] in blacklist or os.path.basename(split[0]) in blacklist:
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

            libs = merge_dicts(libs, lddr(realpath, libs, blacklist))
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
            if details['so'].startswith('libstdc++.so') or details['so'].startswith('libgcc_s.so') or \
                    details['so'].startswith('libatomic.so') or \
                    (not src.startswith('/usr/lib/') and not src.startswith('/lib/')):
                dst = dest_dir + os.sep + appdir_libs + os.sep + dep
                logger.debug("Copying library " + dep + ": " + src + ' -> ' + dst)
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
    blacklist = [
        'linux-vdso.so.1',
        'ld-linux-x86-64.so.2',
        '/lib64/ld-linux-x86-64.so.2'
    ]

    update_blacklist_cmd = r'wget --quiet https://raw.githubusercontent.com/probonopd/AppImages/master/excludelist' \
                           r' -O - | sort | uniq | grep -v "^#.*" | grep "[^-\s]"'
    blacklist += os.popen(update_blacklist_cmd).read().split('\n')
    logger.info(blacklist)
    blacklist = [p for p in blacklist if not p.startswith("libstdc++.so") and not p.startswith("libgcc_s.so")]
    logger.info(blacklist)
    # exit(0)

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
    exedeps = resolve_dependencies(binary_name, blacklist)

    dependencies = merge_dicts(dependencies, exedeps)

    logger.info("Building AppDir in '%s'", deploy_dir)
    build_appdir(deploy_dir, binary_name, dependencies, qt_plugin_dir, qt_qml_dir, qt_lib_dir,
                 is_debug_version=is_debug_version)


def linux_deploy_deps_to_lib_dir(binary_name: str, lib_dir: str):
    blacklist = [
        'linux-vdso.so.1',
        'ld-linux-x86-64.so.2',
        '/lib64/ld-linux-x86-64.so.2'
    ]

    update_blacklist_cmd = r'wget --quiet https://raw.githubusercontent.com/probonopd/AppImages/master/excludelist' \
                           r' -O - | sort | uniq | grep -v "^#.*" | grep "[^-\s]"'
    blacklist += os.popen(update_blacklist_cmd).read().split('\n')
    logger.info(blacklist)
    blacklist = [p for p in blacklist if not p.startswith("libstdc++.so") and not p.startswith("libgcc_s.so")]
    logger.info(blacklist)
    # exit(0)

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
            src = details['realpath']
            if details["so"].startswith("libatomic.so") or (
                not src.startswith("/usr/lib/") and not src.startswith("/lib/")
            ):
                dst = lib_dir + os.sep + dep
                logger.debug("Copying library " + dep + ": " + src + ' -> ' + dst)
                shutil.copyfile(src, dst)  # overrides dest no questions asked
                strip(dst)