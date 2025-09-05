#!/bin/bash

# This script compiles Vulkan shaders into SPIR-V
# Make sure to have glslangValidator installed

# Create output directory
mkdir -p spv

# Vertex shaders
glslangValidator -V line.vert -o spv/line.vert.spv
glslangValidator -V wideline.vert -o spv/wideline.vert.spv
glslangValidator -V wideline1.vert -o spv/wideline1.vert.spv

# Fragment shaders
glslangValidator -V line_func.frag -o spv/line_func.frag.spv
glslangValidator -V wideline_func.frag -o spv/wideline_func.frag.spv
glslangValidator -V wideline_func1.frag -o spv/wideline_func1.frag.spv

# Geometry shader
glslangValidator -V wideline.geom -o spv/wideline.geom.spv

echo "Shader compilation complete!" 