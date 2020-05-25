import sys
import os
import numpy as np

jarsDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'jars')
os.environ['ZIMG_JARS_DIR'] = jarsDIR

from ._imgpy import *

if sys.version_info[0] < 3 or sys.version_info[1] < 8:
    sys.stderr.write('Error: need python 3.8 or higher\n')
    sys.exit(1)
