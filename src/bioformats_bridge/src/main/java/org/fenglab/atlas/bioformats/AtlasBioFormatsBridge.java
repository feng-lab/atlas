package org.fenglab.atlas.bioformats;

import com.google.protobuf.ByteString;
import io.grpc.Server;
import io.grpc.netty.shaded.io.grpc.netty.NettyServerBuilder;
import io.grpc.stub.StreamObserver;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeGrpc;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.DatasetInfoResponse;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.FileGroupingPolicy;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.ListFormatsResponse;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.MetadataEntry;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.PixelChunk;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.ProbeResponse;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.ReaderFormat;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.Request;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.Response;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.ResolutionInfo;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.RuntimeInfoResponse;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.SeriesInfo;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.ShutdownResponse;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.StatusCode;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.ThumbnailChunk;
import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.File;
import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Hashtable;
import java.util.Locale;
import java.util.Map;
import java.util.Map.Entry;
import java.util.Objects;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import loci.formats.FormatException;
import loci.formats.FormatTools;
import loci.formats.IFormatReader;
import loci.formats.ImageReader;
import loci.formats.MetadataTools;
import loci.formats.in.SDTReader;
import loci.formats.meta.IMetadata;
import loci.formats.meta.MetadataStore;
import ome.units.UNITS;
import ome.units.quantity.Length;
import ome.xml.model.primitives.Color;

public final class AtlasBioFormatsBridge
{
  private static final int MAX_FRAME_BYTES = 512 * 1024 * 1024;
  private static final int PIXEL_CHUNK_BYTES = 8 * 1024 * 1024;
  private static final int GRPC_PIXEL_CHUNK_BYTES = PIXEL_CHUNK_BYTES;
  private static final int PROTOCOL_VERSION = 1;
  private static final int JAVA_AVAILABLE_PROCESSORS = Math.max(1, Runtime.getRuntime().availableProcessors());
  private static final int READER_CACHE_MIN_IDLE_READERS = 4;
  private static final int READER_CACHE_MAX_IDLE_READERS =
    Math.max(READER_CACHE_MIN_IDLE_READERS, JAVA_AVAILABLE_PROCESSORS);
  private static final long READER_CACHE_IDLE_MILLIS = 5 * 60 * 1000L;
  private static final int READER_CACHE_HEAP_PRESSURE_USED_PERCENT = 75;
  private static final int READER_CACHE_HEAP_CRITICAL_USED_PERCENT = 90;

  private final InputStream in;
  private final OutputStream out;
  private final ReaderCache readerCache;
  private final ExecutorService planeReadExecutor;
  private final BridgeDiagnostics diagnostics;

  private AtlasBioFormatsBridge(final InputStream in, final OutputStream out, final BridgeDiagnostics diagnostics)
  {
    this.in = in == null ? null : new BufferedInputStream(in);
    this.out = out == null ? null : new BufferedOutputStream(out);
    this.diagnostics = diagnostics;
    this.readerCache = new ReaderCache(diagnostics);
    this.planeReadExecutor = Executors.newFixedThreadPool(JAVA_AVAILABLE_PROCESSORS, new ThreadFactory() {
      private final AtomicLong threadId = new AtomicLong(1);

      @Override public Thread newThread(final Runnable runnable)
      {
        final Thread thread = new Thread(runnable, "atlas-bioformats-plane-read-" + threadId.getAndIncrement());
        thread.setDaemon(true);
        return thread;
      }
    });
  }

  private AtlasBioFormatsBridge(final InputStream in, final OutputStream out)
  {
    this(in, out, BridgeDiagnostics.disabled());
  }

  private AtlasBioFormatsBridge(final BridgeDiagnostics diagnostics)
  {
    this(null, null, diagnostics);
  }

  public static void main(final String[] args) throws Exception
  {
    final OutputStream protocolOut = new FileOutputStream(FileDescriptor.out);
    System.setProperty("org.slf4j.simpleLogger.logFile", "System.err");
    System.setProperty("org.slf4j.simpleLogger.defaultLogLevel", "warn");
    System.setOut(System.err);

    int grpcPort = -1;
    boolean verbose = false;
    String diagnosticsFile = "";
    for (final String arg : args) {
      if (arg.startsWith("--grpc-port=")) {
        grpcPort = parseNonNegativeIntArgument(arg, "--grpc-port=");
      } else if (arg.equals("--verbose")) {
        verbose = true;
      } else if (arg.startsWith("--diagnostics-file=")) {
        diagnosticsFile = arg.substring("--diagnostics-file=".length());
      } else if (!arg.isEmpty()) {
        throw new IllegalArgumentException("unknown argument: " + arg);
      }
    }
    final BridgeDiagnostics diagnostics = new BridgeDiagnostics(verbose, diagnosticsFile);
    diagnostics.log("event=startup mode=" + (grpcPort >= 0 ? "grpc" : "stdio") + " diagnostics_file=" +
                    BridgeDiagnostics.quote(diagnosticsFile) + " java_pid=" + ProcessHandle.current().pid());

    if (grpcPort >= 0) {
      runGrpcServer(grpcPort, protocolOut, diagnostics);
      return;
    }

    final AtlasBioFormatsBridge bridge = new AtlasBioFormatsBridge(System.in, protocolOut, diagnostics);
    bridge.run();
  }

  private static int parseNonNegativeIntArgument(final String arg, final String prefix)
  {
    final int value = Integer.parseInt(arg.substring(prefix.length()));
    if (value < 0) {
      throw new IllegalArgumentException(prefix + " must be >= 0");
    }
    return value;
  }

  private static void
  runGrpcServer(final int grpcPort, final OutputStream startupOut, final BridgeDiagnostics diagnostics) throws Exception
  {
    final AtlasBioFormatsBridge bridge = new AtlasBioFormatsBridge(diagnostics);
    final AtomicReference<Server> serverRef = new AtomicReference<>();
    final Runnable shutdown = () ->
    {
      final Server server = serverRef.get();
      if (server != null) {
        server.shutdown();
      }
    };
    final InetSocketAddress listenAddress = new InetSocketAddress(InetAddress.getByName("127.0.0.1"), grpcPort);
    final Server server = NettyServerBuilder.forAddress(listenAddress)
                            .maxInboundMessageSize(MAX_FRAME_BYTES)
                            .addService(new BioFormatsGrpcService(bridge, shutdown))
                            .build()
                            .start();
    serverRef.set(server);
    startupOut.write(("ATLAS_BIOFORMATS_GRPC_PORT=" + server.getPort() + "\n").getBytes(StandardCharsets.US_ASCII));
    startupOut.flush();
    try {
      server.awaitTermination();
    }
    finally {
      bridge.closeCachedReaders();
      diagnostics.close();
    }
  }

  private interface ResponseSink
  {
    void send(Response response) throws Exception;

    default int pixelChunkBytes()
    {
      return PIXEL_CHUNK_BYTES;
    }

    default boolean recordsImagePlaneReads()
    {
      return false;
    }

    default void recordImagePlaneRead(final long bytes, final long elapsedNanos) {}
  }

  private static final class DiagnosticResponseSink implements ResponseSink
  {
    private final ResponseSink delegate;
    private long responseCount = 0;
    private long errorResponses = 0;
    private long pixelChunks = 0;
    private long pixelBytes = 0;
    private long thumbnailChunks = 0;
    private long thumbnailBytes = 0;
    private long imagePlaneReads = 0;
    private long imagePlaneBytes = 0;
    private long imagePlaneReadNanos = 0;
    private long maxImagePlaneReadNanos = 0;

    DiagnosticResponseSink(final ResponseSink delegate)
    {
      this.delegate = delegate;
    }

    @Override public void send(final Response response) throws Exception
    {
      ++responseCount;
      if (response.getStatus() != StatusCode.STATUS_CODE_OK) {
        ++errorResponses;
      }
      if (response.hasPixelChunk()) {
        ++pixelChunks;
        pixelBytes += response.getPixelChunk().getData().size();
      }
      if (response.hasThumbnailChunk()) {
        ++thumbnailChunks;
        thumbnailBytes += response.getThumbnailChunk().getData().size();
      }
      delegate.send(response);
    }

    @Override public int pixelChunkBytes()
    {
      return delegate.pixelChunkBytes();
    }

    @Override public boolean recordsImagePlaneReads()
    {
      return true;
    }

    @Override public synchronized void recordImagePlaneRead(final long bytes, final long elapsedNanos)
    {
      ++imagePlaneReads;
      imagePlaneBytes += bytes;
      imagePlaneReadNanos += elapsedNanos;
      maxImagePlaneReadNanos = Math.max(maxImagePlaneReadNanos, elapsedNanos);
    }

    long responseCount()
    {
      return responseCount;
    }

    long errorResponses()
    {
      return errorResponses;
    }

    long pixelChunks()
    {
      return pixelChunks;
    }

    long pixelBytes()
    {
      return pixelBytes;
    }

    long thumbnailChunks()
    {
      return thumbnailChunks;
    }

    long thumbnailBytes()
    {
      return thumbnailBytes;
    }

    synchronized long imagePlaneReads() {
      return imagePlaneReads;
    }

    synchronized long imagePlaneBytes() {
      return imagePlaneBytes;
    }

    synchronized long imagePlaneReadNanos() {
      return imagePlaneReadNanos;
    }

    synchronized long maxImagePlaneReadNanos() {
      return maxImagePlaneReadNanos;
    }
  }

  private static final class BridgeDiagnostics
  {
    private static final BridgeDiagnostics DISABLED = new BridgeDiagnostics(false);

