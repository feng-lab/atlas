#!/bin/sh

cd $(pwd)
cd ..

conda activate pt
python util/build_ext_libs.py all
python util/build_and_deploy_atlas.py

python util/build_ext_libs.py conda-zimg
# conda-build conda-opencv-recipe
conda-build zimg-recipe
