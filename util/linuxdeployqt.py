import os
import sys
import subprocess
import tempfile
import shutil


def error(s):
    print("ERROR: " + s)
    exit(1)


def warn(s):
    print("WARNING: " + s)


def info(s):
    print("INFO: " + s)


def debug(s):
    print("DEBUG: " + s)


def merge_dicts(x, y):
    '''Given two dicts, merge them into a new dict as a shallow copy.'''
    z = x.copy()
    z.update(y)
    return z


def which(program):
    '''Determine if argument is an executable and return the full path. Return None if none of either.'''

    def is_exe(fpath):
        return os.path.isfile(fpath) and os.access(fpath, os.X_OK)

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
    '''Get all library dependencies (recursive) of 'executable' using objdump'''
    libs = {}
    return objdumpr(executable, libs, blacklist)


def objdumpr(executable, libs, blacklist):
    '''Get all library dependencies (recursive) of 'executable' using objdump'''
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
            debug("'%s' is blacklisted. Skipping..." % (split[0]))
            continue

        so = split[1]
        path = split[2]
        realpath = os.path.realpath(path)

        if not os.path.exists(path):
            debug("Can't find path for %s (resolved to %s). Skipping..." % (so, path))
            continue

        if so not in libs:
            details = {'so': so, 'path': path, 'realpath': realpath, 'dependants': set([executable]), 'type': 'lib'}
            libs[so] = details

            debug("Resolved %s to %s" % (so, realpath))

            libs = merge_dicts(libs, lddr(realpath, libs))
        else:
            libs[so]['dependants'].add(executable)

    return libs


def ldd(executable, blacklist):
    '''Get all library dependencies (recursive) of 'executable' '''
    libs = {}
    return lddr(executable, libs, blacklist)


def lddr(executable, libs, blacklist):
    '''Get all library dependencies (recursive) of 'executable' '''
    output = subprocess.check_output(["ldd", "-r", executable]).decode('utf-8')
    output = output.split('\n')

    for line in output:
        if line.startswith('undefined symbol:'):
            continue

        split = line.split()
        if len(split) == 0:
            continue

        if split[0] in blacklist or os.path.basename(split[0]) in blacklist:
            #debug("'%s' is blacklisted. Skipping..." % (split[0]))
            continue

        if split[0] == 'statically' and split[1] == 'linked':
            debug("'%s' is statically linked. Skipping..." % (os.path.basename(executable)))
            continue

        if len(split) < 3:
            warn("Could not determine path of %s %s for ldd output line '%s'. Skipping..." % (
            os.path.basename(executable), split, line))
            continue

        so = split[0]
        path = split[2]
        realpath = os.path.realpath(path)

        if not os.path.exists(path):
            debug("Can't find path for %s (resolved to %s). Skipping..." % (so, path))
            continue

        if so not in libs:
            details = {'so': so, 'path': path, 'realpath': realpath, 'dependants': set([executable]), 'type': 'lib'}
            libs[so] = details

            debug("Resolved %s to %s" % (so, realpath))

            libs = merge_dicts(libs, lddr(realpath, libs, blacklist))
        else:
            libs[so]['dependants'].add(executable)

    return libs


