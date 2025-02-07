import sys
import os

# Get the current directory
current_dir = os.path.dirname(os.path.abspath(__file__))
os.environ['Resources_DIR'] = current_dir
os.environ['ZIMG_JARS_DIR'] = os.path.join(current_dir, 'jars')

from ._imgpy import *

if sys.version_info[0] < 3 or sys.version_info[1] < 12:
    sys.stderr.write('Error: need python 3.12 or higher\n')
    sys.exit(1)
