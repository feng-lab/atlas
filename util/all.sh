#!/bin/sh

cd $(pwd)
cd ..

conda activate pt
python util/build_ext_libs.py all
python util/build_and_deploy_atlas.py

conda-build zimg-recipe
