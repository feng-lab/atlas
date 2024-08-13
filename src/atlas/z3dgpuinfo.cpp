#include "z3dgpuinfo.h"

#include "z3dgl.h"
#include "zlog.h"
#include <QProcess>

namespace nim {

size_t getDedicatedVideoMemoryMB();

Z3DGpuInfo& Z3DGpuInfo::instance()
{
  static Z3DGpuInfo gpuInfo;
  return gpuInfo;
}

Z3DGpuInfo::Z3DGpuInfo()
{
  detectGpuInfo();
}

int Z3DGpuInfo::glslMajorVersion() const
{
  if (isSupported()) {
    return m_glslMajorVersion;
  }
  LOG(FATAL) << "Current GPU card not supported. This function call should not happen.";
  return -1;
}

int Z3DGpuInfo::glslMinorVersion() const
{
  if (isSupported()) {
    return m_glslMinorVersion;
  }
  LOG(FATAL) << "Current GPU card not supported. This function call should not happen.";
  return -1;
}

int Z3DGpuInfo::glslReleaseVersion() const
{
  if (isSupported()) {
    return m_glslReleaseVersion;
  }
  LOG(FATAL) << "Current GPU card not supported. This function call should not happen.";
  return -1;
}

Z3DGpuInfo::GpuVendor Z3DGpuInfo::gpuVendor() const
{
  return m_gpuVendor;
}

bool Z3DGpuInfo::isExtensionSupported(const QString& extension) const
{
  return m_glExtensionsString.contains(extension, Qt::CaseInsensitive);
}

QString Z3DGpuInfo::glVersionString() const
{
  return m_glVersionString;
}

QString Z3DGpuInfo::glVendorString() const
{
  return m_glVendorString;
}

QString Z3DGpuInfo::glRendererString() const
{
  return m_glRendererString;
}

QString Z3DGpuInfo::glShadingLanguageVersionString() const
{
  return m_glslVersionString;
}

void Z3DGpuInfo::getDataScaleForTexture(size_t width,
                                        size_t height,
                                        size_t depth,
                                        double& widthScale,
                                        double& heightScale,
                                        double& depthScale) const
{
  bool scaleZ = depth > std::pow(textureSizeLimit() * 1.0, 1 / 3.0);
  double scale = 1.0;
  auto dataSize = width * height * depth;
  if (dataSize > textureSizeLimit()) {
    if (scaleZ) {
      scale = std::pow((textureSizeLimit() * 1.0) / dataSize, 1 / 3.0);
    } else {
      scale = std::sqrt((textureSizeLimit() * 1.0) / dataSize);
    }
  }
  size_t resHeight = height * scale;
  size_t resWidth = width * scale;
  size_t resDepth = scaleZ ? (depth * scale) : double(depth);
  widthScale = scale;
  heightScale = scale;
  depthScale = scaleZ ? scale : 1.0;

  size_t maxTexSize = depth > 1 ? max3DTextureSize() : maxTextureSize();
  if (resHeight > maxTexSize) {
    heightScale *= static_cast<double>(maxTexSize) / resHeight;
  }
  if (resWidth > maxTexSize) {
    widthScale *= static_cast<double>(maxTexSize) / resWidth;
  }
  if (resDepth > maxTexSize) {
    depthScale *= static_cast<double>(maxTexSize) / resDepth;
  }
}

bool Z3DGpuInfo::needScaleDataForTexture(size_t width, size_t height, size_t depth) const
{
  double s1 = 1.0;
  double s2 = 1.0;
  double s3 = 1.0;
  getDataScaleForTexture(width, height, depth, s1, s2, s3);
  return s1 != 1.0 || s2 != 1.0 || s3 != 1.0;
}

QString Z3DGpuInfo::glExtensionsString() const
{
  return m_glExtensionsString;
}

bool Z3DGpuInfo::isTessellationShaderSupported() const
{
  return GLVersionGE(4, 0);
}

bool Z3DGpuInfo::isTextureFilterAnisotropicSupported() const
{
  return isExtensionSupported("GL_EXT_texture_filter_anisotropic");
}

bool Z3DGpuInfo::isImagingSupported() const
{
  return isExtensionSupported("GL_ARB_imaging");
}

void Z3DGpuInfo::logGpuInfo() const
{
  if (!isSupported()) {
    LOG(INFO) << "Current GPU card is not supported. Reason: " << m_notSupportedReason;
    LOG(INFO) << "3D functions will be disabled.";
    return;
  }

#ifdef __APPLE__
  QProcess dispInfo;
  dispInfo.start("system_profiler", QStringList() << "SPDisplaysDataType");

  if (dispInfo.waitForFinished(-1)) {
    LOG(INFO) << dispInfo.readAllStandardOutput();
  } else {
    LOG(INFO) << dispInfo.readAllStandardError();
  }
#endif

  LOG(INFO) << "OpenGL Vendor:                 " << m_glVendorString;
  LOG(INFO) << "OpenGL Renderer:               " << m_glRendererString;
  LOG(INFO) << "OpenGL Version:                " << m_glVersionString;
  LOG(INFO) << "OpenGL SL Version:             " << m_glslVersionString;
  LOG(INFO) << "Context Core Profile Bit: " << m_contextCoreProfileBit;
  LOG(INFO) << "Context Compatibility Profile Bit: " << m_contextCompatibilityProfileBit;
  LOG(INFO) << "Context Flag Forward Compatible Bit: " << m_contextFlagForwardCompatibleBit;
  LOG(INFO) << "Context Flag Debug Bit: " << m_contextFlagDebugBit;
  LOG(INFO) << "Context Flag Robust Access Bit: " << m_contextFlagRobustAccessBit;
  LOG(INFO) << "OpenGL Extensions: " << m_glExtensionsString;
  LOG(INFO) << "Max Viewport Dimensions:       " << m_maxViewportDims;
  LOG(INFO) << "Max Renderbuffer Size:         " << m_maxRenderbufferSize;
  LOG(INFO) << fmt::format("Max Texture Size:              {} (use {})", m_maxTexureSize, maxTextureSize());
  LOG(INFO) << fmt::format("Max 3D Texture Size:           {} (use {})", m_max3DTextureSize, max3DTextureSize());
  LOG(INFO) << "Max Color Attachments:         " << m_maxColorAttachments;
  LOG(INFO) << "Max Draw Buffer:               " << m_maxDrawBuffer;
  if (m_maxGeometryOutputVertices > 0) {
    LOG(INFO) << "Max GS Output Vertices:        " << m_maxGeometryOutputVertices;
  }
  LOG(INFO) << "Max VS Texture Image Units:    " << m_maxVertexTextureImageUnits;
  if (m_maxGeometryTextureImageUnits > 0) {
    LOG(INFO) << "Max GS Texture Image Units:    " << m_maxGeometryTextureImageUnits;
  }
  LOG(INFO) << "Max FS Texture Image Units:    " << m_maxTextureImageUnits;
  LOG(INFO) << "VS+GS+FS Texture Image Units:  " << m_maxCombinedTextureImageUnits;
  // LOG(INFO) << "Max Texture Coordinates:       " << m_maxTextureCoords;
  LOG(INFO) << "Max Array Texture Layers:      " << m_maxArrayTextureLayers;

  LOG(INFO) << fmt::format("Total Graphics Memory Size:    {} MB", dedicatedVideoMemoryMB());

  LOG(INFO) << fmt::format("Smooth Point Size Range:       ({}, {})", m_minSmoothPointSize, m_maxSmoothPointSize);
  LOG(INFO) << "Smooth Point Size Granularity: " << m_smoothPointSizeGranularity;
  // LOG(INFO) << fmt::format("Aliased Point Size Range:      ({}, {})", m_minAliasedPointSize, m_maxAliasedPointSize);

  LOG(INFO) << fmt::format("Smooth Line Width Range:       ({}, {})", m_minSmoothLineWidth, m_maxSmoothLineWidth);
  LOG(INFO) << "Smooth Line Width Granularity: " << m_smoothLineWidthGranularity;
  LOG(INFO) << fmt::format("Aliased Line Width Range:      ({}, {})", m_minAliasedLineWidth, m_maxAliasedLineWidth);

  LOG(INFO) << "Max Texture Buffer Size:       " << m_maxTextureBufferSize;

  LOG(INFO) << "";
}

bool Z3DGpuInfo::isLinkedListSupported() const
{
  return GLVersionGE(4, 2);
}

void Z3DGpuInfo::detectGpuInfo()
{
  // reinterpret_cast allowed (AliasedType is the (possibly cv-qualified) signed or unsigned variant of DynamicType)
  m_glVersionString = QString(reinterpret_cast<const char*>(glGetString(GL_VERSION)));
  m_glVendorString = QString(reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
  m_glRendererString = QString(reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

  GLint contextFlags;
  glGetIntegerv(GL_CONTEXT_FLAGS, &contextFlags);
  m_contextFlagForwardCompatibleBit = contextFlags & GLint(GL_CONTEXT_FLAG_FORWARD_COMPATIBLE_BIT);
  m_contextFlagDebugBit = contextFlags & GLint(GL_CONTEXT_FLAG_DEBUG_BIT);
  m_contextFlagRobustAccessBit = contextFlags & GLint(GL_CONTEXT_FLAG_ROBUST_ACCESS_BIT);

  GLint contextProfileMask;
  glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &contextProfileMask);
  m_contextCoreProfileBit = contextProfileMask & GLint(GL_CONTEXT_CORE_PROFILE_BIT);
  m_contextCompatibilityProfileBit = contextProfileMask & GLint(GL_CONTEXT_COMPATIBILITY_PROFILE_BIT);

  if (!m_contextFlagForwardCompatibleBit) {
    m_glExtensionsString = QString(reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS)));
  }

