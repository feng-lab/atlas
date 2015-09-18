# OSX build:
# 0. (optional, if no mkl) brew install fftw --build-bottle
# 1. run "build" from 3rdparty folder
# 1.5. for xcode 7, patch itkSignedMaurerDistanceMapImageFilter.hxx, change three Math::NotAlmostEquals(x,y) to x!=y
# 2. use official qt5

# Win build:
# 0. patch libtiff win32 TIFFFdOpen
# 1. run "build" from 3rdparty folder
# 2. use official qt5

TEMPLATE = subdirs

SUBDIRS += \
    img \
    atlas
