# OSX build:
# 0. (optional, if no mkl) brew install fftw --build-bottle
# 1. run "build" from 3rdparty folder
# 2. use official qt5

# Win build:
# 0. patch libtiff win32 TIFFFdOpen
# 1. run "build" from 3rdparty folder
# 2. use official qt5

TEMPLATE = subdirs

SUBDIRS = \
    img \    # sub-project names
    atlas

# sub projects folders
img.subdir = img
atlas.subdir = atlas

atlas.depends = img
