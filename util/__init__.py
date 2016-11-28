import sys


if sys.version_info[0] < 3 or sys.version_info[1] < 5:
    sys.stderr.write('Error: need python 3.5 or higher\n')
    sys.exit(1)