    private final boolean enabled;
    private final PrintStream output;
    private final boolean closeOutput;
    private final AtomicLong sequence = new AtomicLong(1);

    BridgeDiagnostics(final boolean enabled)
    {
      this(enabled, System.err, false);
    }

    BridgeDiagnostics(final boolean enabled, final String diagnosticsFile) throws FileNotFoundException
    {
      this(enabled,
           createOutput(enabled, diagnosticsFile),
           enabled && diagnosticsFile != null && !diagnosticsFile.isEmpty());
    }

    private BridgeDiagnostics(final boolean enabled, final PrintStream output, final boolean closeOutput)
    {
      this.enabled = enabled;
      this.output = output;
      this.closeOutput = closeOutput;
    }

    static BridgeDiagnostics disabled()
    {
      return DISABLED;
    }

    boolean isEnabled()
    {
      return enabled;
    }

    void logRequestFinished(final Request request,
                            final String status,
                            final long elapsedNanos,
                            final DiagnosticResponseSink sink,
                            final Throwable failure)
    {
      if (!enabled) {
        return;
      }

      final StringBuilder builder = new StringBuilder();
      builder.append("event=request_end status=")
        .append(status)
        .append(" elapsed_ms=")
        .append(formatMillis(elapsedNanos))
        .append(' ')
        .append(requestDetail(request));
      if (sink != null) {
        builder.append(" responses=")
          .append(sink.responseCount())
          .append(" error_responses=")
          .append(sink.errorResponses())
          .append(" pixel_chunks=")
          .append(sink.pixelChunks())
          .append(" pixel_bytes=")
          .append(sink.pixelBytes())
          .append(" thumbnail_chunks=")
          .append(sink.thumbnailChunks())
          .append(" thumbnail_bytes=")
          .append(sink.thumbnailBytes())
          .append(" image_plane_reads=")
          .append(sink.imagePlaneReads())
          .append(" image_plane_bytes=")
          .append(sink.imagePlaneBytes())
          .append(" image_plane_read_ms=")
          .append(formatMillis(sink.imagePlaneReadNanos()))
          .append(" image_plane_max_read_ms=")
          .append(formatMillis(sink.maxImagePlaneReadNanos()));
      }
      if (failure != null) {
        builder.append(" exception=")
          .append(quote(failure.getClass().getName()))
          .append(" message=")
          .append(quote(failure.getMessage()));
      }
      log(builder.toString());
    }

    void log(final String message)
    {
      if (!enabled) {
        return;
      }
      output.println("ATLAS_BIOFORMATS_DIAG seq=" + sequence.getAndIncrement() + " " + message + " " + heapSummary());
      output.flush();
    }

    void close()
    {
      if (closeOutput) {
        output.close();
      }
    }

    private static PrintStream createOutput(final boolean enabled, final String diagnosticsFile)
      throws FileNotFoundException
    {
      if (enabled && diagnosticsFile != null && !diagnosticsFile.isEmpty()) {
        return new PrintStream(new FileOutputStream(diagnosticsFile, true), true, StandardCharsets.UTF_8);
      }
      return System.err;
    }

    static String formatMillis(final long elapsedNanos)
    {
      return String.format(Locale.ROOT, "%.3f", elapsedNanos / 1_000_000.0);
    }

    static String quote(final String value)
    {
      if (value == null) {
        return "\"\"";
      }
      final StringBuilder builder = new StringBuilder(value.length() + 2);
      builder.append('"');
      for (int i = 0; i < value.length(); ++i) {
        final char c = value.charAt(i);
        switch (c) {
          case '\\':
            builder.append("\\\\");
            break;
          case '"':
            builder.append("\\\"");
            break;
          case '\n':
            builder.append("\\n");
            break;
          case '\r':
            builder.append("\\r");
            break;
          case '\t':
            builder.append("\\t");
            break;
          default:
            builder.append(c);
            break;
        }
      }
      builder.append('"');
      return builder.toString();
    }

    private static String heapSummary()
    {
      return HeapSnapshot.capture().summary();
    }

    private static String requestDetail(final Request request)
    {
      final StringBuilder builder = new StringBuilder();
      builder.append("request_id=").append(request.getRequestId()).append(" command=").append(commandName(request));
      switch (request.getCommandCase()) {
        case PROBE:
          builder.append(" path=")
            .append(quote(request.getProbe().getPath()))
            .append(" grouping_policy=")
            .append(request.getProbe().getGroupingPolicy().name())
            .append(" metadata_filtered=")
            .append(request.getProbe().getMetadataFiltered());
          break;
        case DATASET_INFO:
          builder.append(" path=")
            .append(quote(request.getDatasetInfo().getPath()))
            .append(" grouping_policy=")
            .append(request.getDatasetInfo().getGroupingPolicy().name())
            .append(" metadata_filtered=")
            .append(request.getDatasetInfo().getMetadataFiltered());
          break;
        case READ_REGION:
          builder.append(" path=")
            .append(quote(request.getReadRegion().getPath()))
            .append(" grouping_policy=")
            .append(request.getReadRegion().getGroupingPolicy().name())
            .append(" metadata_filtered=")
            .append(request.getReadRegion().getMetadataFiltered())
            .append(" series=")
            .append(request.getReadRegion().getSeries())
            .append(" resolution=")
            .append(request.getReadRegion().getResolution())
            .append(" x=")
            .append(request.getReadRegion().getX())
            .append(" y=")
            .append(request.getReadRegion().getY())
            .append(" width=")
            .append(request.getReadRegion().getWidth())
            .append(" height=")
            .append(request.getReadRegion().getHeight())
            .append(" z=")
            .append(request.getReadRegion().getZ())
            .append(" depth=")
            .append(request.getReadRegion().getDepth())
            .append(" c=")
            .append(request.getReadRegion().getC())
            .append(" channel_count=")
            .append(request.getReadRegion().getChannelCount())
            .append(" t=")
            .append(request.getReadRegion().getT())
            .append(" time_count=")
            .append(request.getReadRegion().getTimeCount());
          break;
        case READ_THUMBNAIL:
          builder.append(" path=")
            .append(quote(request.getReadThumbnail().getPath()))
            .append(" grouping_policy=")
            .append(request.getReadThumbnail().getGroupingPolicy().name())
            .append(" metadata_filtered=")
            .append(request.getReadThumbnail().getMetadataFiltered())
            .append(" series=")
            .append(request.getReadThumbnail().getSeries())
            .append(" z=")
            .append(request.getReadThumbnail().getZ())
            .append(" t=")
            .append(request.getReadThumbnail().getT());
          break;
        default:
          break;
      }
      return builder.toString();
    }

    private static String commandName(final Request request)
    {
      switch (request.getCommandCase()) {
        case RUNTIME_INFO:
          return "runtime_info";
        case LIST_FORMATS:
          return "list_formats";
        case PROBE:
          return "probe";
        case DATASET_INFO:
          return "dataset_info";
        case READ_REGION:
          return "read_region";
        case READ_THUMBNAIL:
          return "read_thumbnail";
        case SHUTDOWN:
          return "shutdown";
        case COMMAND_NOT_SET:
          return "missing";
      }
      return "unknown";
    }
  }

  private void run() throws Exception
  {
    if (in == null || out == null) {
      throw new IllegalStateException("stdio bridge was not initialized with streams");
    }
    final ResponseSink sink = this::writeResponse;
    try {
      boolean running = true;
      while (running) {
        final Request request;
        try {
          request = readRequest();
        }
        catch (final EOFException eof) {
          break;
        }

        try {
          running = handleRequest(request, sink);
        }
        catch (final Throwable t) {
          sendError(sink, request.getRequestId(), statusFor(t), t.getMessage());
        }
      }
    }
    finally {
      closeCachedReaders();
      diagnostics.close();
    }
  }

  private boolean handleRequest(final Request request, final ResponseSink sink) throws Exception
  {
    final long startNanos = System.nanoTime();
    final DiagnosticResponseSink diagnosticSink = diagnostics.isEnabled() ? new DiagnosticResponseSink(sink) : null;
    final ResponseSink actualSink = diagnosticSink == null ? sink : diagnosticSink;
    try {
      final boolean keepRunning = handleRequestCommand(request, actualSink);
      diagnostics.logRequestFinished(request,
                                     keepRunning ? "ok" : "shutdown",
                                     System.nanoTime() - startNanos,
                                     diagnosticSink,
                                     null);
      return keepRunning;
    }
    catch (final Throwable t) {
      diagnostics.logRequestFinished(request, "error", System.nanoTime() - startNanos, diagnosticSink, t);
      throw t;
    }
  }

  private boolean handleRequestCommand(final Request request, final ResponseSink sink) throws Exception
  {
    switch (request.getCommandCase()) {
      case RUNTIME_INFO:
        sendOk(sink, request.getRequestId(), Response.newBuilder().setRuntimeInfo(runtimeInfo()));
        return true;
      case LIST_FORMATS:
        sendOk(sink, request.getRequestId(), Response.newBuilder().setListFormats(listFormats()));
        return true;
      case PROBE:
        sendOk(sink, request.getRequestId(), Response.newBuilder().setProbe(probe(request)));
        return true;
      case DATASET_INFO:
        sendOk(sink, request.getRequestId(), Response.newBuilder().setDatasetInfo(datasetInfo(request)));
        return true;
      case READ_REGION:
        readRegion(request, sink);
        return true;
      case READ_THUMBNAIL:
        readThumbnail(request, sink);
        return true;
      case SHUTDOWN:
        sendOk(sink, request.getRequestId(), Response.newBuilder().setShutdown(ShutdownResponse.newBuilder()));
        return false;
      default:
        sendError(sink, request.getRequestId(), StatusCode.STATUS_CODE_INVALID_ARGUMENT, "missing request command");
        return true;
    }
  }

