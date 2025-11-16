import sys
import os

# Enforce supported Python versions early and fail with an import error
if sys.version_info < (3, 12):
    raise ImportError("zimg requires Python >= 3.12")

# Get the current directory and configure resources
current_dir = os.path.dirname(os.path.abspath(__file__))
os.environ['Resources_DIR'] = current_dir
os.environ['ZIMG_JARS_DIR'] = os.path.join(current_dir, 'jars')

from ._imgpy import *
