package org.fenglab.atlas.bioformats;

import io.grpc.Server;
import io.grpc.netty.shaded.io.grpc.netty.NettyServerBuilder;
import io.grpc.stub.StreamObserver;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeGrpc;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.Request;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.Response;
import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicReference;

public final class AtlasBioFormatsGrpcBridge
{
  private AtlasBioFormatsGrpcBridge() {}

  public static void main(final String[] args) throws Exception
  {
    configureLogging();
    final OutputStream startupOut = new FileOutputStream(FileDescriptor.out);
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
    if (grpcPort < 0) {
      throw new IllegalArgumentException("--grpc-port is required for the gRPC bridge");
    }

    runGrpcServer(grpcPort, workerCount, startupOut);
  }

  private static void configureLogging()
  {
    System.setProperty("org.slf4j.simpleLogger.logFile", "System.err");
    System.setProperty("org.slf4j.simpleLogger.defaultLogLevel", "warn");
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
    final AtlasBioFormatsBridgeCore bridge = new AtlasBioFormatsBridgeCore(workerCount);
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
                            .maxInboundMessageSize(AtlasBioFormatsBridgeCore.MAX_FRAME_BYTES)
                            .addService(new BioFormatsGrpcService(bridge, shutdown))
                            .build()
                            .start();
    serverRef.set(server);
    startupOut.write(("ATLAS_BIOFORMATS_GRPC_PORT=" + server.getPort() + "\n").getBytes(StandardCharsets.US_ASCII));
    startupOut.flush();
    server.awaitTermination();
    bridge.closeAllSessions();
  }

  private static final class BioFormatsGrpcService extends BioFormatsBridgeGrpc.BioFormatsBridgeImplBase
  {
    private final AtlasBioFormatsBridgeCore bridge;
    private final Runnable shutdown;

    BioFormatsGrpcService(final AtlasBioFormatsBridgeCore bridge, final Runnable shutdown)
    {
      this.bridge = bridge;
      this.shutdown = shutdown;
    }

    @Override public void execute(final Request request, final StreamObserver<Response> responseObserver)
    {
      boolean keepRunning = true;
      final AtlasBioFormatsBridgeCore.ResponseSink sink = new AtlasBioFormatsBridgeCore.ResponseSink() {
        @Override public void send(final Response response)
        {
          responseObserver.onNext(response);
        }

        @Override public int pixelChunkBytes()
        {
          return AtlasBioFormatsBridgeCore.GRPC_PIXEL_CHUNK_BYTES;
        }
      };
      try {
        keepRunning = bridge.handleRequest(request, sink);
        responseObserver.onCompleted();
      }
      catch (final Throwable t) {
        try {
          bridge.sendError(sink, request.getRequestId(), AtlasBioFormatsBridgeCore.statusFor(t), t.getMessage());
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