  private RuntimeInfoResponse runtimeInfo()
  {
    final Package bridgePackage = AtlasBioFormatsBridge.class.getPackage();
    final String bridgeVersion = bridgePackage == null ? "" : bridgePackage.getImplementationVersion();
    return RuntimeInfoResponse.newBuilder()
      .setProtocolVersion(PROTOCOL_VERSION)
      .setBridgeVersion(nullToEmpty(bridgeVersion))
      .setBioformatsVersion(nullToEmpty(FormatTools.VERSION))
      .setJavaVersion(nullToEmpty(System.getProperty("java.version")))
      .setJavaVmName(nullToEmpty(System.getProperty("java.vm.name")))
      .setProcessId(ProcessHandle.current().pid())
      .build();
  }

  private Request readRequest() throws IOException
  {
    final int size = readLittleEndianInt(in);
    if (size < 0 || size > MAX_FRAME_BYTES) {
      throw new IOException("invalid request frame size: " + size);
    }
    final byte[] data = in.readNBytes(size);
    if (data.length != size) {
      throw new EOFException("truncated request frame");
    }
    return Request.parseFrom(data);
  }

  private static int readLittleEndianInt(final InputStream input) throws IOException
  {
    final int b0 = input.read();
    if (b0 < 0)
      throw new EOFException();
    final int b1 = input.read();
    final int b2 = input.read();
    final int b3 = input.read();
    if ((b1 | b2 | b3) < 0)
      throw new EOFException();
    return (b0 & 0xff) | ((b1 & 0xff) << 8) | ((b2 & 0xff) << 16) | ((b3 & 0xff) << 24);
  }

  private void sendOk(final ResponseSink sink, final long requestId, final Response.Builder response) throws Exception
  {
    response.setRequestId(requestId);
    response.setStatus(StatusCode.STATUS_CODE_OK);
    sink.send(response.build());
  }

  private void sendError(final ResponseSink sink, final long requestId, final StatusCode status, final String message)
    throws Exception
  {
    final Response response = Response.newBuilder()
                                .setRequestId(requestId)
                                .setStatus(status)
                                .setErrorMessage(message == null ? status.name() : message)
                                .build();
    sink.send(response);
  }

  private void writeResponse(final Response response) throws IOException
  {
    final byte[] data = response.toByteArray();
    writeLittleEndianInt(out, data.length);
    out.write(data);
    out.flush();
  }

  private static void writeLittleEndianInt(final OutputStream output, final int value) throws IOException
  {
    output.write(value & 0xff);
    output.write((value >>> 8) & 0xff);
    output.write((value >>> 16) & 0xff);
    output.write((value >>> 24) & 0xff);
  }

  private ListFormatsResponse listFormats()
  {
    final ListFormatsResponse.Builder response = ListFormatsResponse.newBuilder();
    final ImageReader imageReader = new ImageReader();
    for (final IFormatReader reader : imageReader.getReaders()) {
      final ReaderFormat.Builder format = ReaderFormat.newBuilder()
                                            .setFormatName(nullToEmpty(reader.getFormat()))
                                            .setReaderClass(reader.getClass().getName())
                                            .setHasCompanionFiles(reader.hasCompanionFiles());
      for (final String suffix : reader.getSuffixes()) {
        if (suffix != null && !suffix.isEmpty()) {
          format.addSuffixes(suffix.startsWith(".") ? suffix.substring(1) : suffix);
        }
      }
      response.addFormats(format);
    }
    return response.build();
  }

  private ProbeResponse probe(final Request request) throws FormatException, IOException
  {
    final String path = request.getProbe().getPath();
    requireExistingFile(path);

    try (IFormatReader reader =
           createReader(request.getProbe().getGroupingPolicy(), request.getProbe().getMetadataFiltered())) {
      if (!reader.isThisType(path, true)) {
        return ProbeResponse.newBuilder().setCanRead(false).build();
      }
      reader.setId(path);
      configureReaderAfterOpen(reader);
      return ProbeResponse.newBuilder()
        .setCanRead(true)
        .setFormatName(nullToEmpty(reader.getFormat()))
        .setReaderClass(reader.getClass().getName())
        .addAllUsedFiles(nonNullArray(reader.getUsedFiles()))
        .build();
    }
  }

  private DatasetInfoResponse datasetInfo(final Request request) throws FormatException, IOException
  {
    final String path = request.getDatasetInfo().getPath();
    try (ReaderLease lease = readerCache.acquire(path,
                                                 request.getDatasetInfo().getGroupingPolicy(),
                                                 request.getDatasetInfo().getMetadataFiltered())) {
      final IFormatReader reader = lease.reader();
      final DatasetInfoResponse.Builder response = DatasetInfoResponse.newBuilder()
                                                     .setPath(path)
                                                     .setFormatName(nullToEmpty(reader.getFormat()))
                                                     .setReaderClass(reader.getClass().getName())
                                                     .addAllUsedFiles(nonNullArray(reader.getUsedFiles()));
      for (int s = 0; s < reader.getSeriesCount(); ++s) {
        response.addSeries(seriesInfo(reader, s));
      }
      lease.markReusable();
      return response.build();
    }
  }

  private SeriesInfo seriesInfo(final IFormatReader reader, final int series) throws FormatException, IOException
  {
    final int oldSeries = reader.getSeries();
    final int oldResolution = reader.getResolution();
    try {
      reader.setSeries(series);
      reader.setResolution(0);
      final int pixelType = reader.getPixelType();
      final int rgbChannelCount = Math.max(1, reader.getRGBChannelCount());
      final SeriesInfo.Builder info = SeriesInfo.newBuilder()
                                        .setSeries(series)
                                        .setSizeX(reader.getSizeX())
                                        .setSizeY(reader.getSizeY())
                                        .setSizeZ(reader.getSizeZ())
                                        .setSizeC(reader.getSizeC())
                                        .setEffectiveSizeC(reader.getEffectiveSizeC())
                                        .setSizeT(reader.getSizeT())
                                        .setImageCount(reader.getImageCount())
                                        .setRgbChannelCount(rgbChannelCount)
                                        .setBytesPerPixel(FormatTools.getBytesPerPixel(pixelType))
                                        .setPixelType(FormatTools.getPixelTypeString(pixelType))
                                        .setLittleEndian(reader.isLittleEndian())
                                        .setInterleaved(reader.isInterleaved())
                                        .setRgb(reader.isRGB())
                                        .setIndexed(reader.isIndexed())
                                        .setFalseColor(reader.isFalseColor())
                                        .setDimensionOrder(nullToEmpty(reader.getDimensionOrder()))
                                        .setResolutionCount(reader.getResolutionCount())
                                        .setOptimalTileWidth(reader.getOptimalTileWidth())
                                        .setOptimalTileHeight(reader.getOptimalTileHeight())
                                        .setThumbnailSeries(reader.isThumbnailSeries())
                                        .addAllUsedFiles(nonNullArray(reader.getSeriesUsedFiles()));
      addPhysicalSizes(reader, series, info);
      addChannelMetadata(reader, series, info);
      addOriginalMetadata(reader, info);
      addResolutionMetadata(reader, info);
      return info.build();
    }
    finally {
      reader.setSeries(oldSeries);
      reader.setResolution(oldResolution);
    }
  }

  private void addPhysicalSizes(final IFormatReader reader, final int series, final SeriesInfo.Builder info)
  {
    if (!(reader.getMetadataStore() instanceof IMetadata)) {
      return;
    }
    final IMetadata metadata = (IMetadata)reader.getMetadataStore();
    final Length sx = metadata.getPixelsPhysicalSizeX(series);
    final Length sy = metadata.getPixelsPhysicalSizeY(series);
    final Length sz = metadata.getPixelsPhysicalSizeZ(series);
    final Double sxUm = physicalSizeMicrometers(sx);
    final Double syUm = physicalSizeMicrometers(sy);
    final Double szUm = physicalSizeMicrometers(sz);
    if (sxUm != null) {
      info.setHasPhysicalSizeX(true);
      info.setPhysicalSizeXUm(sxUm);
    }
    if (syUm != null) {
      info.setHasPhysicalSizeY(true);
      info.setPhysicalSizeYUm(syUm);
    }
    if (szUm != null) {
      info.setHasPhysicalSizeZ(true);
      info.setPhysicalSizeZUm(szUm);
    }
  }

  private static Double physicalSizeMicrometers(final Length length)
  {
    if (length == null) {
      return null;
    }
    final Number value;
    try {
      value = length.value(UNITS.MICROMETER);
    }
    catch (final RuntimeException e) {
      return null;
    }
    if (value == null) {
      return null;
    }
    final double micrometers = value.doubleValue();
    return Double.isFinite(micrometers) && micrometers > 0.0 ? micrometers : null;
  }

  private void addChannelMetadata(final IFormatReader reader, final int series, final SeriesInfo.Builder info)
  {
    if (!(reader.getMetadataStore() instanceof IMetadata)) {
      return;
    }
    final IMetadata metadata = (IMetadata)reader.getMetadataStore();
    boolean hasAllColors = true;
    final ArrayList<Integer> colors = new ArrayList<>();
    for (int c = 0; c < reader.getEffectiveSizeC(); ++c) {
      final String name = metadata.getChannelName(series, c);
      info.addChannelNames(name == null ? "" : name);
      final Color color = metadata.getChannelColor(series, c);
      if (color == null) {
        hasAllColors = false;
      } else {
        colors.add(packRgba(color));
      }
    }
    if (hasAllColors) {
      info.addAllChannelColorsRgba(colors);
    }
  }

