import os
import shutil
import sys


HEADER = """INCLUDEPATH += $$PWD

DEFINES += CERES_USE_CXX11 CERES_NO_SUITESPARSE CERES_NO_CXSPARSE CERES_NO_THREADS CERES_STD_UNORDERED_MAP

"""

FOOT = """contains(QMAKE_CXX, g++) | contains(QMAKE_CXX, clang++) {
    OPT_CXXFLAGS = -Wno-unused-parameter -Wno-sign-compare

    opt.name = optimization
    opt.input = OPTIMIZATION_SOURCES
    opt.variable_out = OBJECTS
    opt.output = ${QMAKE_VAR_OBJECTS_DIR}${QMAKE_FILE_IN_BASE}$${first(QMAKE_EXT_OBJ)}
    opt.commands = $${QMAKE_CXX} $(CXXFLAGS) $$OPT_CXXFLAGS $(INCPATH) -c ${QMAKE_FILE_IN} -o ${QMAKE_FILE_OUT}
    QMAKE_EXTRA_COMPILERS += opt
} else {
    OPT_CXXFLAGS = /wd4100 # 'identifier' : unreferenced formal parameter

    opt.name = optimization
    opt.input = OPTIMIZATION_SOURCES
    opt.variable_out = OBJECTS
    opt.output = ${QMAKE_VAR_OBJECTS_DIR}${QMAKE_FILE_IN_BASE}$${first(QMAKE_EXT_OBJ)}
    opt.commands = $${QMAKE_CXX} $(CXXFLAGS) $$OPT_CXXFLAGS $(INCPATH) /c ${QMAKE_FILE_IN} /Fo${QMAKE_FILE_OUT}
    QMAKE_EXTRA_COMPILERS += opt
}

"""


PATCH_1 = """#ifdef _USE_QSLOG_
QDebug operator << (QDebug s, const FunctionSample& m)
{
  std::ostringstream oss;
  oss << m;
  s.nospace() << oss.str().c_str();
  return s.space();
}
#endif

"""


PATCH_2 = """#ifdef _USE_QSLOG_
// qDebug output of eigen matrix and vector
template<typename Derived>
QDebug operator << (QDebug s, const Eigen::DenseBase<Derived> & m)
{
  std::ostringstream oss;
  oss << m;
  s.nospace() << oss.str().c_str();
  return s.space();
}

template<typename ExpressionType>
QDebug operator << (QDebug s, const Eigen::WithFormat<ExpressionType> & m)
{
  std::ostringstream oss;
  oss << m;
  s.nospace() << oss.str().c_str();
  return s.space();
}
#endif

"""


