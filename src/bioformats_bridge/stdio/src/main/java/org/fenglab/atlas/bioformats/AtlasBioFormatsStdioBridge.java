package org.fenglab.atlas.bioformats;

import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.Request;
import org.fenglab.atlas.bioformats.proto.BioFormatsBridgeProto.Response;
import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.EOFException;
import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public final class AtlasBioFormatsStdioBridge
{
  private static final int READER_COUNT = 1;

  private AtlasBioFormatsStdioBridge() {}

  public static void main(final String[] args) throws Exception
  {
    configureLogging();
    if (args.length != 0) {
      throw new IllegalArgumentException("stdio bridge does not accept command-line arguments");
    }

    final AtlasBioFormatsBridgeCore bridge = new AtlasBioFormatsBridgeCore(READER_COUNT);
    final OutputStream protocolOut = new FileOutputStream(FileDescriptor.out);
    System.setOut(System.err);
    try {
      run(bridge, System.in, protocolOut);
    }
    finally {
      bridge.closeAllSessions();
    }
  }

  private static void configureLogging()
  {
    System.setProperty("org.slf4j.simpleLogger.logFile", "System.err");
    System.setProperty("org.slf4j.simpleLogger.defaultLogLevel", "warn");
  }

  private static void run(final AtlasBioFormatsBridgeCore bridge, final InputStream input, final OutputStream output)
    throws Exception
  {
    final BufferedInputStream in = new BufferedInputStream(input);
    final BufferedOutputStream out = new BufferedOutputStream(output);
    final AtlasBioFormatsBridgeCore.ResponseSink sink = response -> writeResponse(out, response);

    boolean running = true;
    while (running) {
      final Request request;
      try {
        request = readRequest(in);
      }
      catch (final EOFException eof) {
        break;
      }

      try {
        running = bridge.handleRequest(request, sink);
      }
      catch (final Throwable t) {
        bridge.sendError(sink, request.getRequestId(), AtlasBioFormatsBridgeCore.statusFor(t), t.getMessage());
      }
    }
  }

  private static Request readRequest(final InputStream input) throws IOException
  {
    final int size = readLittleEndianInt(input);
    if (size < 0 || size > AtlasBioFormatsBridgeCore.MAX_FRAME_BYTES) {
      throw new IOException("invalid request frame size: " + size);
    }
    final byte[] data = input.readNBytes(size);
    if (data.length != size) {
      throw new EOFException("truncated request frame");
    }
    return Request.parseFrom(data);
  }

  private static int readLittleEndianInt(final InputStream input) throws IOException
  {
    final int b0 = input.read();
    if (b0 < 0) {
      throw new EOFException();
    }
    final int b1 = input.read();
    final int b2 = input.read();
    final int b3 = input.read();
    if ((b1 | b2 | b3) < 0) {
      throw new EOFException();
    }
    return (b0 & 0xff) | ((b1 & 0xff) << 8) | ((b2 & 0xff) << 16) | ((b3 & 0xff) << 24);
  }

  private static void writeResponse(final OutputStream output, final Response response) throws IOException
  {
    final byte[] data = response.toByteArray();
    writeLittleEndianInt(output, data.length);
    output.write(data);
    output.flush();
  }

  private static void writeLittleEndianInt(final OutputStream output, final int value) throws IOException
  {
    output.write(value & 0xff);
    output.write((value >>> 8) & 0xff);
    output.write((value >>> 16) & 0xff);
    output.write((value >>> 24) & 0xff);
  }
}
