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
mvn -DskipTests package
cp target/atlas-bioformats-bridge.jar ../3rdparty/build/jars/
```

The Java bridge intentionally stays on the Protobuf Java 3.25.x maintenance
line. Keep the Maven `protobuf.version` property pinned to the same patch
version for both `protobuf-java` and `protoc`; Atlas' C++ side uses its own
vendored Protobuf runtime in a separate process and only shares protobuf wire
frames with this sidecar.

The protocol exposes Bio-Formats resolution metadata and streams both region
pixels and thumbnails as chunked protobuf responses. Atlas uses full-resolution
metadata for the canonical `ZImgInfo`, then adds integer-ratio Bio-Formats
pyramid levels to the `ZImgPack` tile index when the reader reports compatible
sub-resolutions. The gRPC backend starts the bridge with `--grpc-port=<port>`;
`--worker-count=<count>` controls how many independent `IFormatReader`
instances are opened per dataset inside that single Java process.
