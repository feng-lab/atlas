# Atlas Bio-Formats Bridge

This directory owns the Java bridge used by Atlas' native `ZImgBioFormats`
reader. The bridge builds two transport jars from one shared core:

- `atlas-bioformats-bridge-grpc.jar` is the default desktop jar. It runs one JVM
  with a loopback gRPC service and can open multiple Java-side `IFormatReader`
  workers inside that process.
- `atlas-bioformats-bridge-stdio.jar` uses the length-prefixed stdin/stdout protobuf 
  protocol and always runs one Java process.

Both transports compile and run against Atlas' packaged
`bioformats_package.jar`.

Atlas does not pin a separate Bio-Formats Maven version here. The Maven build
uses `../3rdparty/build/jars/bioformats_package.jar` as a system-scoped compile
dependency, and Atlas runtime loading uses that same jar beside the selected
transport jar.

Rebuild the bridge jars only when changing the bridge Java code or protobuf
protocol:

```sh
cd src/bioformats_bridge
mvn -DskipTests package
cp grpc/target/atlas-bioformats-bridge-grpc.jar ../3rdparty/build/jars/
cp stdio/target/atlas-bioformats-bridge-stdio.jar ../3rdparty/build/jars/
```

The generated transport jars intentionally shade their Java-side protocol
dependencies but not Bio-Formats itself. The stdio jar shades only the shared
bridge core plus protobuf runtime, while the gRPC jar also shades gRPC.

The Java bridge intentionally stays on the Protobuf Java 3.25.x maintenance
line. Keep the Maven `protobuf.version` property pinned to the same patch
version for both `protobuf-java` and `protoc`; Atlas' C++ side uses its own
vendored Protobuf runtime in a separate process and only shares protobuf wire
frames with this sidecar.

Keep all Java gRPC runtime artifacts and `protoc-gen-grpc-java` on the same
Maven `grpc.version` property so generated stubs and runtime APIs stay aligned.

The protocol exposes Bio-Formats resolution metadata and streams both region
pixels and thumbnails as chunked protobuf responses. Atlas uses full-resolution
metadata for the canonical `ZImgInfo`, then adds integer-ratio Bio-Formats
pyramid levels to the `ZImgPack` tile index when the reader reports compatible
sub-resolutions.