  private static int packRgba(final Color color)
  {
    return ((color.getRed() & 0xff) << 24) | ((color.getGreen() & 0xff) << 16) | ((color.getBlue() & 0xff) << 8) |
      (color.getAlpha() & 0xff);
  }

  private void addOriginalMetadata(final IFormatReader reader, final SeriesInfo.Builder info)
  {
    addMetadataTable(reader.getGlobalMetadata(), info);
    addMetadataTable(reader.getSeriesMetadata(), info);
  }

  private void addMetadataTable(final Hashtable<String, Object> metadata, final SeriesInfo.Builder info)
  {
    for (final Entry<String, Object> entry : metadata.entrySet()) {
      if (entry.getKey() != null && entry.getValue() != null) {
        info.addMetadata(MetadataEntry.newBuilder().setKey(entry.getKey()).setValue(entry.getValue().toString()));
      }
    }
  }

  private void addResolutionMetadata(final IFormatReader reader, final SeriesInfo.Builder info)
    throws FormatException, IOException
  {
    final int oldResolution = reader.getResolution();
    try {
      final int resolutionCount = Math.max(1, reader.getResolutionCount());
      for (int r = 0; r < resolutionCount; ++r) {
        reader.setResolution(r);
        info.addResolutions(ResolutionInfo.newBuilder()
                              .setResolution(r)
                              .setSizeX(reader.getSizeX())
                              .setSizeY(reader.getSizeY())
                              .setSizeZ(reader.getSizeZ())
                              .setEffectiveSizeC(reader.getEffectiveSizeC())
                              .setSizeT(reader.getSizeT())
                              .setImageCount(reader.getImageCount())
                              .setOptimalTileWidth(reader.getOptimalTileWidth())
                              .setOptimalTileHeight(reader.getOptimalTileHeight()));
      }
    }
    finally {
      reader.setResolution(oldResolution);
    }
  }

  private void readRegion(final Request request, final ResponseSink sink) throws Exception
  {
    final var readRegion = request.getReadRegion();
    try (
      ReaderLease lease =
        readerCache.acquire(readRegion.getPath(), readRegion.getGroupingPolicy(), readRegion.getMetadataFiltered())) {
      final IFormatReader reader = lease.reader();
      final int series = toInt("series", readRegion.getSeries());
      final int resolution = toInt("resolution", readRegion.getResolution());
      final int x = toInt("x", readRegion.getX());
      final int y = toInt("y", readRegion.getY());
      final int z0 = toInt("z", readRegion.getZ());
      final int c0 = toInt("c", readRegion.getC());
      final int t0 = toInt("t", readRegion.getT());
      final int width = toPositiveInt("width", readRegion.getWidth());
      final int height = toPositiveInt("height", readRegion.getHeight());
      final int depth = toPositiveInt("depth", readRegion.getDepth());
      final int channelCount = toPositiveInt("channel_count", readRegion.getChannelCount());
      final int timeCount = toPositiveInt("time_count", readRegion.getTimeCount());

      reader.setSeries(series);
      reader.setResolution(resolution);
      validateRegion(reader, x, y, z0, c0, t0, width, height, depth, channelCount, timeCount);

      final ChunkWriter chunks = new ChunkWriter(sink, request.getRequestId(), sink.pixelChunkBytes());
      final int rgbChannelCount = Math.max(1, reader.getRGBChannelCount());
      final int bytesPerPixel = FormatTools.getBytesPerPixel(reader.getPixelType());
      final boolean littleEndian = reader.isLittleEndian();
      final boolean interleaved = reader.isInterleaved();
      final int independentPlaneConcurrency =
        rgbChannelCount == 1
          ? readerCache.internalPlaneReadConcurrency(saturatedPlaneCount(depth, channelCount, timeCount))
          : 1;
      if (independentPlaneConcurrency > 1) {
        // Return the validated reader before fan-out so one worker can reuse it from the cache.
        lease.markReusable();
        lease.close();
        readIndependentPlanesConcurrently(sink,
                                          readRegion.getPath(),
                                          readRegion.getGroupingPolicy(),
                                          readRegion.getMetadataFiltered(),
                                          series,
                                          resolution,
                                          x,
                                          y,
                                          z0,
                                          c0,
                                          t0,
                                          width,
                                          height,
                                          depth,
                                          channelCount,
                                          timeCount,
                                          independentPlaneConcurrency,
                                          chunks,
                                          bytesPerPixel,
                                          littleEndian);
        chunks.finish();
        return;
      }
      byte[] cachedRgbPlane = null;
      int cachedRgbPlaneT = Integer.MIN_VALUE;
      int cachedRgbPlaneZ = Integer.MIN_VALUE;
      int cachedRgbPlaneBioC = Integer.MIN_VALUE;
      for (int t = t0; t < t0 + timeCount; ++t) {
        for (int atlasC = c0; atlasC < c0 + channelCount; ++atlasC) {
          final int bioC = atlasC / rgbChannelCount;
          final int rgbC = atlasC % rgbChannelCount;
          for (int z = z0; z < z0 + depth; ++z) {
            final int plane = reader.getIndex(z, bioC, t);
            if (rgbChannelCount == 1) {
              final byte[] image = openRegionBytes(sink, reader, plane, x, y, width, height);
              chunks.writePlane(image, bytesPerPixel, littleEndian);
            } else {
              final byte[] image;
              if (depth == 1 && cachedRgbPlane != null && cachedRgbPlaneT == t && cachedRgbPlaneZ == z &&
                  cachedRgbPlaneBioC == bioC) {
                image = cachedRgbPlane;
              } else {
                image = openRegionBytes(sink, reader, plane, x, y, width, height);
                if (depth == 1) {
                  cachedRgbPlane = image;
                  cachedRgbPlaneT = t;
                  cachedRgbPlaneZ = z;
                  cachedRgbPlaneBioC = bioC;
                }
              }
              chunks
                .writeRgbChannel(image, width, height, rgbChannelCount, rgbC, bytesPerPixel, littleEndian, interleaved);
            }
          }
        }
      }
      chunks.finish();
      lease.markReusable();
    }
  }

  private void readIndependentPlanesConcurrently(final ResponseSink sink,
                                                 final String path,
                                                 final FileGroupingPolicy groupingPolicy,
                                                 final boolean metadataFiltered,
                                                 final int series,
                                                 final int resolution,
                                                 final int x,
                                                 final int y,
                                                 final int z0,
                                                 final int c0,
                                                 final int t0,
                                                 final int width,
                                                 final int height,
                                                 final int depth,
                                                 final int channelCount,
                                                 final int timeCount,
                                                 final int concurrency,
                                                 final ChunkWriter chunks,
                                                 final int bytesPerPixel,
                                                 final boolean littleEndian) throws Exception
  {
    final ArrayList<Future<byte[]>> pendingReads = new ArrayList<>(concurrency);
    for (int t = t0; t < t0 + timeCount; ++t) {
      for (int bioC = c0; bioC < c0 + channelCount; ++bioC) {
        for (int z = z0; z < z0 + depth; ++z) {
          pendingReads.add(submitIndependentPlaneRead(sink,
                                                      path,
                                                      groupingPolicy,
                                                      metadataFiltered,
                                                      series,
                                                      resolution,
                                                      x,
                                                      y,
                                                      width,
                                                      height,
                                                      new PlaneReadRequest(z, bioC, t)));
          if (pendingReads.size() == concurrency) {
            writeIndependentPlaneBatch(pendingReads, chunks, bytesPerPixel, littleEndian);
            pendingReads.clear();
          }
        }
      }
    }
    if (!pendingReads.isEmpty()) {
      writeIndependentPlaneBatch(pendingReads, chunks, bytesPerPixel, littleEndian);
    }
  }

  private Future<byte[]> submitIndependentPlaneRead(final ResponseSink sink,
                                                    final String path,
                                                    final FileGroupingPolicy groupingPolicy,
                                                    final boolean metadataFiltered,
                                                    final int series,
                                                    final int resolution,
                                                    final int x,
                                                    final int y,
                                                    final int width,
                                                    final int height,
                                                    final PlaneReadRequest request)
  {
    return planeReadExecutor.submit(() -> {
      try (ReaderLease lease = readerCache.acquire(path, groupingPolicy, metadataFiltered)) {
        final IFormatReader reader = lease.reader();
        reader.setSeries(series);
        reader.setResolution(resolution);
        final byte[] image =
          openRegionBytes(sink, reader, reader.getIndex(request.z, request.bioC, request.t), x, y, width, height);
        lease.markReusable();
        return image;
      }
    });
  }

  private void writeIndependentPlaneBatch(final ArrayList<Future<byte[]>> pendingReads,
                                          final ChunkWriter chunks,
                                          final int bytesPerPixel,
                                          final boolean littleEndian) throws Exception
  {
    boolean completed = false;
    try {
      for (final Future<byte[]> pendingRead : pendingReads) {
        chunks.writePlane(waitForPlaneRead(pendingRead), bytesPerPixel, littleEndian);
      }
      completed = true;
    }
    finally {
      if (!completed) {
        for (final Future<byte[]> pendingRead : pendingReads) {
          pendingRead.cancel(true);
        }
      }
    }
  }

