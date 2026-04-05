# Atlas libtiff overlay

Atlas vendors upstream `libtiff` as the clean submodule at `src/3rdparty/libtiff`,
but Atlas does not use libtiff's own CMake/configure path.

Instead, Atlas:

- compiles the upstream libtiff sources directly from `src/img/CMakeLists.txt`
- links the codec/dependency libraries from Atlas' build graph
- supplies hardcoded generated config headers from `src/3rdparty/libtiff_atlas/include`

The three headers in `include/` are Atlas-owned copies of the generated libtiff
configuration headers:

- `tif_config.h`
- `tiffconf.h`
- `tiffvers.h`

They intentionally live outside the submodule so the submodule stays clean and
updates remain mechanical.

## Update workflow

1. Move the `src/3rdparty/libtiff` submodule to the desired upstream tag/commit.
2. Review `src/img/CMakeLists.txt` for source-list changes if upstream added or
   removed files Atlas compiles directly.
3. Refresh the Atlas-owned generated headers. In particular, keep
   `include/tiffvers.h` aligned with the pinned submodule version unless Atlas
   intentionally wants different version metadata.
4. Rebuild Atlas and validate TIFF read/write paths.

Do not edit files inside the submodule for Atlas-specific configuration.
Atlas-specific libtiff ownership belongs in this overlay directory or in Atlas'
own build files.
