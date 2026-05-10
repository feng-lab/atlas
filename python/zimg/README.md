# zimg

Python bindings for Atlas’ image I/O + processing library (built with nanobind).

The main entry point is `ZImg`, a multidimensional image container designed for
fast CPU array interop, region-of-interest (ROI) access, and common microscopy /
scientific image formats.

## Highlights

- Multidimensional images: `C`, `Z`, `Y`, `X`, and `T` (time is represented as a list of arrays in Python).
- Read image metadata without loading full pixel data (`ZImg.readImgInfos`, `ZImg.readImgInfo`).
- Read full images or ROIs via `ZImgRegion` (end coordinate is **exclusive**).
- Fast CPU array interop via `ZImg.data` / `ZImg.to_arrays()` (NumPy / Torch / TensorFlow / JAX / Array API / memoryview).
- Zero-copy array wrapping when possible (CPU C-contiguous + `layout="CZYX"`), with `copy_if_needed` to enforce or relax this.
- Sub-block / tile access (`ZImg.readSubBlockLists`, `ZImg.readSubBlock`) for formats that support it.
- Streaming writers via Python-implemented providers (`ZImg.writeImg` + `ZImgSliceProvider` / `ZImgBlockProvider`).
- Save images (`ZImg.save`) with optional `ZImgWriteParameters` (compression, etc.).
- neuTube tracing / skeletonization workers exposed directly to Python:
  `ZNeutubeSkeletonize`, `ZNeutubeAutoTrace`,
  `ZNeutubeBlockedAutoTrace`, `ZSwcSubtract`, and `TraceConfig`.
- Embedded neuTube JSON presets available directly in Python via
  `zimg.neutube_json`.

## Installation

- Requires Python `>= 3.12`.
- Prebuilt wheels target the package's configured Stable ABI floor, so a wheel
  built with newer regular CPython still targets the same runtime floor.
- Requires NumPy (installed automatically by `pip install zimg`).
- If a prebuilt wheel is available for your platform: `pip install zimg`.
- If `pip` falls back to building from source, see “Building from source” below.

## Quickstart

```python
import zimg

img = zimg.ZImg("example.ome.tif")
arr0 = img.to_arrays("numpy")[0]  # t = 0
print(arr0.shape)  # (C, Z, Y, X)
print(arr0.dtype)

img.save("out.tif")
```

## neuTube processing

The package exposes neuTube processing workers directly. They can be configured
from Python with setter methods, or loaded/saved as task files via
`loadTask(...)` / `saveTask(...)`.

Available classes:

- `ZNeutubeSkeletonize`: binary image to SWC skeletonization.
- `ZNeutubeAutoTrace`: whole-volume auto tracing on a selected channel / timepoint.
- `ZNeutubeBlockedAutoTrace`: blocked auto tracing for large datasets.
- `ZSwcSubtract`: subtract one or more SWCs from an input SWC.
- `TraceConfig`: algorithm-override struct for tracing score / behavior knobs.

### `ZImgSource` input model

These tracing workers accept `ZImgSource`, not just a plain filename. This
matches Atlas' native image-source model and supports:

- single files
- file lists
- scene selection
- ROI selection
- explicit format hints

For simple single-file use, `setInputImagePath(...)` is still available.

```python
import zimg

source = zimg.ZImgSource("signal.ome.tif")
source.scene = 0
source.region = zimg.ZImgRegion((0, 0, 0, 0, 0), (-1, -1, -1, 1, 1))
```

### Embedded config presets

Current neuTube tracing / skeletonization presets are embedded directly into the
Python package as `zimg.neutube_json`.

- Parsed JSON presets are exposed as Python `dict` values such as
  `zimg.neutube_json.SKELETONIZE`,
  `zimg.neutube_json.FLYEM_SKELETONIZE`,
  `zimg.neutube_json.TRACE_CONFIG`, and
  `zimg.neutube_json.TRACE_CONFIG_BIOCYTIN`.
- JSON text is also available through `get_preset_text(...)`.
- `write_preset(...)` can materialize a preset to disk when a file is needed.

The tracing/skeletonization workers accept either:

- a config file path, via `setSkeletonizeConfigPath(...)` or `setTraceConfigPath(...)`
- an inline Python `dict`, via `setSkeletonizeConfig(...)` or `setTraceConfig(...)`

### Skeletonize a binary image to SWC

```python
import zimg

proc = zimg.ZNeutubeSkeletonize()
proc.setInputImageSource(zimg.ZImgSource("binary_mask.tif"))
proc.setSkeletonizeConfig(zimg.neutube_json.SKELETONIZE)
proc.setOutputSwcPath("binary_mask.swc")
proc.run()

print(proc.hasResult(), proc.outputSwcPath())
```

### Auto trace a signal volume

```python
import zimg

proc = zimg.ZNeutubeAutoTrace()
proc.setInputImageSource(zimg.ZImgSource("signal.ome.tif"))
proc.setSelectedChannelTime(0, 0)
proc.setTraceConfig(zimg.neutube_json.TRACE_CONFIG)
proc.setOutputSwcPath("autotrace.swc")

# Optional: override selected tracing parameters only when you already know
# which fields you want to change for your dataset.
#
# overrides = zimg.TraceConfig()
# overrides.minAutoScore = ...
# overrides.seedMethod = ...
# proc.setAlgoConfigOverrides(overrides)

proc.run()
```

### Blocked auto trace for large datasets

`ZNeutubeBlockedAutoTrace` is intended for larger datasets where tracing
should run block-by-block instead of materializing the whole volume at once.
When possible, it derives metadata such as dataset shape and z-scale directly
from the provided `ZImgSource`.

It also exposes advanced tiling controls such as block core size and halo, but
those values are workload-specific. If you do not already have tuned settings
for a dataset, start with the worker defaults instead of guessing block sizes in
Python.