  private static byte[] waitForPlaneRead(final Future<byte[]> pendingRead) throws Exception
  {
    try {
      return pendingRead.get();
    }
    catch (final InterruptedException e) {
      Thread.currentThread().interrupt();
      throw e;
    }
    catch (final ExecutionException e) {
      final Throwable cause = e.getCause();
      if (cause instanceof Exception) {
        throw (Exception)cause;
      }
      if (cause instanceof Error) {
        throw (Error)cause;
      }
      throw new RuntimeException(cause);
    }
  }

  private static long saturatedPlaneCount(final int depth, final int channelCount, final int timeCount)
  {
    return saturatedMultiply(saturatedMultiply(depth, channelCount), timeCount);
  }

  private static long saturatedMultiply(final long lhs, final int rhs)
  {
    if (lhs > Long.MAX_VALUE / rhs) {
      return Long.MAX_VALUE;
    }
    return lhs * rhs;
  }

  private static final class PlaneReadRequest
  {
    final int z;
    final int bioC;
    final int t;

    PlaneReadRequest(final int z, final int bioC, final int t)
    {
      this.z = z;
      this.bioC = bioC;
      this.t = t;
    }
  }

  private static byte[] openRegionBytes(final ResponseSink sink,
                                        final IFormatReader reader,
                                        final int plane,
                                        final int x,
                                        final int y,
                                        final int width,
                                        final int height) throws Exception
  {
    if (!sink.recordsImagePlaneReads()) {
      return reader.openBytes(plane, x, y, width, height);
    }
    final long startNanos = System.nanoTime();
    final byte[] image = reader.openBytes(plane, x, y, width, height);
    sink.recordImagePlaneRead(image.length, System.nanoTime() - startNanos);
    return image;
  }

  private void readThumbnail(final Request request, final ResponseSink sink) throws Exception
  {
    final var readThumbnail = request.getReadThumbnail();
    try (ReaderLease lease = readerCache.acquire(readThumbnail.getPath(),
                                                 readThumbnail.getGroupingPolicy(),
                                                 readThumbnail.getMetadataFiltered())) {
      final IFormatReader reader = lease.reader();
      final int series = toInt("series", readThumbnail.getSeries());
      final int z = toInt("z", readThumbnail.getZ());
      final int t = toInt("t", readThumbnail.getT());

      reader.setSeries(series);
      reader.setResolution(0);
      if (z < 0 || t < 0 || z >= reader.getSizeZ() || t >= reader.getSizeT()) {
        throw new IllegalArgumentException("requested thumbnail plane is outside the dataset");
      }

      final int width = reader.getThumbSizeX();
      final int height = reader.getThumbSizeY();
      if (width <= 0 || height <= 0) {
        throw new FormatException("reader did not provide a usable thumbnail size");
      }

      final int rgbChannelCount = Math.max(1, reader.getRGBChannelCount());
      final int atlasChannelCount = reader.getEffectiveSizeC() * rgbChannelCount;
      final int bytesPerPixel = FormatTools.getBytesPerPixel(reader.getPixelType());
      final boolean littleEndian = reader.isLittleEndian();
      final boolean interleaved = reader.isInterleaved();
      final ThumbnailChunkWriter chunks =
        new ThumbnailChunkWriter(sink,
                                 request.getRequestId(),
                                 width,
                                 height,
                                 atlasChannelCount,
                                 bytesPerPixel,
                                 FormatTools.getPixelTypeString(reader.getPixelType()),
                                 sink.pixelChunkBytes());

      for (int atlasC = 0; atlasC < atlasChannelCount; ++atlasC) {
        final int bioC = atlasC / rgbChannelCount;
        final int rgbC = atlasC % rgbChannelCount;
        final int plane = reader.getIndex(z, bioC, t);
        final byte[] image = reader.openThumbBytes(plane);
        if (rgbChannelCount == 1) {
          chunks.writePlane(image, bytesPerPixel, littleEndian);
        } else {
          chunks.writeRgbChannel(image, width, height, rgbChannelCount, rgbC, bytesPerPixel, littleEndian, interleaved);
        }
      }
      chunks.finish();
      lease.markReusable();
    }
  }

  private void closeCachedReaders()
  {
    planeReadExecutor.shutdownNow();
    readerCache.close();
  }

  private void validateRegion(final IFormatReader reader,
                              final int x,
                              final int y,
                              final int z,
                              final int c,
                              final int t,
                              final int width,
                              final int height,
                              final int depth,
                              final int channelCount,
                              final int timeCount)
  {
    final int rgbChannelCount = Math.max(1, reader.getRGBChannelCount());
    final int atlasChannelCount = reader.getEffectiveSizeC() * rgbChannelCount;
    if (x < 0 || y < 0 || z < 0 || c < 0 || t < 0 || x + width > reader.getSizeX() || y + height > reader.getSizeY() ||
        z + depth > reader.getSizeZ() || c + channelCount > atlasChannelCount || t + timeCount > reader.getSizeT()) {
      throw new IllegalArgumentException("requested region is outside the dataset");
    }
  }

  private static final class ReaderKey
  {
    final String normalizedPath;
    final FileGroupingPolicy groupingPolicy;
    final boolean metadataFiltered;

    private ReaderKey(final String normalizedPath,
                      final FileGroupingPolicy groupingPolicy,
                      final boolean metadataFiltered)
    {
      this.normalizedPath = normalizedPath;
      this.groupingPolicy = groupingPolicy;
      this.metadataFiltered = metadataFiltered;
    }

    static ReaderKey from(final String path, final FileGroupingPolicy groupingPolicy, final boolean metadataFiltered)
      throws IOException
    {
      return new ReaderKey(normalizeExistingPath(path), groupingPolicy, metadataFiltered);
    }

    @Override public boolean equals(final Object other)
    {
      if (this == other) {
        return true;
      }
      if (!(other instanceof ReaderKey)) {
        return false;
      }
      final ReaderKey rhs = (ReaderKey)other;
      return metadataFiltered == rhs.metadataFiltered && normalizedPath.equals(rhs.normalizedPath) &&
        groupingPolicy == rhs.groupingPolicy;
    }

    @Override public int hashCode()
    {
      return Objects.hash(normalizedPath, groupingPolicy, metadataFiltered);
    }
  }

  private static final class CachedReader
  {
    final ReaderKey key;
    final IFormatReader reader;
    final ArrayList<FileFingerprint> fileFingerprints;
    final long idleSinceMillis;

    CachedReader(final ReaderKey key,
                 final IFormatReader reader,
                 final ArrayList<FileFingerprint> fileFingerprints,
                 final long idleSinceMillis)
    {
      this.key = key;
      this.reader = reader;
      this.fileFingerprints = fileFingerprints;
      this.idleSinceMillis = idleSinceMillis;
    }

    boolean isCurrent()
    {
      for (final FileFingerprint fingerprint : fileFingerprints) {
        if (!fingerprint.matchesCurrentFile()) {
          return false;
        }
      }
      return true;
    }
  }

  private static final class FileFingerprint
  {
    final String normalizedPath;
    final long lastModifiedMillis;
    final long length;

    private FileFingerprint(final String normalizedPath, final long lastModifiedMillis, final long length)
    {
      this.normalizedPath = normalizedPath;
      this.lastModifiedMillis = lastModifiedMillis;
      this.length = length;
    }

    static FileFingerprint fromPath(final String path) throws IOException
    {
      final File file = requireExistingFile(path);
      return new FileFingerprint(file.getCanonicalPath(), file.lastModified(), file.length());
    }

    boolean matchesCurrentFile()
    {
      final File file = new File(normalizedPath);
      return file.exists() && file.lastModified() == lastModifiedMillis && file.length() == length;
    }
  }

  private enum HeapPressureLevel
  {
    NORMAL("normal"),
    PRESSURE("pressure"),
    CRITICAL("critical");

    private final String diagnosticName;

    HeapPressureLevel(final String diagnosticName)
    {
      this.diagnosticName = diagnosticName;
    }

    String diagnosticName()
    {
      return diagnosticName;
    }
  }

  private static final class HeapSnapshot
  {
    private final long usedBytes;
    private final long committedBytes;
    private final long maxBytes;

    private HeapSnapshot(final long usedBytes, final long committedBytes, final long maxBytes)
    {
      this.usedBytes = usedBytes;
      this.committedBytes = committedBytes;
      this.maxBytes = maxBytes;
    }

    static HeapSnapshot capture()
    {
      final Runtime runtime = Runtime.getRuntime();
      final long committedBytes = runtime.totalMemory();
      return new HeapSnapshot(committedBytes - runtime.freeMemory(), committedBytes, runtime.maxMemory());
    }

    boolean hasBoundedMax()
    {
      return maxBytes > 0 && maxBytes != Long.MAX_VALUE;
    }

    long usedPercentOfMax()
    {
      if (!hasBoundedMax()) {
        return 0;
      }
      return Math.min(100, (usedBytes * 100) / maxBytes);
    }

    HeapPressureLevel pressureLevel()
    {
      if (!hasBoundedMax()) {
        return HeapPressureLevel.NORMAL;
      }
      final long usedPercent = usedPercentOfMax();
      if (usedPercent >= READER_CACHE_HEAP_CRITICAL_USED_PERCENT) {
        return HeapPressureLevel.CRITICAL;
      }
      if (usedPercent >= READER_CACHE_HEAP_PRESSURE_USED_PERCENT) {
        return HeapPressureLevel.PRESSURE;
      }
      return HeapPressureLevel.NORMAL;
    }

