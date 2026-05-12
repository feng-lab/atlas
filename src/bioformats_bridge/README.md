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

Run Atlas or `zbenchmark` with `--atlas_bioformats_bridge_diagnostics=true` to
enable Java bridge diagnostics. Atlas logs the Java diagnostics file path when
the bridge starts, and Java writes `ATLAS_BIOFORMATS_DIAG` lines to that file.
The diagnostics include request timing, streamed pixel/thumbnail byte counts,
Bio-Formats plane-read counts/timing, reader-cache hit/miss/open/trim/shutdown
state, active/idle reader counts, reader-cache heap pressure state, and Java
heap snapshots. Compare the Atlas log and Java diagnostics file by
`request_id`; C++ does not read or mirror the Java file back into the Atlas log.

For local policy work, use the deterministic Bio-Formats benchmark group:

```sh
ATLAS_BIOFORMATS_BENCHMARK_FILE=/Users/feng/Documents/omeimages/Ventana/openslide/Ventana-1.bif \
  build/Release/zbenchmark --benchmark_filter=BioFormatsVentana --benchmark_repetitions=3 \
  --atlas_bioformats_bridge_diagnostics=true --v=1
```

If `ATLAS_BIOFORMATS_BENCHMARK_FILE` is unset, the benchmark uses the Ventana
path above. Region benchmarks read a fixed 4x4 grid of 1024x1024 regions from
series 0/resolution 0 and register sequential plus 1/2/4/8/16-way concurrent
gRPC reads.

The protocol exposes Bio-Formats resolution metadata and streams both region
pixels and thumbnails as chunked protobuf responses. The request API is
path-based: Atlas sends the file path and read parameters for each metadata,
region, or thumbnail request, while Java owns path normalization and internal
reader reuse. Completed readers return to a bounded idle cache that grows with
observed Java bridge concurrency up to the runtime's available processor count.
Readers are closed by Java on overflow, idle trim, shutdown, or failed requests.
Atlas uses full-resolution metadata for the canonical `ZImgInfo`, then adds integer-ratio
Bio-Formats pyramid levels to the `ZImgPack` tile index when the reader reports
compatible sub-resolutions. The gRPC backend starts the bridge with
`--grpc-port=<port>`.

Reader-cache policy is Java-owned. The normal target is performance-first:
retain enough idle readers to match observed concurrent bridge reads, bounded by
the Java runtime's available processor count. There is no absolute heap-size cap
in this policy, so machines with large JVM heaps keep the larger cache while
heap use is healthy. If used heap reaches `75%` of the JVM's effective maximum,
the idle-reader target is reduced gradually as usage approaches the critical
threshold. At `90%`, Java treats the heap as critical and closes all idle readers;
active requests are never interrupted by cache trimming. The diagnostics expose
`heap_pressure_level`, `idle_reader_target`, `trim_heap_pressure`, and
`heap_pressure_closed_readers` to make cache behavior measurable during
benchmark runs.

Independent non-RGB planes in one region request can be read concurrently inside
Java when the bridge is otherwise idle and heap pressure is normal. This uses the
same reader cache, batches at the Java runtime's available processor count, and
writes completed planes back in Atlas' requested order. Packed RGB data uses a
separate same-plane reuse path because Bio-Formats returns those channels from
one decoded image plane.