  if (GLVersionGE(3, 3)) {
    m_isSupported = true;

    // Prevent segfault
    const char* glslVS = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
    m_glslVersionString = glslVS ? QString(glslVS) : "";

    if (!parseVersionString(m_glVersionString, m_glMajorVersion, m_glMinorVersion, m_glReleaseVersion)) {
      LOG(ERROR) << "Malformed OpenGL version string: " << m_glVersionString;
    }

    // GPU Vendor
    if (m_glVendorString.contains("NVIDIA", Qt::CaseInsensitive)) {
      m_gpuVendor = GpuVendor::NVIDIA;
    } else if (m_glVendorString.contains("ATI", Qt::CaseInsensitive)) {
      m_gpuVendor = GpuVendor::AMD;
    } else if (m_glVendorString.contains("INTEL", Qt::CaseInsensitive)) {
      m_gpuVendor = GpuVendor::INTEL;
    } else {
      m_gpuVendor = GpuVendor::UNKNOWN;
    }

    // Shaders
    if (!parseVersionString(m_glslVersionString, m_glslMajorVersion, m_glslMinorVersion, m_glslReleaseVersion)) {
      LOG(ERROR) << "Malformed GLSL version string: " << m_glslVersionString;
      m_isSupported = false;
      m_notSupportedReason = QString("Malformed GLSL version string: %1").arg(m_glslVersionString);
    }

    m_maxGeometryOutputVertices = -1;
    glGetIntegerv(GL_MAX_GEOMETRY_OUTPUT_VERTICES_EXT, &m_maxGeometryOutputVertices);

    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &m_maxArrayTextureLayers);

    glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &m_maxTextureBufferSize);

    //
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, &m_maxViewportDims);
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &m_maxRenderbufferSize);
    // Texturing
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_maxTexureSize);
    glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &m_max3DTextureSize);
    // http://www.opengl.org/wiki/Textures_-_more
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &m_maxTextureImageUnits);
    glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &m_maxVertexTextureImageUnits);

    m_maxGeometryTextureImageUnits = -1;
    glGetIntegerv(GL_MAX_GEOMETRY_TEXTURE_IMAGE_UNITS, &m_maxGeometryTextureImageUnits);

    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &m_maxCombinedTextureImageUnits);

    if (isTextureFilterAnisotropicSupported()) {
      glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &m_maxTextureAnisotropy);
    } else {
      m_maxTextureAnisotropy = 1.0;
    }

    glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS_EXT, &m_maxColorAttachments);
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &m_maxDrawBuffer);

    // Point
    GLfloat range[2];
    glGetFloatv(GL_SMOOTH_POINT_SIZE_RANGE, range);
    glGetFloatv(GL_SMOOTH_POINT_SIZE_GRANULARITY, &m_smoothPointSizeGranularity);
    m_minSmoothPointSize = range[0];
    m_maxSmoothPointSize = range[1];
    // can not find in opengl doc
    // glGetFloatv(GL_ALIASED_POINT_SIZE_RANGE, range);
    // m_minAliasedPointSize = range[0];
    // m_maxAliasedPointSize = range[1];

    // Line
    glGetFloatv(GL_SMOOTH_LINE_WIDTH_RANGE, range);
    glGetFloatv(GL_SMOOTH_LINE_WIDTH_GRANULARITY, &m_smoothLineWidthGranularity);
    m_minSmoothLineWidth = range[0];
    m_maxSmoothLineWidth = range[1];
    glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, range);
    m_minAliasedLineWidth = range[0];
    m_maxAliasedLineWidth = range[1];

    detectDedicatedVideoMemory();
  } else {
    m_isSupported = false;
    m_notSupportedReason =
      "Minimum OpenGL version required is 3.3, while current openGL version is: \"" + m_glVersionString + "\"";
  }
}