    String summary()
    {
      return "heap_used_mb=" + bytesToMiB(usedBytes) + " heap_committed_mb=" + bytesToMiB(committedBytes) +
        " heap_max_mb=" + bytesToMiB(maxBytes);
    }

    private static long bytesToMiB(final long bytes)
    {
      return bytes / (1024L * 1024L);
    }
  }

  private static final class TrimResult
  {
    private final ArrayList<IFormatReader> readersToClose = new ArrayList<>();
    private final HeapPressureLevel heapPressureLevel;
    private final int idleReaderTarget;
    private int expiredReaders = 0;
    private int overflowReaders = 0;
    private int heapPressureReaders = 0;

    TrimResult(final HeapPressureLevel heapPressureLevel, final int idleReaderTarget)
    {
      this.heapPressureLevel = heapPressureLevel;
      this.idleReaderTarget = idleReaderTarget;
    }

    static TrimResult empty()
    {
      return new TrimResult(HeapPressureLevel.NORMAL, 0);
    }

    boolean isEmpty()
    {
      return readersToClose.isEmpty();
    }

    int size()
    {
      return readersToClose.size();
    }

    void addExpired(final IFormatReader reader)
    {
      readersToClose.add(reader);
      ++expiredReaders;
    }

    void addOverflow(final IFormatReader reader)
    {
      readersToClose.add(reader);
      ++overflowReaders;
    }

    void addHeapPressure(final IFormatReader reader)
    {
      readersToClose.add(reader);
      ++heapPressureReaders;
    }

    String detail()
    {
      if (isEmpty()) {
        return "";
      }
      return " trim_expired=" + expiredReaders + " trim_overflow=" + overflowReaders +
        " trim_heap_pressure=" + heapPressureReaders +
        " trim_heap_pressure_level=" + heapPressureLevel.diagnosticName() +
        " trim_idle_reader_target=" + idleReaderTarget;
    }
  }

  private static final class ReaderCache
  {
    private final Object lock = new Object();
    private final BridgeDiagnostics diagnostics;
    private final Map<ReaderKey, ArrayDeque<CachedReader>> idleByKey = new HashMap<>();
    private final ArrayDeque<CachedReader> idleReaders = new ArrayDeque<>();
    private final ScheduledExecutorService trimExecutor =
      Executors.newSingleThreadScheduledExecutor(new ThreadFactory() {
        @Override public Thread newThread(final Runnable runnable)
        {
          final Thread thread = new Thread(runnable, "atlas-bioformats-reader-cache-trim");
          thread.setDaemon(true);
          return thread;
        }
      });
    private ScheduledFuture<?> scheduledTrim = null;
    private boolean closed = false;
    private int activeReaders = 0;
    private long openedReaders = 0;
    private long cacheHits = 0;
    private long cacheMisses = 0;
    private long closedReaders = 0;
    private long heapPressureClosedReaders = 0;
    private int peakActiveReaders = 0;

    ReaderCache(final BridgeDiagnostics diagnostics)
    {
      this.diagnostics = diagnostics;
    }

    ReaderLease acquire(final String path, final FileGroupingPolicy groupingPolicy, final boolean metadataFiltered)
      throws FormatException, IOException
    {
      final ReaderKey key = ReaderKey.from(path, groupingPolicy, metadataFiltered);
      final long startNanos = System.nanoTime();
      final CachedReader cachedReader = takeIdleReader(key);
      if (cachedReader != null) {
        log("event=reader_cache action=acquire result=hit " + keyDetail(key) +
            " elapsed_ms=" + BridgeDiagnostics.formatMillis(System.nanoTime() - startNanos) + stats());
        return new ReaderLease(this, key, cachedReader.reader);
      }
      final long openStartNanos = System.nanoTime();
      final IFormatReader reader;
      try {
        reader = openReader(key);
      }
      catch (final FormatException | IOException | RuntimeException e) {
        log("event=reader_cache action=open result=error " + keyDetail(key) +
            " elapsed_ms=" + BridgeDiagnostics.formatMillis(System.nanoTime() - openStartNanos) + " exception=" +
            BridgeDiagnostics.quote(e.getClass().getName()) + " message=" + BridgeDiagnostics.quote(e.getMessage()));
        throw e;
      }

      boolean keepReader = false;
      synchronized (lock) {
        if (!closed) {
          markActiveReaderLocked();
          ++openedReaders;
          ++cacheMisses;
          keepReader = true;
        }
      }
      if (!keepReader) {
        closeReaderQuietly(reader);
        throw new IOException("reader cache is closed");
      }
      log("event=reader_cache action=acquire result=miss " + keyDetail(key) +
          " elapsed_ms=" + BridgeDiagnostics.formatMillis(System.nanoTime() - startNanos) +
          " open_ms=" + BridgeDiagnostics.formatMillis(System.nanoTime() - openStartNanos) + stats());
      return new ReaderLease(this, key, reader);
    }

    private CachedReader takeIdleReader(final ReaderKey key) throws IOException
    {
      final ArrayList<IFormatReader> readersToClose = new ArrayList<>();
      final CachedReader cachedReader;
      synchronized (lock) {
        if (closed) {
          throw new IOException("reader cache is closed");
        }
        final TrimResult trimResult = trimLocked(System.currentTimeMillis());
        readersToClose.addAll(trimResult.readersToClose);
        final ArrayDeque<CachedReader> queue = idleByKey.get(key);
        CachedReader reusableReader = null;
        while (queue != null && !queue.isEmpty()) {
          final CachedReader candidate = queue.pollLast();
          idleReaders.remove(candidate);
          if (candidate.isCurrent()) {
            reusableReader = candidate;
            break;
          }
          readersToClose.add(candidate.reader);
        }
        cachedReader = reusableReader;
        if (queue != null && queue.isEmpty()) {
          idleByKey.remove(key);
        }
        if (cachedReader != null) {
          markActiveReaderLocked();
          ++cacheHits;
        }
        closedReaders += readersToClose.size();
        heapPressureClosedReaders += trimResult.heapPressureReaders;
        scheduleTrimLocked(System.currentTimeMillis());
      }
      closeReadersQuietly(readersToClose);
      if (!readersToClose.isEmpty()) {
        log("event=reader_cache action=close_stale count=" + readersToClose.size() + " " + keyDetail(key) + stats());
      }
      return cachedReader;
    }

    void release(final ReaderKey key, final IFormatReader reader)
    {
      final ArrayList<IFormatReader> readersToClose = new ArrayList<>();
      final ArrayList<FileFingerprint> fileFingerprints = fileFingerprintsFor(reader, key);
      final String action;
      TrimResult trimResult = TrimResult.empty();
      synchronized (lock) {
        decrementActiveReaderLocked();
        final long nowMillis = System.currentTimeMillis();
        if (closed || READER_CACHE_IDLE_MILLIS == 0 || fileFingerprints.isEmpty()) {
          readersToClose.add(reader);
          action = "release_close";
        } else {
          final CachedReader cachedReader = new CachedReader(key, reader, fileFingerprints, nowMillis);
          idleByKey.computeIfAbsent(key, ignored -> new ArrayDeque<>()).addLast(cachedReader);
          idleReaders.addLast(cachedReader);
          trimResult = trimLocked(nowMillis);
          readersToClose.addAll(trimResult.readersToClose);
          scheduleTrimLocked(nowMillis);
          action = "release_cache";
        }
        closedReaders += readersToClose.size();
        heapPressureClosedReaders += trimResult.heapPressureReaders;
      }
      log("event=reader_cache action=" + action + " " + keyDetail(key) + " fingerprint_count=" +
          fileFingerprints.size() + " closed_now=" + readersToClose.size() + trimResult.detail() + stats());
      closeReadersQuietly(readersToClose);
    }

    void discard(final ReaderKey key, final IFormatReader reader, final String reason)
    {
      synchronized (lock) {
        decrementActiveReaderLocked();
        ++closedReaders;
      }
      log("event=reader_cache action=discard reason=" + BridgeDiagnostics.quote(reason) + " " + keyDetail(key) +
          stats());
      closeReaderQuietly(reader);
    }

    void close()
    {
      final ArrayList<IFormatReader> readersToClose = new ArrayList<>();
      synchronized (lock) {
        if (closed) {
          return;
        }
        closed = true;
        cancelScheduledTrimLocked();
        while (!idleReaders.isEmpty()) {
          readersToClose.add(idleReaders.removeFirst().reader);
        }
        idleByKey.clear();
        closedReaders += readersToClose.size();
      }
      trimExecutor.shutdownNow();
      log("event=reader_cache action=shutdown closed_now=" + readersToClose.size() + stats());
      closeReadersQuietly(readersToClose);
    }

    int internalPlaneReadConcurrency(final long planeReadCount)
    {
      if (planeReadCount <= 1 || HeapSnapshot.capture().pressureLevel() != HeapPressureLevel.NORMAL) {
        return 1;
      }
      synchronized (lock) {
        // External bridge concurrency takes precedence over multiplying readers inside each request.
        if (closed || activeReaders > 1) {
          return 1;
        }
      }
      return (int)Math.min(planeReadCount, JAVA_AVAILABLE_PROCESSORS);
    }

    private static ArrayList<FileFingerprint> fileFingerprintsFor(final IFormatReader reader, final ReaderKey key)
    {
      final ArrayList<FileFingerprint> fingerprints = new ArrayList<>();
      try {
        final String[] usedFiles = reader.getUsedFiles();
        if (usedFiles == null || usedFiles.length == 0) {
          fingerprints.add(FileFingerprint.fromPath(key.normalizedPath));
          return fingerprints;
        }
        for (final String usedFile : usedFiles) {
          if (usedFile != null && !usedFile.isEmpty()) {
            fingerprints.add(FileFingerprint.fromPath(usedFile));
          }
        }
      }
      catch (final IOException | RuntimeException ignored) {
        fingerprints.clear();
      }
      return fingerprints;
    }

