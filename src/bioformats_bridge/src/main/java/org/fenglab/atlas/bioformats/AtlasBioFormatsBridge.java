package org.fenglab.atlas.bioformats;

import com.google.protobuf.ByteString;
import io.grpc.Server;
import io.grpc.netty.shaded.io.grpc.netty.NettyServerBuilder;
import io.grpc.stub.StreamObserver;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeGrpc;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.CloseDatasetResponse;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.FileGroupingPolicy;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.ListFormatsResponse;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.MetadataEntry;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.OpenDatasetResponse;
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
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Hashtable;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Map.Entry;
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
  private static final int GRPC_PIXEL_CHUNK_BYTES = 1024 * 1024;
  private static final int PROTOCOL_VERSION = 1;

  private final InputStream in;
  private final OutputStream out;
  private final int workerCount;
  private final Object sessionsLock = new Object();
  private final Map<Long, ReaderSession> sessions = new LinkedHashMap<>();
  private final AtomicLong nextSessionId = new AtomicLong(1);

  private AtlasBioFormatsBridge(final InputStream in, final OutputStream out, final int workerCount)
  {
    if (workerCount <= 0) {
      throw new IllegalArgumentException("worker count must be positive");
    }
    this.in = in == null ? null : new BufferedInputStream(in);
    this.out = out == null ? null : new BufferedOutputStream(out);
    this.workerCount = workerCount;
  }

  private AtlasBioFormatsBridge(final int workerCount)
  {
    this(null, null, workerCount);
  }

  public static void main(final String[] args) throws Exception
  {
    final OutputStream protocolOut = new FileOutputStream(FileDescriptor.out);
    System.setProperty("org.slf4j.simpleLogger.logFile", "System.err");
    System.setProperty("org.slf4j.simpleLogger.defaultLogLevel", "warn");
    System.setOut(System.err);

    int grpcPort = -1;
    int workerCount = 1;
    for (final String arg : args) {
      if (arg.startsWith("--grpc-port=")) {
        grpcPort = parseNonNegativeIntArgument(arg, "--grpc-port=");
      } else if (arg.startsWith("--worker-count=")) {
        workerCount = parsePositiveIntArgument(arg, "--worker-count=");
      } else if (!arg.isEmpty()) {
        throw new IllegalArgumentException("unknown argument: " + arg);
      }
    }

    if (grpcPort >= 0) {
      runGrpcServer(grpcPort, workerCount, protocolOut);
      return;
    }

    final AtlasBioFormatsBridge bridge = new AtlasBioFormatsBridge(System.in, protocolOut, workerCount);
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

  private static int parsePositiveIntArgument(final String arg, final String prefix)
  {
    final int value = Integer.parseInt(arg.substring(prefix.length()));
    if (value <= 0) {
      throw new IllegalArgumentException(prefix + " must be > 0");
    }
    return value;
  }

  private static void runGrpcServer(final int grpcPort, final int workerCount, final OutputStream startupOut)
    throws Exception
  {
    final AtlasBioFormatsBridge bridge = new AtlasBioFormatsBridge(workerCount);
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
    server.awaitTermination();
    bridge.closeAllSessions();
  }

  private interface ResponseSink
  {
    void send(Response response) throws Exception;

    default int pixelChunkBytes()
    {
      return PIXEL_CHUNK_BYTES;
    }
  }

  private void run() throws Exception
  {
    if (in == null || out == null) {
      throw new IllegalStateException("stdio bridge was not initialized with streams");
    }
    final ResponseSink sink = this::writeResponse;
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
    closeAllSessions();
  }

  private boolean handleRequest(final Request request, final ResponseSink sink) throws Exception
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
      case OPEN_DATASET:
        sendOk(sink, request.getRequestId(), Response.newBuilder().setOpenDataset(openDataset(request)));
        return true;
      case READ_REGION:
        readRegion(request, sink);
        return true;
      case READ_THUMBNAIL:
        readThumbnail(request, sink);
        return true;
      case CLOSE_DATASET:
        closeDataset(request.getCloseDataset().getSessionId());
        sendOk(sink, request.getRequestId(), Response.newBuilder().setCloseDataset(CloseDatasetResponse.newBuilder()));
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

  private OpenDatasetResponse openDataset(final Request request) throws FormatException, IOException
  {
    final String path = request.getOpenDataset().getPath();
    requireExistingFile(path);
    final ArrayList<IFormatReader> readers = new ArrayList<>(workerCount);
    boolean success = false;
    try {
      for (int i = 0; i < workerCount; ++i) {
        final IFormatReader reader =
          createReader(request.getOpenDataset().getGroupingPolicy(), request.getOpenDataset().getMetadataFiltered());
        readers.add(reader);
        reader.setId(path);
        configureReaderAfterOpen(reader);
      }
      final IFormatReader metadataReader = readers.get(0);
      final long sessionId = nextSessionId.getAndIncrement();
      final OpenDatasetResponse.Builder response = OpenDatasetResponse.newBuilder()
                                                     .setSessionId(sessionId)
                                                     .setPath(path)
                                                     .setFormatName(nullToEmpty(metadataReader.getFormat()))
                                                     .setReaderClass(metadataReader.getClass().getName())
                                                     .addAllUsedFiles(nonNullArray(metadataReader.getUsedFiles()));
      for (int s = 0; s < metadataReader.getSeriesCount(); ++s) {
        response.addSeries(seriesInfo(metadataReader, s));
      }
      synchronized (sessionsLock) {
        sessions.put(sessionId, new ReaderSession(sessionId, path, readers));
      }
      success = true;
      return response.build();
    }
    finally {
      if (!success) {
        for (final IFormatReader reader : readers) {
          reader.close();
        }
      }
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
    final java.util.ArrayList<Integer> colors = new java.util.ArrayList<>();
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
    final ReaderSession session = sessionFor(request.getReadRegion().getSessionId());
    final IFormatReader reader = session.readReader();
    synchronized (reader) {
      final int series = toInt("series", request.getReadRegion().getSeries());
      final int resolution = toInt("resolution", request.getReadRegion().getResolution());
      final int x = toInt("x", request.getReadRegion().getX());
      final int y = toInt("y", request.getReadRegion().getY());
      final int z0 = toInt("z", request.getReadRegion().getZ());
      final int c0 = toInt("c", request.getReadRegion().getC());
      final int t0 = toInt("t", request.getReadRegion().getT());
      final int width = toPositiveInt("width", request.getReadRegion().getWidth());
      final int height = toPositiveInt("height", request.getReadRegion().getHeight());
      final int depth = toPositiveInt("depth", request.getReadRegion().getDepth());
      final int channelCount = toPositiveInt("channel_count", request.getReadRegion().getChannelCount());
      final int timeCount = toPositiveInt("time_count", request.getReadRegion().getTimeCount());

      reader.setSeries(series);
      reader.setResolution(resolution);
      validateRegion(reader, x, y, z0, c0, t0, width, height, depth, channelCount, timeCount);

      final ChunkWriter chunks = new ChunkWriter(sink, request.getRequestId(), sink.pixelChunkBytes());
      final int rgbChannelCount = Math.max(1, reader.getRGBChannelCount());
      final int bytesPerPixel = FormatTools.getBytesPerPixel(reader.getPixelType());
      final boolean littleEndian = reader.isLittleEndian();
      final boolean interleaved = reader.isInterleaved();
      for (int t = t0; t < t0 + timeCount; ++t) {
        for (int atlasC = c0; atlasC < c0 + channelCount; ++atlasC) {
          final int bioC = atlasC / rgbChannelCount;
          final int rgbC = atlasC % rgbChannelCount;
          for (int z = z0; z < z0 + depth; ++z) {
            final int plane = reader.getIndex(z, bioC, t);
            final byte[] image = reader.openBytes(plane, x, y, width, height);
            if (rgbChannelCount == 1) {
              chunks.writePlane(image, bytesPerPixel, littleEndian);
            } else {
              chunks
                .writeRgbChannel(image, width, height, rgbChannelCount, rgbC, bytesPerPixel, littleEndian, interleaved);
            }
          }
        }
      }
      chunks.finish();
    }
  }

  private void readThumbnail(final Request request, final ResponseSink sink) throws Exception
  {
    final ReaderSession session = sessionFor(request.getReadThumbnail().getSessionId());
    final IFormatReader reader = session.primaryReader();
    synchronized (reader) {
      final int series = toInt("series", request.getReadThumbnail().getSeries());
      final int z = toInt("z", request.getReadThumbnail().getZ());
      final int t = toInt("t", request.getReadThumbnail().getT());

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
    }
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

  private void closeDataset(final long sessionId) throws IOException
  {
    final ReaderSession session;
    synchronized (sessionsLock) {
      session = sessions.remove(sessionId);
    }
    if (session != null) {
      session.close();
    }
  }

  private void closeAllSessions()
  {
    final ArrayList<ReaderSession> toClose;
    synchronized (sessionsLock) {
      toClose = new ArrayList<>(sessions.values());
      sessions.clear();
    }
    for (final ReaderSession session : toClose) {
      try {
        session.close();
      }
      catch (final IOException ignored) {
        // best-effort process shutdown
      }
    }
  }

  private ReaderSession sessionFor(final long sessionId)
  {
    synchronized (sessionsLock) {
      final ReaderSession session = sessions.get(sessionId);
      if (session != null) {
        return session;
      }
    }
    throw new IllegalArgumentException("unknown Bio-Formats session id: " + sessionId);
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

  private static void configureReaderAfterOpen(final IFormatReader reader)
  {
    final IFormatReader activeReader = reader instanceof ImageReader ? ((ImageReader)reader).getReader() : reader;
    if (activeReader instanceof SDTReader) {
      // SDTReader's default preload cache can retain hundreds of MB for one requested plane. Atlas reads SDT data as
      // small regions through multiple long-lived bridge readers, so keep SDT reads streaming instead of retaining that
      // per-reader cache.
      ((SDTReader)activeReader).setPreLoad(false);
    }
  }

  private static void requireExistingFile(final String path) throws FileNotFoundException
  {
    if (path == null || path.isEmpty()) {
      throw new FileNotFoundException("empty path");
    }
    if (!new File(path).exists()) {
      throw new FileNotFoundException(path);
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
    final java.util.ArrayList<String> result = new java.util.ArrayList<>();
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

  private static final class ReaderSession
  {
    final long id;
    final String path;
    final List<IFormatReader> readers;
    final AtomicLong nextReadReader = new AtomicLong(0);

    ReaderSession(final long id, final String path, final List<IFormatReader> readers)
    {
      if (readers.isEmpty()) {
        throw new IllegalArgumentException("reader session requires at least one reader");
      }
      this.id = id;
      this.path = path;
      this.readers = readers;
    }

    IFormatReader primaryReader()
    {
      return readers.get(0);
    }

    IFormatReader readReader()
    {
      final long rawIndex = nextReadReader.getAndIncrement();
      final int index = Math.floorMod(rawIndex, readers.size());
      return readers.get(index);
    }

    void close() throws IOException
    {
      for (final IFormatReader reader : readers) {
        synchronized (reader) {
          reader.close();
        }
      }
    }
  }
}