def determine_qt_plugins(deps, qt_plugin_dir):
    plugin_list = set()

    for so in deps:

        # Platform plugin
        if so.startswith("libQt5Gui"):
            debug("'%s' found" % so)
            plugin_list.add('platforms' + os.sep + 'libqxcb.so')

        # CUPS print support
        if so.startswith("libQt5PrintSupport"):
            debug("'%s' found" % so)
            plugin_list.add('printsupport' + os.sep + 'libcupsprintersupport.so')

        # SVG support
        if so.startswith("libQt5Svg"):
            debug("'%s' found" % so)
            plugin_list.add('imageformats' + os.sep + 'libqsvg.so')

        # Network support
        if so.startswith("libQt5Network"):
            debug("'%s' found" % so)
            for plugin in os.listdir(qt_plugin_dir + os.sep + 'bearer'):
                plugin_list.add('bearer' + os.sep + plugin)

        # SQL support
        if so.startswith("libQt5Sql"):
            debug("'%s' found" % so)
            for plugin in os.listdir(qt_plugin_dir + os.sep + 'sqldrivers'):
                plugin_list.add('sqldrivers' + os.sep + plugin)

        # Multimedia support
        if so.startswith("libQt5Multimedia."):
            debug("'%s' found" % so)
            for plugin in os.listdir(qt_plugin_dir + os.sep + 'mediaservice'):
                plugin_list.add('mediaservice' + os.sep + plugin)
            for plugin in os.listdir(qt_plugin_dir + os.sep + 'audio'):
                plugin_list.add('audio' + os.sep + plugin)
            for plugin in os.listdir(qt_plugin_dir + os.sep + 'playlistformats'):
                plugin_list.add('playlistformats' + os.sep + plugin)

        # Sensors support
        if so.startswith("libQt5Sensors"):
            debug("'%s' found" % so)
            for plugin in os.listdir(qt_plugin_dir + os.sep + 'sensors'):
                plugin_list.add('sensors' + os.sep + plugin)
            for plugin in os.listdir(qt_plugin_dir + os.sep + 'sensorgestures'):
                plugin_list.add('sensorgestures' + os.sep + plugin)

        # Positioning support
        if so.startswith("libQt5Positioning"):
            debug("'%s' found" % so)
            for plugin in os.listdir(qt_plugin_dir + os.sep + 'position'):
                plugin_list.add('position' + os.sep + plugin)

    for image_plugins in os.listdir(qt_plugin_dir + os.sep + 'imageformats'):
        if not image_plugins.startswith('libqsvg'):
            plugin_list.add('imageformats' + os.sep + image_plugins)

    if 'platforms' + os.sep + 'libqxcb.so' in plugin_list:
        for plugin in os.listdir(qt_plugin_dir + os.sep + 'xcbglintegrations'):
            plugin_list.add('xcbglintegrations' + os.sep + plugin)

    not_added = set()
    for root, subdirs, files in os.walk(qt_plugin_dir):
        for f in files:
            plugin = os.path.join(root, f).replace(qt_plugin_dir + os.sep, '')
            if plugin not in plugin_list:
                not_added.add(plugin)
                # print files #os.path.join(root, files)
                #
    not_added = list(not_added)
    debug("Left out these Qt plugins: %s" % not_added)

    return list(plugin_list), not_added


def strip(f):
    res = subprocess.call(('strip', "-x", f))
    debug("Stripping '%s'" % f)
    if res > 0:
        warn("'strip' command failed with return code '%s' on file '%s'" % (res, f))
    return res


def patch_elf(options, f):
    arguments = ['patchelf'] + options + [f]
    res = subprocess.call(arguments)
    debug("Running patchelf '%s'" % arguments)
    if res > 0:
        warn("'patchelf' command failed with return code '%s' on file '%s'" % (res, f))
    return res


def create_qt_conf(conf_path):
    # Set Plugins and imports paths
    qt_conf = "[Paths]\n"
    qt_conf += "Plugins = plugins\n"
    qt_conf += "Imports = qml\n"
    qt_conf += "Qml2Imports = qml\n"

    qc = conf_path + os.sep + "qt.conf"
    debug("Writing qt.conf to '%s'" % qc)
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
    debug("Writing desktop to '%s'" % f)
    text_file = open(f, "w")
    text_file.write(d)
    text_file.close()