def patch_some_files(out_folder):
    """
    QsLog support
    """

    file = os.path.join(out_folder, 'internal', 'line_search.cc')
    with open(file, mode='r', encoding='utf-8') as f:
        lines = f.readlines()
    with open(file, mode='w', encoding='utf-8') as f:
        for line in lines:
            if line.find('<< std::scientific << std::setprecision(kErrorMessageNumericPrecision)') >= 0:
                line = '#ifdef _USE_QSLOG_\n' + \
                       line.replace('<< std::scientific << std::setprecision(kErrorMessageNumericPrecision)',
                                    '<< qSetRealNumberPrecision(kErrorMessageNumericPrecision)') + \
                        '\n#else\n' + line + '#endif\n'
            elif line.find('VLOG(3) << std::scientific') >= 0:
                line = '#ifdef _USE_QSLOG_\n' + \
                       line.replace('VLOG(3) << std::scientific',
                                    'VLOG(3) << ""') + \
                        '\n#else\n' + line + '#endif\n'
            elif line.find('<< std::setprecision(kErrorMessageNumericPrecision)') >= 0:
                line = '#ifdef _USE_QSLOG_\n' + \
                       line.replace('<< std::setprecision(kErrorMessageNumericPrecision)',
                                    '<< qSetRealNumberPrecision(kErrorMessageNumericPrecision)') + \
                        '\n#else\n' + line + '#endif\n'
            if line.startswith("LineSearch::LineSearch(const LineSearch::Options& options)"):
                f.write(PATCH_1)
            f.write(line)

    file = os.path.join(out_folder, 'internal', 'eigen.h')
    with open(file, mode='r', encoding='utf-8') as f:
        lines = f.readlines()
    with open(file, mode='w', encoding='utf-8') as f:
        for line in lines:
            if line.startswith('#include "Eigen/Core"'):
                f.write('#ifdef _USE_QSLOG_\n#include <QDebug>\n#endif\n')
            if line.startswith("typedef Eigen::Matrix<double, Eigen::Dynamic, 1> Vector;"):
                f.write(PATCH_2)
            f.write(line)

    file = os.path.join(out_folder, 'internal', 'line_search_minimizer.cc')
    with open(file, mode='r', encoding='utf-8') as f:
        newlines = []
        for line in f.readlines():
            line = line.replace('VLOG_IF(1, is_not_silent)', 'VLOG_IF(0, is_not_silent)')
            newlines.append(line)
    with open(file, mode='w', encoding='utf-8') as f:
        for line in newlines:
            f.write(line)

    file = os.path.join(out_folder, 'internal', 'port.h')
    with open(file, mode='r', encoding='utf-8') as f:
        lines = f.readlines()
    with open(file, mode='w', encoding='utf-8') as f:
        for line in lines:
            line = line.replace('#include "internal/config.h"',
                                '//#include "internal/config.h"')
            f.write(line)

    file = os.path.join(out_folder, 'internal', 'lapack.cc')
    with open(file, mode='r', encoding='utf-8') as f:
        lines = f.readlines()
    with open(file, mode='w', encoding='utf-8') as f:
        for line in lines:
            if line.startswith('// C interface to the LAPACK'):
                f.write('#if 0\n')
            if line.startswith("namespace ceres {"):
                f.write('#endif\n')
            f.write(line)


def update_file_include_paths(file):
    """
    remove "ceres/"
    replace "glog/logging.h" with "zlog.h"
    """

    with open(file, mode='r', encoding='utf-8') as f:
        newlines = []
        for line in f.readlines():
            line = line.replace('"ceres/', '"')
            line = line.replace('"glog/logging.h"', '"zlog.h"')
            newlines.append(line)
    with open(file, mode='w', encoding='utf-8') as f:
        for line in newlines:
            f.write(line)


def patch_generated_file_include_path(file):
    """
    """

    with open(file, mode='r', encoding='utf-8') as f:
        newlines = []
        for line in f.readlines():
            line = line.replace('"schur_eliminator_impl.h"', '"internal/schur_eliminator_impl.h"')
            line = line.replace('"partitioned_matrix_view_impl.h"', '"internal/partitioned_matrix_view_impl.h"')
            newlines.append(line)
    with open(file, mode='w', encoding='utf-8') as f:
        for line in newlines:
            f.write(line)


def update_include_paths(out_folder, header_files, internal_header_files, source_files, generated_source_files):
    """
    update ceres include paths
    """

    for hf in header_files:
        update_file_include_paths(os.path.join(out_folder, hf))
    for hf in internal_header_files:
        update_file_include_paths(os.path.join(out_folder, 'internal', hf))
    for sf in source_files:
        update_file_include_paths(os.path.join(out_folder, 'internal', sf))
    for sf in generated_source_files:
        update_file_include_paths(os.path.join(out_folder, 'internal', 'generated', sf))
        patch_generated_file_include_path(os.path.join(out_folder, 'internal', 'generated', sf))


def write_project_file(out_folder, header_files, internal_header_files, source_files, generated_source_files):
    """
    generate optimization.pri file
    """

    with open(os.path.join(out_folder, 'optimization.pri'), mode='w') as f:
        f.write(HEADER)

        # header files
        f.write('HEADERS += \\\n')
        for hf in header_files:
            f.write("    $$PWD/" + hf + "\\\n")
        for hf in internal_header_files:
            f.write("    $$PWD/internal/" + hf + "\\\n")
        f.write("\n")

        # source files
        f.write('OPTIMIZATION_SOURCES = \\\n')
        for sf in source_files:
            f.write("    $$PWD/internal/" + sf + "\\\n")
        for sf in generated_source_files:
            f.write("    $$PWD/internal/generated/" + sf + "\\\n")
        f.write("\n")

        # others
        f.write(FOOT)


