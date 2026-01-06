#!/bin/bash

#mkdir build
#cd build
#
#BUILD_CONFIG=Release
#
#IFS=""
## now we can start configuring
#cmake ../python -G "Ninja" \
#    -Wno-dev \
#    -DCMAKE_BUILD_TYPE=$BUILD_CONFIG \
#    -DCMAKE_INSTALL_PREFIX:PATH="${SP_DIR}" \
#    -DPYTHON_EXECUTABLE:FILEPATH="${PREFIX}/bin/python"
#
## compile & install!
#ninja install

cp -r zimg ${SP_DIR}
if [ -d zimg.libs ]; then
  cp -r zimg.libs ${SP_DIR}
fi
