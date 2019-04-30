import sys
import os

jarsDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'jars')
print(jarsDIR)
os.environ['ZIMG_JARS_DIR'] = jarsDIR

from ._imgpy import *

if sys.version_info[0] < 3 or sys.version_info[1] < 7:
    sys.stderr.write('Error: need python 3.7 or higher\n')
    sys.exit(1)
