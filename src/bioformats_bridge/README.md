# Atlas Bio-Formats Bridge

This directory owns the Java sidecar used by Atlas' native `ZImgBioFormats`
reader. The sidecar speaks the protobuf protocol in `src/protos` over
length-prefixed stdin/stdout frames and compiles/runs against Atlas' packaged
`bioformats_package.jar`.

Atlas does not pin a separate Bio-Formats Maven version here. The Maven build
uses `../3rdparty/build/jars/bioformats_package.jar` as a system-scoped compile
dependency, and Atlas runtime loading uses that same jar beside
`atlas-bioformats-bridge.jar`.

Rebuild the bridge jar only when changing the bridge Java code or protobuf
protocol:

```sh
cd src/bioformats_bridge
mvn -DskipTests package
cp target/atlas-bioformats-bridge.jar ../3rdparty/build/jars/
```

The generated jar intentionally shades only `protobuf-java`; Bio-Formats stays
outside this jar so Atlas can keep using the packaged `bioformats_package.jar`
as the single Bio-Formats runtime payload.

The Java bridge intentionally stays on the Protobuf Java 3.25.x maintenance
line. Keep the Maven `protobuf.version` property pinned to the same patch
version for both `protobuf-java` and `protoc`; Atlas' C++ side uses its own
vendored Protobuf runtime in a separate process and only shares protobuf wire
frames with this sidecar.

The protocol exposes Bio-Formats resolution metadata and streams both region
pixels and thumbnails as chunked protobuf responses. Atlas uses full-resolution
metadata for the canonical `ZImgInfo`, then adds integer-ratio Bio-Formats
pyramid levels to the `ZImgPack` tile index when the reader reports compatible
sub-resolutions.

## Validation Data

Prefer deterministic Bio-Formats `FakeReader` inputs for bridge protocol,
metadata, chunking, and voxel-layout tests:

- https://bio-formats.readthedocs.io/en/stable/metadata/FakeReader.html
- https://bio-formats.readthedocs.io/en/v8.1.0/developers/generating-test-images.html

Format-breadth checks should run against an external corpus instead of vendoring
large microscopy datasets into Atlas. Useful sources are the public OME sample
image downloads and local lab files such as ND2/CZI fixtures:

- https://downloads.openmicroscopy.org/images/

To build a small local public corpus:

```sh
python util/download_bioformats_samples.py --mode sample --output-dir "$HOME/Documents/omeimages"
```

Then run the optional breadth smoke test with:

```sh
ATLAS_BIOFORMATS_BREADTH_DIR="$HOME/Documents/omeimages" ./zbioformatstest
```

For a full external-drive mirror of the public OME image tree, use `all` mode.
This mode mirrors every non-hidden file under the OME image root, including
format sidecars. Downloads are resumable: existing complete files are skipped,
partial files are continued with HTTP range requests when the server supports
them, and `manifest.json` is used as a saved download plan. The downloader
keeps a separate `full_manifest.json` inventory of the OME tree; every run
ensures that full inventory exists first. If `manifest.json` is missing, the
downloader loads the full inventory, derives a new plan from the current
selection flags, and writes `manifest.json` before downloading. Delete
`manifest.json` to derive a new plan from the cached full inventory, or delete
`full_manifest.json` to refresh the inventory from OME.

```sh
python util/download_bioformats_samples.py --mode all --output-dir "/Volumes/External/omeimages"
```

Use `--dry-run` first to estimate planned file count and total bytes. In `all`
mode, size, depth, and total-byte caps are unlimited by default; pass
`--max-total-gib`, `--max-file-mib`, or `--max-depth` to derive a smaller plan.
Use `--per-format` in `all` mode to cap the number of files selected from each
top-level format.