    private void trimIdleReaders()
    {
      final TrimResult trimResult;
      synchronized (lock) {
        if (closed) {
          return;
        }
        scheduledTrim = null;
        trimResult = trimLocked(System.currentTimeMillis());
        closedReaders += trimResult.size();
        heapPressureClosedReaders += trimResult.heapPressureReaders;
        scheduleTrimLocked(System.currentTimeMillis());
      }
      if (!trimResult.isEmpty()) {
        log("event=reader_cache action=trim closed_now=" + trimResult.size() + trimResult.detail() + stats());
      }
      closeReadersQuietly(trimResult.readersToClose);
    }

    private TrimResult trimLocked(final long nowMillis)
    {
      final HeapSnapshot heapSnapshot = HeapSnapshot.capture();
      final HeapPressureLevel heapPressureLevel = heapSnapshot.pressureLevel();
      final int idleReaderTarget = idleReaderTargetLocked(heapSnapshot);
      final TrimResult trimResult = new TrimResult(heapPressureLevel, idleReaderTarget);
      while (!idleReaders.isEmpty()) {
        final CachedReader oldest = idleReaders.peekFirst();
        final boolean expired =
          READER_CACHE_IDLE_MILLIS == 0 || nowMillis - oldest.idleSinceMillis >= READER_CACHE_IDLE_MILLIS;
        final boolean overflow = idleReaders.size() > idleReaderTarget;
        if (!expired && !overflow) {
          break;
        }
        removeIdleReaderLocked(oldest);
        if (expired) {
          trimResult.addExpired(oldest.reader);
        } else if (heapPressureLevel != HeapPressureLevel.NORMAL) {
          trimResult.addHeapPressure(oldest.reader);
        } else {
          trimResult.addOverflow(oldest.reader);
        }
      }
      return trimResult;
    }

    private void scheduleTrimLocked(final long nowMillis)
    {
      if (closed || idleReaders.isEmpty() || READER_CACHE_IDLE_MILLIS == 0) {
        cancelScheduledTrimLocked();
        return;
      }
      if (scheduledTrim != null && !scheduledTrim.isDone()) {
        return;
      }
      final long oldestIdleSinceMillis = idleReaders.peekFirst().idleSinceMillis;
      final long delayMillis = Math.max(1L, oldestIdleSinceMillis + READER_CACHE_IDLE_MILLIS - nowMillis);
      scheduledTrim = trimExecutor.schedule(this::trimIdleReaders, delayMillis, TimeUnit.MILLISECONDS);
    }

    private void cancelScheduledTrimLocked()
    {
      if (scheduledTrim != null) {
        scheduledTrim.cancel(false);
        scheduledTrim = null;
      }
    }

    private void removeIdleReaderLocked(final CachedReader cachedReader)
    {
      idleReaders.remove(cachedReader);
      final ArrayDeque<CachedReader> queue = idleByKey.get(cachedReader.key);
      if (queue == null) {
        return;
      }
      queue.remove(cachedReader);
      if (queue.isEmpty()) {
        idleByKey.remove(cachedReader.key);
      }
    }

    private void markActiveReaderLocked()
    {
      ++activeReaders;
      if (activeReaders > peakActiveReaders) {
        peakActiveReaders = activeReaders;
      }
    }

    private int idleReaderTargetLocked(final HeapSnapshot heapSnapshot)
    {
      final int adaptiveTarget =
        Math.min(READER_CACHE_MAX_IDLE_READERS, Math.max(READER_CACHE_MIN_IDLE_READERS, peakActiveReaders));
      final HeapPressureLevel pressureLevel = heapSnapshot.pressureLevel();
      if (pressureLevel == HeapPressureLevel.NORMAL) {
        return adaptiveTarget;
      }
      if (pressureLevel == HeapPressureLevel.CRITICAL) {
        return 0;
      }
      final int pressureRange = READER_CACHE_HEAP_CRITICAL_USED_PERCENT - READER_CACHE_HEAP_PRESSURE_USED_PERCENT;
      final long remainingRange = READER_CACHE_HEAP_CRITICAL_USED_PERCENT - heapSnapshot.usedPercentOfMax();
      final int pressureTarget = (int)((adaptiveTarget * remainingRange + pressureRange - 1) / pressureRange);
      return Math.min(adaptiveTarget, Math.max(READER_CACHE_MIN_IDLE_READERS, pressureTarget));
    }

    private void decrementActiveReaderLocked()
    {
      if (activeReaders <= 0) {
        throw new IllegalStateException("reader cache active reader count underflow");
      }
      --activeReaders;
    }

    private String stats()
    {
      if (!diagnostics.isEnabled()) {
        return "";
      }
      synchronized (lock) {
        return statsLocked();
      }
    }

    private String statsLocked()
    {
      final HeapSnapshot heapSnapshot = HeapSnapshot.capture();
      return " active_readers=" + activeReaders + " idle_readers=" + idleReaders.size() +
        " opened_readers=" + openedReaders + " cache_hits=" + cacheHits + " cache_misses=" + cacheMisses +
        " closed_readers=" + closedReaders + " peak_active_readers=" + peakActiveReaders +
        " idle_reader_target=" + idleReaderTargetLocked(heapSnapshot) +
        " idle_reader_limit=" + READER_CACHE_MAX_IDLE_READERS +
        " heap_pressure_level=" + heapSnapshot.pressureLevel().diagnosticName() +
        " heap_pressure_closed_readers=" + heapPressureClosedReaders;
    }

    private void log(final String message)
    {
      diagnostics.log(message);
    }
  }

  private static final class ReaderLease implements AutoCloseable
  {
    private final ReaderCache cache;
    private final ReaderKey key;
    private IFormatReader reader;
    private boolean reusable = false;

    ReaderLease(final ReaderCache cache, final ReaderKey key, final IFormatReader reader)
    {
      this.cache = cache;
      this.key = key;
      this.reader = reader;
    }

    IFormatReader reader()
    {
      if (reader == null) {
        throw new IllegalStateException("reader lease has already been closed");
      }
      return reader;
    }

    void markReusable()
    {
      reusable = true;
    }

    @Override public void close()
    {
      if (reader == null) {
        return;
      }
      final IFormatReader readerToRelease = reader;
      reader = null;
      if (reusable) {
        cache.release(key, readerToRelease);
      } else {
        cache.discard(key, readerToRelease, "not_reusable");
      }
    }
  }

  private static String keyDetail(final ReaderKey key)
  {
    return "path=" + BridgeDiagnostics.quote(key.normalizedPath) + " grouping_policy=" + key.groupingPolicy.name() +
      " metadata_filtered=" + key.metadataFiltered;
  }

  private static IFormatReader createReader(final FileGroupingPolicy groupingPolicy, final boolean metadataFiltered)
    throws FormatException, IOException
  {
    final IFormatReader reader = new ImageReader();
    reader.setFlattenedResolutions(false);
    if (groupingPolicy == FileGroupingPolicy.FILE_GROUPING_POLICY_ENABLED) {
      reader.setGroupFiles(true);
    } else if (groupingPolicy == FileGroupingPolicy.FILE_GROUPING_POLICY_DISABLED) {
      reader.setGroupFiles(false);
    }
    reader.setMetadataFiltered(metadataFiltered);
    reader.setOriginalMetadataPopulated(true);
    final MetadataStore store = MetadataTools.createOMEXMLMetadata();
    if (store != null) {
      reader.setMetadataStore(store);
    }
    return reader;
  }

  private static IFormatReader openReader(final ReaderKey key) throws FormatException, IOException
  {
    final IFormatReader reader = createReader(key.groupingPolicy, key.metadataFiltered);
    boolean success = false;
    try {
      reader.setId(key.normalizedPath);
      configureReaderAfterOpen(reader);
      success = true;
      return reader;
    }
    finally {
      if (!success) {
        reader.close();
      }
    }
  }

  private static void configureReaderAfterOpen(final IFormatReader reader)
  {
    final IFormatReader activeReader = reader instanceof ImageReader ? ((ImageReader)reader).getReader() : reader;
    if (activeReader instanceof SDTReader) {
      // SDTReader's default preload cache can retain hundreds of MB for one requested plane. Atlas reads SDT data as
      // small regions through cached bridge readers, so keep SDT reads streaming instead of retaining that per-reader
      // cache.
      ((SDTReader)activeReader).setPreLoad(false);
    }
  }

  private static String normalizeExistingPath(final String path) throws IOException
  {
    return requireExistingFile(path).getCanonicalPath();
  }

  private static File requireExistingFile(final String path) throws FileNotFoundException
  {
    if (path == null || path.isEmpty()) {
      throw new FileNotFoundException("empty path");
    }
    final File file = new File(path);
    if (!file.exists()) {
      throw new FileNotFoundException(path);
    }
    return file;
  }

  private static void closeReadersQuietly(final Iterable<IFormatReader> readers)
  {
    for (final IFormatReader reader : readers) {
      closeReaderQuietly(reader);
    }
  }

  private static void closeReaderQuietly(final IFormatReader reader)
  {
    try {
      reader.close();
    }
    catch (final IOException ignored) {
    }
  }

