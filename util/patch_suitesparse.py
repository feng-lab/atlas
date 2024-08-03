import difflib
import re
import logging
import os
import glob

import common_dirs
from logger import setup_logger

logger = logging.getLogger(__name__)


def get_bak_file_name(orig_file: str):
    return orig_file + '.bak'


def process_CXSparse_line(line: str, last_line: str):
    if last_line == '#else\n':
        return line
    line_pattern = re.compile(
        r"^(?P<prefix>.+) (?P<fun_name>(cs_malloc|cs_calloc|cs_realloc))(?P<first_para>.*)(?P<sizeof>, *sizeof *\()(?P<type_name>.*)(?P<after_type>\).*\).*)$")
    match_result = line_pattern.match(line)
    if match_result is not None:
        return '#ifdef _MSC_VER\n' \
               f"{match_result.group('prefix')} ({match_result.group('type_name')}*){match_result.group('fun_name')}" \
               f"{match_result.group('first_para')}{match_result.group('sizeof')}{match_result.group('type_name')}" \
               f"{match_result.group('after_type')}\n" \
               '#else\n' \
               f'{line}' \
               '#endif\n'
    else:
        return line


def patch_file_line(orig_file: str, process_line, keep_bak_file: bool = True) -> str:
    bak_file = get_bak_file_name(orig_file)
    if os.path.exists(bak_file):
        os.remove(bak_file)
    os.rename(orig_file, bak_file)
    with open(bak_file, mode='r', encoding='utf-8') as f:
        from_lines = f.readlines()
    with open(orig_file, mode='w', encoding='utf-8') as f:
        to_lines = []
        last_line = ''
        for line in from_lines:
            nline = process_line(line, last_line)
            last_line = line
            f.write(nline)
            to_lines.append(nline)
    logger.info(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file, tofile='<new>'))))
    if not keep_bak_file:
        os.remove(bak_file)
    return bak_file


if __name__ == "__main__":
    logger = setup_logger()

    suitesparse_path = os.path.join(common_dirs.atlas_repository_dir(), '..', 'SuiteSparse')
    for file in glob.glob(os.path.join(suitesparse_path, 'CXSparse', 'Source', '*.c')):
        patch_file_line(file, process_CXSparse_line, keep_bak_file=False)
