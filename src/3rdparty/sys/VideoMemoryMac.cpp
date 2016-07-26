#include <CGLRenderers.h>
#include <CGLTypes.h>
#include <CGLCurrent.h>
#include <OpenGL.h>
#include <iostream>
#include "zlog.h"

uint64_t getDedicatedVideoMemoryMB()
{
  uint64_t res = 0;
  // Get renderer info for all renderers that match the display mask.
  GLint i, nrend = 0;
  CGLRendererInfoObj rend;
  // Using a -1/0xFFFFFFFF display mask enables us to find all renderers,
  // including those GPUs that are not attached to monitors, aka. offline renderers.
  const GLint displayMask = 0xFFFFFFFF;

  CGLQueryRendererInfo((GLuint)displayMask, &rend, &nrend);

  for (i = 0; i < nrend; i++) {
    GLint thisRendererID;
    GLint videoMemory = 0;
    GLint online = false;

    CGLDescribeRenderer(rend, i, kCGLRPRendererID, &thisRendererID);
    CGLDescribeRenderer(rend, i, kCGLRPOnline, &online);
    LINFO() << "Renderer ID = " << thisRendererID << " online = " << online;

    CGLDescribeRenderer(rend, i, kCGLRPVideoMemoryMegabytes, &videoMemory);
    LINFO() << "Video Memory = " << videoMemory << " MB";

    if (online)
      res = std::max<uint64_t>(res, videoMemory);
  }
  CGLDestroyRendererInfo(rend);
  return res;
}