  private static StatusCode statusFor(final Throwable t)
  {
    if (t instanceof FileNotFoundException) {
      return StatusCode.STATUS_CODE_NOT_FOUND;
    }
    if (t instanceof IllegalArgumentException) {
      return StatusCode.STATUS_CODE_INVALID_ARGUMENT;
    }
    if (t instanceof FormatException) {
      return StatusCode.STATUS_CODE_UNSUPPORTED;
    }
    if (t instanceof IOException) {
      return StatusCode.STATUS_CODE_IO_ERROR;
    }
    return StatusCode.STATUS_CODE_INTERNAL;
  }

  private static String nullToEmpty(final String value)
  {
    return value == null ? "" : value;
  }

  private static java.util.List<String> nonNullArray(final String[] values)
  {
    final ArrayList<String> result = new ArrayList<>();
    if (values == null) {
      return result;
    }
    for (final String value : values) {
      if (value != null) {
        result.add(value);
      }
    }
    return result;
  }

  private static int toInt(final String name, final long value)
  {
    if (value > Integer.MAX_VALUE) {
      throw new IllegalArgumentException(name + " exceeds Bio-Formats int range: " + value);
    }
    return (int)value;
  }

  private static int toPositiveInt(final String name, final long value)
  {
    final int result = toInt(name, value);
    if (result <= 0) {
      throw new IllegalArgumentException(name + " must be positive");
    }
    return result;
  }

  private final class ChunkWriter
  {
    private final ResponseSink sink;
    private final long requestId;
    private final int chunkBytes;
    private final ByteArrayOutputStream buffer;
    private int sequenceIndex = 0;
    private long totalBytes = 0;

    ChunkWriter(final ResponseSink sink, final long requestId, final int chunkBytes)
    {
      this.sink = sink;
      this.requestId = requestId;
      this.chunkBytes = chunkBytes;
      this.buffer = new ByteArrayOutputStream(chunkBytes);
    }

    void writePlane(final byte[] image, final int bytesPerPixel, final boolean littleEndian) throws Exception
    {
      if (littleEndian || bytesPerPixel == 1) {
        writeBytes(image, 0, image.length);
        return;
      }
      for (int i = 0; i < image.length; i += bytesPerPixel) {
        writeSample(image, i, bytesPerPixel, false);
      }
    }

    void writeRgbChannel(final byte[] image,
                         final int width,
                         final int height,
                         final int rgbChannelCount,
                         final int rgbC,
                         final int bytesPerPixel,
                         final boolean littleEndian,
                         final boolean interleaved) throws Exception
    {
      for (int yy = 0; yy < height; ++yy) {
        for (int xx = 0; xx < width; ++xx) {
          final int index;
          if (interleaved) {
            index = ((yy * width + xx) * rgbChannelCount + rgbC) * bytesPerPixel;
          } else {
            index = ((rgbC * height + yy) * width + xx) * bytesPerPixel;
          }
          writeSample(image, index, bytesPerPixel, littleEndian);
        }
      }
    }

    private void writeSample(final byte[] image, final int index, final int bytesPerPixel, final boolean littleEndian)
      throws Exception
    {
      if (littleEndian || bytesPerPixel == 1) {
        writeBytes(image, index, bytesPerPixel);
      } else {
        for (int b = bytesPerPixel - 1; b >= 0; --b) {
          writeByte(image[index + b]);
        }
      }
    }

    private void writeBytes(final byte[] data, int offset, int count) throws Exception
    {
      while (count > 0) {
        final int writable = Math.min(count, chunkBytes - buffer.size());
        buffer.write(data, offset, writable);
        offset += writable;
        count -= writable;
        totalBytes += writable;
        if (buffer.size() == chunkBytes) {
          flush(false);
        }
      }
    }

    private void writeByte(final byte value) throws Exception
    {
      buffer.write(value);
      ++totalBytes;
      if (buffer.size() == chunkBytes) {
        flush(false);
      }
    }

    void finish() throws Exception
    {
      flush(true);
    }

    private void flush(final boolean finalChunk) throws Exception
    {
      if (buffer.size() == 0 && !finalChunk) {
        return;
      }
      final PixelChunk chunk = PixelChunk.newBuilder()
                                 .setSequenceIndex(sequenceIndex++)
                                 .setFinalChunk(finalChunk)
                                 .setTotalBytes(totalBytes)
                                 .setData(ByteString.copyFrom(buffer.toByteArray()))
                                 .build();
      buffer.reset();
      sendOk(sink, requestId, Response.newBuilder().setPixelChunk(chunk));
    }
  }

  private final class ThumbnailChunkWriter
  {
    private final ResponseSink sink;
    private final long requestId;
    private final int width;
    private final int height;
    private final int channelCount;
    private final int bytesPerPixel;
    private final String pixelType;
    private final int chunkBytes;
    private final ByteArrayOutputStream buffer;
    private int sequenceIndex = 0;
    private long totalBytes = 0;

    ThumbnailChunkWriter(final ResponseSink sink,
                         final long requestId,
                         final int width,
                         final int height,
                         final int channelCount,
                         final int bytesPerPixel,
                         final String pixelType,
                         final int chunkBytes)
    {
      this.sink = sink;
      this.requestId = requestId;
      this.width = width;
      this.height = height;
      this.channelCount = channelCount;
      this.bytesPerPixel = bytesPerPixel;
      this.pixelType = pixelType;
      this.chunkBytes = chunkBytes;
      this.buffer = new ByteArrayOutputStream(chunkBytes);
    }

    void writePlane(final byte[] image, final int bytesPerPixel, final boolean littleEndian) throws Exception
    {
      if (littleEndian || bytesPerPixel == 1) {
        writeBytes(image, 0, image.length);
        return;
      }
      for (int i = 0; i < image.length; i += bytesPerPixel) {
        writeSample(image, i, bytesPerPixel, false);
      }
    }

    void writeRgbChannel(final byte[] image,
                         final int width,
                         final int height,
                         final int rgbChannelCount,
                         final int rgbC,
                         final int bytesPerPixel,
                         final boolean littleEndian,
                         final boolean interleaved) throws Exception
    {
      for (int yy = 0; yy < height; ++yy) {
        for (int xx = 0; xx < width; ++xx) {
          final int index;
          if (interleaved) {
            index = ((yy * width + xx) * rgbChannelCount + rgbC) * bytesPerPixel;
          } else {
            index = ((rgbC * height + yy) * width + xx) * bytesPerPixel;
          }
          writeSample(image, index, bytesPerPixel, littleEndian);
        }
      }
    }

    private void writeSample(final byte[] image, final int index, final int bytesPerPixel, final boolean littleEndian)
      throws Exception
    {
      if (littleEndian || bytesPerPixel == 1) {
        writeBytes(image, index, bytesPerPixel);
      } else {
        for (int b = bytesPerPixel - 1; b >= 0; --b) {
          writeByte(image[index + b]);
        }
      }
    }

    private void writeBytes(final byte[] data, int offset, int count) throws Exception
    {
      while (count > 0) {
        final int writable = Math.min(count, chunkBytes - buffer.size());
        buffer.write(data, offset, writable);
        offset += writable;
        count -= writable;
        totalBytes += writable;
        if (buffer.size() == chunkBytes) {
          flush(false);
        }
      }
    }

    private void writeByte(final byte value) throws Exception
    {
      buffer.write(value);
      ++totalBytes;
      if (buffer.size() == chunkBytes) {
        flush(false);
      }
    }

    void finish() throws Exception
    {
      flush(true);
    }

    private void flush(final boolean finalChunk) throws Exception
    {
      if (buffer.size() == 0 && !finalChunk) {
        return;
      }
      final ThumbnailChunk chunk = ThumbnailChunk.newBuilder()
                                     .setSequenceIndex(sequenceIndex++)
                                     .setFinalChunk(finalChunk)
                                     .setTotalBytes(totalBytes)
                                     .setWidth(width)
                                     .setHeight(height)
                                     .setChannelCount(channelCount)
                                     .setBytesPerPixel(bytesPerPixel)
                                     .setPixelType(pixelType)
                                     .setData(ByteString.copyFrom(buffer.toByteArray()))
                                     .build();
      buffer.reset();
      sendOk(sink, requestId, Response.newBuilder().setThumbnailChunk(chunk));
    }
  }

  private static final class BioFormatsGrpcService extends BioFormatsBridgeGrpc.BioFormatsBridgeImplBase
  {
    private final AtlasBioFormatsBridge bridge;
    private final Runnable shutdown;

    BioFormatsGrpcService(final AtlasBioFormatsBridge bridge, final Runnable shutdown)
    {
      this.bridge = bridge;
      this.shutdown = shutdown;
    }

    @Override public void execute(final Request request, final StreamObserver<Response> responseObserver)
    {
      boolean keepRunning = true;
      final ResponseSink sink = new ResponseSink() {
        @Override public void send(final Response response)
        {
          responseObserver.onNext(response);
        }

        @Override public int pixelChunkBytes()
        {
          return GRPC_PIXEL_CHUNK_BYTES;
        }
      };
      try {
        keepRunning = bridge.handleRequest(request, sink);
        responseObserver.onCompleted();
      }
      catch (final Throwable t) {
        try {
          bridge.sendError(sink, request.getRequestId(), statusFor(t), t.getMessage());
          responseObserver.onCompleted();
        }
        catch (final Throwable sendFailure) {
          responseObserver.onError(sendFailure);
        }
      }
      if (!keepRunning) {
        new Thread(shutdown, "atlas-bioformats-grpc-shutdown").start();
      }
    }
  }
}
