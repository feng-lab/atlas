#!/Users/feng/miniconda3/bin/python3

import os
import shutil
import sys

import common_dirs


HEADER = """# ceres solver srcs
set(CERESSOLVER_INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}/include)
set(CERESSOLVER_INTERNAL_INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}/internal)
"""

FOOT = """source_group(CeresSolver FILES ${CERESSOLVER_HEADERS} ${CERESSOLVER_INTERNAL})

"""


def patch_some_files(out_folder: str):
    """
    QsLog support
    """

    file = os.path.join(out_folder, 'internal', 'ceres', 'lapack.cc')
    with open(file, mode='r', encoding='utf-8') as f:
        lines = f.readlines()
    with open(file, mode='w', encoding='utf-8') as f:
        for line in lines:
            if line.startswith('// C interface to the LAPACK'):
                f.write('#ifndef _MKL_LAPACK_H_\n')
            if line.startswith("namespace ceres {"):
                f.write('#endif\n')
            f.write(line)


def update_file_include_paths(file: str):
    """
    replace "glog/logging.h" with "zlog.h"
    """

    with open(file, mode='r', encoding='utf-8') as f:
        newlines = []
        for line in f.readlines():
            # line = line.replace('"glog/logging.h"', '"zlog.h"')
            newlines.append(line)
    with open(file, mode='w', encoding='utf-8') as f:
        for line in newlines:
            f.write(line)


def update_include_paths(out_folder: str, header_files: list, internal_header_files: list,
                         source_files: list, generated_source_files: list):
    """
    update ceres include paths
    """

    for hf in header_files:
        update_file_include_paths(os.path.join(out_folder, 'include', 'ceres', hf))
    for hf in internal_header_files:
        update_file_include_paths(os.path.join(out_folder, 'include', 'ceres', 'internal', hf))
    for sf in source_files:
        update_file_include_paths(os.path.join(out_folder, 'internal', 'ceres', sf))
    for sf in generated_source_files:
        update_file_include_paths(os.path.join(out_folder, 'internal', 'ceres', 'generated', sf))


def write_project_file(out_folder: str, header_files: list, internal_header_files: list,
                       source_files: list, generated_source_files: list):
    """
    generate optimization.pri file
    """

    with open(os.path.join(out_folder, 'ceres-solver.cmake'), mode='w', encoding='utf-8') as f:
        f.write(HEADER)

        # header files
        f.write('set(CERESSOLVER_HEADERS\n')
        for hf in header_files:
            f.write("    ${CMAKE_CURRENT_LIST_DIR}/include/ceres/" + hf + "\n")
        for hf in internal_header_files:
            f.write("    ${CMAKE_CURRENT_LIST_DIR}/include/ceres/internal/" + hf + "\n")
        f.write("    )\n")

        # source files
        f.write('set(CERESSOLVER_INTERNAL\n')
        for sf in source_files:
            f.write("    ${CMAKE_CURRENT_LIST_DIR}/internal/ceres/" + sf + "\n")
        for sf in generated_source_files:
            f.write("    ${CMAKE_CURRENT_LIST_DIR}/internal/ceres/generated/" + sf + "\n")
        f.write("    )\n")

        # others
        f.write(FOOT)


def make_cmake_ceres_solver_project(ceres_folder: str, out_folder: str):
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

    shutil.copytree(os.path.join(ceres_folder, 'include', 'ceres'), os.path.join(out_folder, 'include', 'ceres'))
    shutil.copytree(os.path.join(ceres_folder, 'internal', 'ceres'), os.path.join(out_folder, 'internal', 'ceres'))
    shutil.copy2(common_dirs.src_package_dir() + '/config.h', os.path.join(out_folder, 'include', 'ceres', 'internal'))

    header_files = []
    internal_header_files = []
    source_files = []
    generated_source_files = []

    for item in os.listdir(os.path.join(out_folder, 'include', 'ceres')):
        if os.path.isfile(os.path.join(out_folder, 'include', 'ceres', item)) and item != "c_api.h" and \
                (not item.startswith(".")):
            header_files.append(item)

    for item in os.listdir(os.path.join(out_folder, 'include', 'ceres', 'internal')):
        if os.path.isfile(os.path.join(out_folder, 'include', 'ceres', 'internal', item)) and \
                (not item.startswith(".")):
            internal_header_files.append(item)

    for item in os.listdir(os.path.join(out_folder, 'internal', 'ceres')):
        if os.path.isfile(os.path.join(out_folder, 'internal', 'ceres', item)):
            if (item.endswith(".cc") or item.endswith(".h")) and (not item.endswith("_test.cc")) and \
                    (not item.endswith("_test.h")) and (not item.endswith("test_util.cc")) and \
                    (not item.endswith("test_util.h")) and (not item.endswith("test_utils.cc")) and \
                    (not item.endswith("test_utils.h")) and (not item.endswith("collections_port.cc")) and \
                    (not item.startswith("gmock") and (item != "c_api.cc")) and (not item.startswith(".")):
                source_files.append(item)

    for item in os.listdir(os.path.join(out_folder, 'internal', 'ceres', 'generated')):
        if os.path.isfile(os.path.join(out_folder, 'internal', 'ceres', 'generated', item)) and \
                (not item.startswith(".")):
            generated_source_files.append(item)

    write_project_file(out_folder, header_files, internal_header_files, source_files, generated_source_files)
    update_include_paths(out_folder, header_files, internal_header_files, source_files, generated_source_files)
    patch_some_files(out_folder)


if __name__ == "__main__":
    if len(sys.argv) > 2:
        make_cmake_ceres_solver_project(sys.argv[1], sys.argv[2])
    else:
        make_cmake_ceres_solver_project(common_dirs.base_dir() + '/ceres-solver',
                                        common_dirs.ext_dir() + '/ceres-solver')