def copy_tree(root_src_dir, root_dst_dir):
    for src_dir, dirs, files in os.walk(root_src_dir):
        dst_dir = src_dir.replace(root_src_dir, root_dst_dir)
        if not os.path.exists(dst_dir):
            os.mkdir(dst_dir)
        for file_ in files:
            src_file = os.path.join(src_dir, file_)
            dst_file = os.path.join(dst_dir, file_)
            if os.path.exists(dst_file):
                os.remove(dst_file)
                print("warn: file '{0}' is overwritten".format(dst_file))
            shutil.copy2(src_file, dst_dir)


def copy_tree2(src, dst, symlinks=False, ignore=None):
    if not os.path.exists(dst):
        os.makedirs(dst)
    for item in os.listdir(src):
        s = os.path.join(src, item)
        d = os.path.join(dst, item)
        if os.path.isdir(s):
            copy_tree2(s, d, symlinks, ignore)
        else:
            # if not os.path.exists(d) or os.stat(src).st_mtime - os.stat(dst).st_mtime > 1:
            if os.path.exists(d):
                os.remove(d)
                print("warn: file '{0}' is overwritten".format(d))
            shutil.copy2(s, d)


def copy_tree3(src, dst, symlinks=False, ignore=None):
    for item in os.listdir(src):
        s = os.path.join(src, item)
        d = os.path.join(dst, item)
        if os.path.isdir(s):
            shutil.copytree(s, d, symlinks, ignore)
        else:
            shutil.copy2(s, d)


def make_qt_optimization_project(ceres_folder, out_folder):
    """
    rip ceres for neuTube
    """

    if not os.path.exists(ceres_folder):
        print("folder '{0}' do not exist, abort".format(ceres_folder))
        return

    if not os.path.exists(out_folder):
        os.makedirs(out_folder)
    else:
        shutil.rmtree(out_folder, ignore_errors=True)
        os.makedirs(out_folder)

    copy_tree(os.path.join(ceres_folder, 'include', 'ceres'), out_folder)
    copy_tree(os.path.join(ceres_folder, 'internal', 'ceres'), os.path.join(out_folder, 'internal'))

    header_files = []
    internal_header_files = []
    source_files = []
    generated_source_files = []

    for item in os.listdir(out_folder):
        if os.path.isfile(os.path.join(out_folder, item)) and item != "c_api.h" and (not item.startswith(".")):
            header_files.append(item)

    for item in os.listdir(os.path.join(out_folder, 'internal')):
        if os.path.isfile(os.path.join(out_folder, 'internal', item)):
            if item.endswith(".h") and (not item.endswith("_test.h")) and (not item.endswith("_test_utils.h")) and \
                    (not item.startswith("gmock")) and (not item.startswith(".")) and (item != "config.h"):
                internal_header_files.append(item)
            if item.endswith(".cc") and (not item.endswith("_test.cc")) and (not item.endswith("_test_utils.cc")) and \
                    (not item.startswith("gmock") and (item != "c_api.cc")) and (not item.startswith(".")) and \
                    (not item.startswith("collections_port")) and (not item.endswith("test_util.cc")):
                source_files.append(item)

    for item in os.listdir(os.path.join(out_folder, 'internal', 'generated')):
        if not item.startswith("."):
            generated_source_files.append(item)

    write_project_file(out_folder, header_files, internal_header_files, source_files, generated_source_files)
    update_include_paths(out_folder, header_files, internal_header_files, source_files, generated_source_files)
    patch_some_files(out_folder)


if __name__ == "__main__":
    if len(sys.argv) > 2:
        make_qt_optimization_project(sys.argv[1], sys.argv[2])
    else:
        make_qt_optimization_project('/Users/feng/code/ceres-solver', '/Users/feng/code/optimization')