def build_appdir(dest_dir, executable, dependencies, qt_plugin_dir, qt_plugins):
    from distutils.dir_util import copy_tree

    if not os.path.exists(dest_dir):
        os.makedirs(dest_dir)

    appdir_libs = 'lib'
    appdir_qml = 'qml'
    appdir_plugins = 'plugins'
    # appdir_libs = 'lib'

    if not os.path.exists(dest_dir + os.sep + appdir_libs):
        os.makedirs(dest_dir + os.sep + appdir_libs)
    if not os.path.exists(dest_dir + os.sep + appdir_plugins):
        os.makedirs(dest_dir + os.sep + appdir_plugins)
    if not os.path.exists(dest_dir + os.sep + appdir_qml):
        os.makedirs(dest_dir + os.sep + appdir_qml)

    dest_file = dest_dir + os.sep + os.path.basename(executable)

    # TODO overwrite option/awareness

    # Handle executable
    shutil.copyfile(executable, dest_file)  # overrides dest
    shutil.copytree(os.path.join(os.path.dirname(executable), 'Resources'), os.path.join(dest_dir, 'Resources'))
    shutil.copy2(os.path.join(os.path.dirname(executable), 'Atlas.png'), dest_dir)


    # Strip executable
    strip(dest_file)
    patch_elf(["--set-rpath", "$ORIGIN" + os.sep + appdir_libs], dest_file)

    for dep in dependencies:
        details = dependencies[dep]

        if details['type'] == 'lib':
            src = details['realpath']
            dst = dest_dir + os.sep + appdir_libs + os.sep + dep
            debug("Copying library " + dep + ": " + src + ' -> ' + dst)
            shutil.copyfile(src, dst)  # overrides dest no questions asked
            strip(dst)

        elif details['type'] == 'qml plugin':
            src = details['realpath']
            dst = dest_dir + os.sep + appdir_qml + os.sep + src.replace(qt_qml_dir + os.sep, '', 1)
            if not os.path.exists(os.path.dirname(dst)):
                os.makedirs(os.path.dirname(dst))
            dst_dir = os.path.dirname(dst)
            src_dir = os.path.dirname(src)

            debug("Copying qml plugin dir " + dep + ": " + src_dir + ' -> ' + dst_dir)
            copy_tree(src_dir, dst_dir, update=1)
            strip(dst)
        else:
            src = details['realpath']
            debug("Unhandled type '%s' (%s)" % (details['type'],))

    for qt_plugin in qt_plugins:

        src = qt_plugin_dir + os.sep + qt_plugin
        dst = dest_dir + os.sep + appdir_plugins + os.sep + qt_plugin
        if not os.path.exists(os.path.dirname(dst)):
            os.makedirs(os.path.dirname(dst))

        debug("Copying Qt plugin " + os.path.basename(qt_plugin) + ": " + src + ' -> ' + dst)
        shutil.copyfile(src, dst)  # overrides dest no questions asked
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
    debug("Building AppImage %s from %s" % (appimage, appdir))
    res = subprocess.call(('AppImageAssistant', appdir, appimage))
    return res


def linuxdeployqt(binary_name: str, deploy_dir: str, qt_base_dir: str):
    blacklist = [
        'linux-vdso.so.1',
        'ld-linux-x86-64.so.2',
        '/lib64/ld-linux-x86-64.so.2'
    ]

    update_blacklist_cmd = 'wget --quiet https://raw.githubusercontent.com/probonopd/AppImages/master/excludelist -O - | sort | uniq | grep -v "^#.*" | grep "[^-\s]"'
    blacklist += os.popen(update_blacklist_cmd).read().split('\n')
    print(blacklist)
    # exit(0)

    qt_qml_dir = qt_base_dir + os.sep + 'qml'
    qt_bin_dir = qt_base_dir + os.sep + 'bin'
    qt_plugin_dir = qt_base_dir + os.sep + 'plugins'

    # temporary directory to work in
    tmp_dir = os.path.join(tempfile.gettempdir(), "linuxdeployqt.py.tmp")
    if os.path.exists(tmp_dir):
        shutil.rmtree(tmp_dir)
    os.makedirs(tmp_dir)

    info('Executable: ' + binary_name)
    info('Qt install directory: ' + qt_base_dir)
    info('Qt QML directory: ' + qt_qml_dir)
    info('Qt bin directory: ' + qt_bin_dir)
    info('Qt plugin directory: ' + qt_plugin_dir)

    dependencies = {}

    info("Resolving shared object dependencies for '%s'" % os.path.basename(binary_name))
    exedeps = resolve_dependencies(binary_name, blacklist)

    dependencies = merge_dicts(dependencies, exedeps)

    # Determine what Qt plugins are used so far
    used_plugins, not_used_plugins = determine_qt_plugins(dependencies, qt_plugin_dir)

    # Resolve dependencies for detected Qt plugins
    info("Resolving dependencies for %s Qt plugins" % len(used_plugins))
    for plugin in used_plugins:
        qp = qt_plugin_dir + os.sep + plugin
        debug("Resolving shared object dependencies for Qt plugin '%s'" % os.path.basename(
            qt_plugin_dir + os.sep + plugin))
        qt_plugin_deps = resolve_dependencies(qp, blacklist)
        for qt_plugin_dep in qt_plugin_deps:

            qt_plugin_deps[qt_plugin_dep]['dependants'].add(qp)

            if qt_plugin_dep not in dependencies:
                debug("Adding Qt plugin shared object dependency '%s'" % qt_plugin_dep)
                dependencies[qt_plugin_dep] = qt_plugin_deps[qt_plugin_dep]

    info("Building AppDir in '%s'" % deploy_dir)
    build_appdir(deploy_dir, binary_name, dependencies, qt_plugin_dir, used_plugins)

