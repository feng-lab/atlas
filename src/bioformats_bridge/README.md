# Atlas Bio-Formats Bridge

This directory owns the Java bridge used by Atlas' native `ZImgBioFormats`
reader. The default desktop transport is a single-JVM gRPC service. The same
protobuf schema also supports the legacy sidecar protocol over length-prefixed
stdin/stdout frames.

Atlas does not pin a separate Bio-Formats Maven version here. The Maven build
uses `../3rdparty/build/jars/bioformats_package.jar` as a system-scoped compile
dependency.

Rebuild the bridge jar only when changing the bridge Java code or protobuf
protocol:

```sh
cd src/bioformats_bridge
mvn -DskipTests clean package
cp target/atlas-bioformats-bridge.jar ../3rdparty/build/jars/
```

For the private developer workflow that publishes a new bridge jar as an Atlas
runtime asset for future builds/CI, use:

```sh
uv run util/build_and_publish_bioformats_bridge.py
```

That helper builds the shaded jar, updates
`src/3rdparty/build/jars/atlas-bioformats-bridge.jar`, copies the same jar into
the private static `atlas_runtime_assets/jars/` deploy mirror, regenerates
`util/atlas_runtime_assets_filelist.py`, and syncs `atlas_runtime_assets` to R2.
Use `--skip-publish` to stop before the R2 sync.

The Java bridge intentionally stays on the Protobuf Java 3.25.x maintenance
line. Keep the Maven `protobuf.version` property pinned to the same patch
version for both `protobuf-java` and `protoc`; Atlas' C++ side uses its own
vendored Protobuf runtime in a separate process and only shares protobuf wire
frames with this sidecar.

The protocol exposes Bio-Formats resolution metadata and streams both region
pixels and thumbnails as chunked protobuf responses. The request API is
path-based: Atlas sends the file path and read parameters for each metadata,
region, or thumbnail request, while Java owns path normalization and internal
reader reuse. Completed readers return to a small bounded idle cache and are
closed by Java on overflow, idle trim, shutdown, or failed requests. Atlas uses
full-resolution metadata for the canonical `ZImgInfo`, then adds integer-ratio
Bio-Formats pyramid levels to the `ZImgPack` tile index when the reader reports
compatible sub-resolutions. The gRPC backend starts the bridge with
`--grpc-port=<port>`.