```python
import zimg

proc = zimg.ZNeutubeBlockedAutoTrace()
proc.setInputImageSource(zimg.ZImgSource("large_signal.ome.zarr"))
proc.setSelectedChannelTime(0, 0)
proc.setSignalDownsampleRatio([2, 2, 1])
proc.setTraceConfig(zimg.neutube_json.TRACE_CONFIG)
proc.setOutputSwcPath("blocked_autotrace.swc")
proc.setOutputSessionDir("blocked_autotrace_session")
proc.run()
```

### Subtract SWCs

```python
import zimg

proc = zimg.ZSwcSubtract()
proc.setInputSwcFilename("full_tree.swc")
proc.setSubtractSwcFilenames(["artifact_1.swc", "artifact_2.swc"])
proc.setOutputSwcFilename("cleaned_tree.swc")
proc.run()
```

## Reading a region (ROI)

`ZImgRegion` is defined by `(x, y, z, c, t)` start/end coordinates, where `end`
is **not included**. Use `-1` for any `end` component to mean “to the end” of
that dimension.

```python
import zimg

region = zimg.ZImgRegion((0, 0, 0, 0, 0), (256, 256, 64, -1, 1))
img = zimg.ZImg("big.ome.tif", region=region)
arr0 = img.to_arrays("numpy")[0]
```

## Creating from arrays

The canonical layout is `CZYX`. Pass `layout=` if your arrays use a different
dimension order. For CPU arrays, `ZImg` will wrap zero-copy when possible:

- If `layout="CZYX"` and the input is CPU C-contiguous: typically zero-copy.
- Otherwise: it will copy unless `copy_if_needed=False` (then it raises).

```python
import numpy as np
import zimg

arr = np.zeros((1, 1, 64, 64), dtype=np.uint16)  # C, Z, Y, X
img = zimg.ZImg(arr, layout="CZYX", copy_if_needed=False)  # enforce zero-copy
img.save("zeros.tif")
```

`ZImg.to_arrays(framework="auto")` will return CPU arrays in the requested
framework. With `framework="auto"`, it mirrors the input framework if the image
was created zero-copy from arrays; otherwise it returns NumPy arrays.

## Sub-block / tiled IO

Some formats store images as sub-blocks / tiles (e.g. pyramid levels, chunked
layouts). `zimg` exposes these via:

- `ZImg.readSubBlockLists(...)` → per-scene list of NumPy int64 arrays describing sub-blocks.
  Each sub-block record contains `(t, x, y, z, width, height, depth, xRatio, yRatio, zRatio)`.
- `ZImg.readSubBlock(...)` → read an individual sub-block by `(scene, blockIndex)`.

## Streaming writes (slice/block providers)

`ZImg.writeImg(...)` can write from a *provider* instead of requiring a full
in-memory `ZImg`. Implement the provider interface in Python:

- `ZImgSliceProvider`: implement `imgInfo()`, `slice(z, t)`, `allSlices(t)`, `wholeImg()`.
- `ZImgBlockProvider`: implement `imgInfo()`, `numBlocks()`, `blockCoord(i)`, `block(i)`, `wholeImg()`.

This is useful for very large datasets or pipelines that generate data
incrementally.

## Compression parameters

`ZImgWriteParameters` exposes common compression knobs (availability depends on
the file format):

- `compression` (see `zimg.Compression`)
- `zlibCompressionLevel`
- `jpegQuality`, `jpegProgressive`, `jpegChrominanceSubsampling`, `jpegAccurateDCT`
- `jpegXRQuality`

## File formats

Supported file formats depend on how the wheel/source was built. The
`zimg.FileFormat` enum includes:

- `Tiff`, `OmeTiff`, `Png`, `Jpeg`, `JpegXR`
- `ZeissCZI`, `ZeissLsm`, `Leica`, `BioFormats`
- `Vaa3DRaw`, `HDF5Img`, `MetaImage`, `ITKImage`
- `FreeImage` (optional; omitted when built with `-DZIMG_DISABLE_FREEIMAGE=ON`)

`BioFormats` requires OME's `bioformats_package.jar`, which is not installed by
default. Configure an existing jar with
`zimg.bioformats.configure("/path/to/bioformats_package.jar")`, or call
`zimg.bioformats.download()` to fetch the pinned runtime jar and enable it for
the current Python process. If no Bio-Formats jar is configured, native formats
continue to work and Bio-Formats-backed readers report unavailable.

The wheel does not ship Java. At import time, the Python package checks
`JAVA_HOME/bin/java` first, then falls back to `java` from `PATH` if `JAVA_HOME`
is unset or unsuitable, and passes the resolved executable to the native bridge
runtime. The bridge jar is compiled for Java 11, so Bio-Formats reads require
Java 11 or newer. Missing or older Java runtimes surface as Python exceptions
from the Bio-Formats read/probe call; native formats do not start Java.

## Notes / limitations

- `ZImg.to_arrays()` and `ZImg.data` expose **CPU-backed** arrays that reference
  the `ZImg` buffers (the Python arrays keep the parent `ZImg` alive).
- GPU arrays are not supported.

## Building from source

This package reuses Atlas’ native CMake build. Building from source generally
requires the same native dependencies as Atlas (compiler toolchain, Qt, and the
Atlas third-party libraries built/configured).

For source builds, use regular CPython at or above the minimum supported
version (not free-threaded Python). The builder itself may be newer than the
wheel ABI floor, but the wheel target stays on that configured `abi3` floor.

From the repo root:

```bash
cd python/zimg
python -m pip install .
```

If you build Atlas via conda recipes, ensure the expected third-party artifacts
are present (e.g., `src/3rdparty/build/`) before building this wheel.
