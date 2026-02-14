#pragma once

#include "zglobal.h"
#include "z3dtypes.h"
#include <QString>

// This class provides information about the GPU
// If the openGL version is too low or certain critical extensions are not supported,
// isSupported() will return false, and notSupportedReason() will return the reason.
// If isSupported() return false, other functions (except gl*Version functions
// which will still be correct.) will return uninitialized values.

namespace nim {

class Z3DGpuInfo
{
public:
  static Z3DGpuInfo& instance();

  // Source backend for populated caps (OpenGL or Vulkan)
  RenderBackend capsBackend() const { return m_capsBackend; }

  enum class GpuVendor
  {
    NVIDIA,
    AMD,
    INTEL,
    UNKNOWN
  };

  Z3DGpuInfo();

  // Initialize caps by querying the current OpenGL context. Safe to call
  // multiple times; subsequent calls are no-ops once OpenGL caps are set.
  void initializeFromOpenGL();

  struct GenericCaps
  {
    uint32_t maxTextureSize = 8192;          // 2D max dimension
    uint32_t max3DTextureSize = 2048;        // 3D max dimension
    int maxArrayTextureLayers = 256;         // array layers
    int maxColorAttachments = 4;             // FBO color attachments
    float maxTextureAnisotropy = 1.f;        // sampler anisotropy
    uint64_t dedicatedVideoMemoryMB = 256;   // VRAM size in MB (approx)

    // The remaining fields are primarily consumed by GL paths; provide
    // conservative defaults when sourcing from non-GL backends.
    int maxCombinedTextureImageUnits = 48;   // VS+GS+FS combined
    int maxTextureImageUnits = 16;           // FS units
    int maxVertexTextureImageUnits = 16;     // VS units
    int maxGeometryTextureImageUnits = 16;   // GS units
    int maxTextureBufferSize = 64 * 1024 * 1024; // texel buffer elements (approx)
    int maxDrawBuffer = 8;                   // draw buffers (GL only)
    int maxViewportDim = 16384;              // min of viewport dims
  };

  // Override generic caps from a non-GL backend (e.g., Vulkan). This sets the
  // source to Vulkan and marks the info as supported so shared code paths can
  // use size/limit queries safely even when GL is not initialized.
  void overrideGenericCaps(const GenericCaps& caps);

  [[nodiscard]] bool isSupported() const
  {
    return m_isSupported;
  }

  [[nodiscard]] QString notSupportedReason() const
  {
    return m_notSupportedReason;
  }

  [[nodiscard]] int glMajorVersion() const
  {
    return m_glMajorVersion;
  }

  [[nodiscard]] int glMinorVersion() const
  {
    return m_glMinorVersion;
  }

  [[nodiscard]] int glReleaseVersion() const
  {
    return m_glReleaseVersion;
  }

  [[nodiscard]] int glslMajorVersion() const;

  [[nodiscard]] int glslMinorVersion() const;

  [[nodiscard]] int glslReleaseVersion() const;

  [[nodiscard]] GpuVendor gpuVendor() const;

  [[nodiscard]] bool isExtensionSupported(const QString& extension) const;

  [[nodiscard]] QString glVersionString() const;

  [[nodiscard]] QString glVendorString() const;

  [[nodiscard]] QString glRendererString() const;

  [[nodiscard]] QString glExtensionsString() const;

  [[nodiscard]] QString glShadingLanguageVersionString() const;

  // directX 10 resource limit
  // 1D 8192 2D 8192 3D 2048
  // directX 11 resource limit
  // 1D 16384 2D 16384 3D 2048
  [[nodiscard]] uint32_t maxTextureSize() const
  {
    return std::min(m_maxViewportDims, std::min(16384, m_maxTexureSize));
  }

  // Returns the maximal side length of 3D textures.
  // directx limit?
  [[nodiscard]] uint32_t max3DTextureSize() const
  {
    return std::min(2048, m_max3DTextureSize);
  }

