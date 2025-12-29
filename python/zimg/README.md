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

## Installation

- Requires Python `>= 3.12`.
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
- `ZeissCZI`, `ZeissLsm`, `Leica`
- `Vaa3DRaw`, `HDF5Img`, `MetaImage`, `ITKImage`
- `FreeImage` (optional; omitted when built with `-DZIMG_DISABLE_FREEIMAGE=ON`)

## Notes / limitations

- `ZImg.to_arrays()` and `ZImg.data` expose **CPU-backed** arrays that reference
  the `ZImg` buffers (the Python arrays keep the parent `ZImg` alive).
- GPU arrays are not supported.

## Building from source

This package reuses Atlas’ native CMake build. Building from source generally
requires the same native dependencies as Atlas (compiler toolchain, Qt, and the
Atlas third-party libraries built/configured).

From the repo root:

```bash
cd python/zimg
python -m pip install .
```

If you build Atlas via conda recipes, ensure the expected third-party artifacts
are present (e.g., `src/3rdparty/build/`) before building this wheel.
