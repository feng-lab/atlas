#!/bin/sh

cd $(pwd)
cd ..

conda activate pt
python utils/build_ext_libs.py all
python utils/build_and_deploy_atlas.py

python utils/build_ext_libs.py conda-zimg
conda-build conda-opencv-recipe
conda-build zimg-recipe