  // Return a value such as 16 or 32. That is the number of image samplers that your GPU supports in the fragment
  // shader. the maximum supported texture image units that can be used to access texture maps from the fragment shader.
  // The value must be at least 16
  [[nodiscard]] int maxTextureImageUnits() const
  {
    return m_maxTextureImageUnits;
  }

  // The following is for the vertex shader (available since GL 2.0). This might return 0 for certain GPUs.
  // the maximum supported texture image units that can be used to access texture maps from the vertex shader.
  // The value may be at least 16.
  [[nodiscard]] int maxVertexTextureImageUnits() const
  {
    return m_maxVertexTextureImageUnits;
  }

  // The following is for the geometry shader (available since GL 3.2)
  // the maximum supported texture image units that can be used to access texture maps from the geometry shader.
  // The value must be at least 16.
  [[nodiscard]] int maxGeometryTextureImageUnits() const
  {
    return m_maxGeometryTextureImageUnits;
  }

  // The following is VS + GS + FS (available since GL 2.0)
  // the maximum supported texture image units that can be used to access texture maps from the vertex shader and the
  // fragment processor combined. If both the vertex shader and the fragment processing stage access the same texture
  // image unit, then that counts as using two texture image units against this limit. The value must be at least 48
  [[nodiscard]] int maxCombinedTextureImageUnits() const
  {
    return m_maxCombinedTextureImageUnits;
  }

  // and the following is the number of texture coordinates available which usually is 8
  // int maxTextureCoordinates() const { return m_maxTextureCoords; }
  // The value indicates the maximum number of layers allowed in an array texture, and must be at least 256.
  [[nodiscard]] int maxArrayTextureLayers() const
  {
    return m_maxArrayTextureLayers;
  }

  [[nodiscard]] size_t dedicatedVideoMemoryMB() const
  {
    return m_dedicatedVideoMemoryMB;
  }

  // directX 10 resource limit
  // 128 MB
  // directX 11 resource limit
  // min(max(128, 0.25f * (amount of dedicated VRAM)), 2048) MB
  // D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A_TERM (128)
  // D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_B_TERM (0.25f)
  // D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM (2048)
  [[nodiscard]] uint64_t textureSizeLimit() const
  {
    return std::min(std::max<uint64_t>(128, 0.25 * dedicatedVideoMemoryMB()), 2048_u64) * 1024 * 1024 / 2;
  }

  // get the required scales to fit uint8_t data of size (width, height, depth) to texture limit
  void getDataScaleForTexture(size_t width,
                              size_t height,
                              size_t depth,
                              double& widthScale,
                              double& heightScale,
                              double& depthScale) const;

  [[nodiscard]] bool needScaleDataForTexture(size_t width, size_t height, size_t depth) const;

  [[nodiscard]] bool isTessellationShaderSupported() const;

  [[nodiscard]] bool isTextureFilterAnisotropicSupported() const;

  // for glBlendEquation
  [[nodiscard]] bool isImagingSupported() const;

  [[nodiscard]] float maxTextureAnisotropy() const
  {
    return m_maxTextureAnisotropy;
  }

  // Returns the maximal number of color attachments for a FBO
  [[nodiscard]] int maxColorAttachments() const
  {
    return m_maxColorAttachments;
  }

  [[nodiscard]] int maxDrawBuffer() const
  {
    return m_maxDrawBuffer;
  }

  // Max number of clip distances supported in vertex shaders (OpenGL
  // GL_MAX_CLIP_DISTANCES). This bounds gl_ClipDistance[] size and how many
  // clip planes can be active at once.
  [[nodiscard]] int maxClipDistances() const
  {
    return m_maxClipDistances;
  }

  [[nodiscard]] float minSmoothPointSize() const
  {
    return m_minSmoothPointSize;
  }

  [[nodiscard]] float maxSmoothPointSize() const
  {
    return m_maxSmoothPointSize;
  }