// format "2.1[.1] otherstring"
bool Z3DGpuInfo::parseVersionString(const QString& versionString, int& major, int& minor, int& release)
{
  major = -1;
  minor = -1;
  release = -1;

  if (versionString.isEmpty()) {
    return false;
  }

  QString str = versionString.mid(0, versionString.indexOf(" "));
  QStringList list = str.split(".");
  if (list.size() < 2 || list.size() > 3) {
    return false;
  }

  bool ok;
  major = list[0].toInt(&ok);
  if (!ok) {
    major = -1;
    return false;
  }

  minor = list[1].toInt(&ok);
  if (!ok) {
    major = -1;
    minor = -1;
    return false;
  }

  if (list.size() > 2) {
    release = list[2].toInt(&ok);
    if (!ok) {
      major = -1;
      minor = -1;
      release = -1;
      return false;
    }
  } else {
    release = 0;
  }

  return true;
}

void Z3DGpuInfo::detectDedicatedVideoMemory()
{
  m_dedicatedVideoMemoryMB = 0;
  if (m_gpuVendor == GpuVendor::NVIDIA) {
    if (isExtensionSupported("GL_NVX_gpu_memory_info")) {
      int retVal;
      // glGetIntegerv(GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &retVal);
      glGetIntegerv(GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, &retVal);
      m_dedicatedVideoMemoryMB = retVal / 1024;
    }
  } else if (m_gpuVendor == GpuVendor::AMD) {
    if (isExtensionSupported("GL_ATI_meminfo")) {
      int retVal[4];
      glGetIntegerv(GL_TEXTURE_FREE_MEMORY_ATI, retVal);
      m_dedicatedVideoMemoryMB = retVal[0] / 1024;
    }
  }
  if (m_dedicatedVideoMemoryMB == 0) {
    m_dedicatedVideoMemoryMB = getDedicatedVideoMemoryMB();
  }
  if (m_dedicatedVideoMemoryMB == 0) {
    LOG(ERROR) << "Can not detect dedicated video memory, use 256";
    m_dedicatedVideoMemoryMB = 256;
  }
}

} // namespace nim