  [[nodiscard]] float smoothPointSizeGranularity() const
  {
    return m_smoothPointSizeGranularity;
  }
  // float minAliasedPointSize() const { return m_minAliasedPointSize; }
  // float maxAliasedPointSize() const { return m_maxAliasedPointSize; }

  [[nodiscard]] float minSmoothLineWidth() const
  {
    return m_minSmoothLineWidth;
  }

  [[nodiscard]] float maxSmoothLineWidth() const
  {
    return m_maxSmoothLineWidth;
  }

  [[nodiscard]] float smoothLineWidthGranularity() const
  {
    return m_smoothLineWidthGranularity;
  }

  [[nodiscard]] float minAliasedLineWidth() const
  {
    return m_minAliasedLineWidth;
  }

  [[nodiscard]] float maxAliasedLineWidth() const
  {
    return m_maxAliasedLineWidth;
  }

  // log useful gpu info
  void logGpuInfo() const;

  [[nodiscard]] bool isLinkedListSupported() const;

protected:
  void detectGpuInfo();

  bool parseVersionString(const QString& versionString, int& major, int& minor, int& release);

  void detectDedicatedVideoMemory();

private:
  bool m_isSupported = false; // whether current graphic card is supported
  QString m_notSupportedReason; // Reason why current gpu card are not supported

  RenderBackend m_capsBackend = RenderBackend::OpenGL; // where caps came from

  int m_glMajorVersion = 0;
  int m_glMinorVersion = 0;
  int m_glReleaseVersion = 0;
  int m_glslMajorVersion = 0;
  int m_glslMinorVersion = 0;
  int m_glslReleaseVersion = 0;

  QString m_glVersionString;
  QString m_glExtensionsString;
  QString m_glVendorString;
  QString m_glRendererString;
  QString m_glslVersionString;
  GpuVendor m_gpuVendor = GpuVendor::UNKNOWN;

  int m_maxViewportDims = 16384;
  int m_maxRenderbufferSize = 8192;
  int m_maxTexureSize = 8192;
  int m_max3DTextureSize = 2048;
  float m_maxTextureAnisotropy = 1.0f;
  int m_maxColorAttachments = 4;
  int m_maxDrawBuffer = 8;
  int m_maxClipDistances = 8;
  int m_maxGeometryOutputVertices = 0;
  int m_maxArrayTextureLayers = 256;

  int m_maxTextureBufferSize = 64 * 1024 * 1024;

  // Return a value such as 16 or 32. That is the number of image samplers that your GPU supports in the fragment
  // shader.
  int m_maxTextureImageUnits = 16;
  // The following is for the vertex shader (available since GL 2.0). This might return 0 for certain GPUs.
  int m_maxVertexTextureImageUnits = 16;
  // The following is for the geometry shader (available since GL 3.2)
  int m_maxGeometryTextureImageUnits = 16;
  // The following is VS + GS + FS (available since GL 2.0)
  int m_maxCombinedTextureImageUnits = 48;

  float m_minSmoothPointSize = 1.0f;
  float m_maxSmoothPointSize = 1.0f;
  float m_smoothPointSizeGranularity = 1.0f;
  // float m_minAliasedPointSize;
  // float m_maxAliasedPointSize;

  float m_minSmoothLineWidth = 1.0f;
  float m_maxSmoothLineWidth = 1.0f;
  float m_smoothLineWidthGranularity = 1.0f;
  float m_minAliasedLineWidth = 1.0f;
  float m_maxAliasedLineWidth = 1.0f;

  uint64_t m_dedicatedVideoMemoryMB = 256;

  bool m_contextCoreProfileBit = false;
  bool m_contextCompatibilityProfileBit = false;
  bool m_contextFlagForwardCompatibleBit = false;
  bool m_contextFlagDebugBit = false;
  bool m_contextFlagRobustAccessBit = false;
};

} // namespace nim
