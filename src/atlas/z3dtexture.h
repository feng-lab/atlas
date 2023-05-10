#pragma once

#include "z3dgl.h"

namespace nim {

class Z3DTexture
{
public:
  Z3DTexture(GLenum textureTarget,
             GLint internalFormat,
             const glm::uvec3& dimension,
             GLenum dataFormat,
             GLenum dataType,
             const GLvoid* data = nullptr,
             GLint minFilter = GLint(GL_LINEAR),
             GLint magFilter = GLint(GL_LINEAR),
             GLint wrap = GLint(GL_CLAMP_TO_EDGE));

  // derive texture target as 1D, 2D or 3D
  Z3DTexture(GLint internalFormat,
             const glm::uvec3& dimension,
             GLenum dataFormat,
             GLenum dataType,
             const GLvoid* data = nullptr,
             GLint minFilter = GLint(GL_LINEAR),
             GLint magFilter = GLint(GL_LINEAR),
             GLint wrap = GLint(GL_CLAMP_TO_EDGE));

  ~Z3DTexture();

  // default is GL_LINEAR and GL_LINEAR.
  // note: openGL default is GL_NEAREST_MIPMAP_LINEAR and GL_LINEAR.
  void setFilter(GLint minFilter = GLint(GL_LINEAR), GLint magFilter = GLint(GL_LINEAR)) const;

  // default is GL_CLAMP_TO_EDGE
  // note: openGL default is GL_REPEAT
  void setWrap(GLint wrap = GLint(GL_CLAMP_TO_EDGE)) const;

  //
  void generateMipmap() const;

  void setDimension(const glm::uvec3& dimension, const GLvoid* data = nullptr)
  {
    if (m_dimension != dimension) {
      m_dimension = dimension;
      initializeImage(data);
    }
  }

  void setInternalFormat(GLint internalformat, const GLvoid* data = nullptr)
  {
    if (m_internalFormat != internalformat) {
      m_internalFormat = internalformat;
      initializeImage(data);
    }
  }

  void setDataFormat(GLenum format, const GLvoid* data = nullptr)
  {
    if (m_dataFormat != format) {
      m_dataFormat = format;
      initializeImage(data);
    }
  }

  void setDataType(GLenum dataType, const GLvoid* data = nullptr)
  {
    if (m_dataType != dataType) {
      m_dataType = dataType;
      initializeImage(data);
    }
  }

  // glTexSubImage*D
  void updateImage(const GLvoid* data) const;

  // glTexSubImage*D
  void updateSubImage(const glm::uvec3& offset, const glm::uvec3& size, const GLvoid* data) const;

  // clear image with 0
  void clearImage() const;

  void bind() const
  {
    glBindTexture(m_textureTarget, m_id);
  }

  [[nodiscard]] GLuint id() const
  {
    return m_id;
  }

  [[nodiscard]] GLenum textureTarget() const
  {
    return m_textureTarget;
  }

  [[nodiscard]] glm::uvec3 dimension() const
  {
    return m_dimension;
  }

  [[nodiscard]] size_t width() const
  {
    return m_dimension.x;
  }

  [[nodiscard]] size_t height() const
  {
    return m_dimension.y;
  }

  [[nodiscard]] size_t depth() const
  {
    return m_dimension.z;
  }

  [[nodiscard]] size_t numPixels() const
  {
    return m_dimension.x * m_dimension.y * m_dimension.z;
  }

  [[nodiscard]] GLenum dataFormat() const
  {
    return m_dataFormat;
  }

  [[nodiscard]] GLenum dataType() const
  {
    return m_dataType;
  }

  [[nodiscard]] GLint internalFormat() const
  {
    return m_internalFormat;
  }

  // calculates the bytes per pixel from dataFormat and dataType
  static size_t bypePerPixel(GLenum dataFormat, GLenum dataType);

  // calculates the bytes per pixel from the internal format
  static size_t bypePerPixel(GLint internalFormat);

  // buffer must have at least bypePerPixel(dataFormat, dataType) * numPixels() bytes space, crash otherwise
  void downloadTextureToBuffer(GLenum dataFormat, GLenum dataType, GLvoid* buffer) const;

  [[nodiscard]] size_t textureSizeOnGPU() const
  {
    return bypePerPixel(m_internalFormat) * numPixels();
  }

  void saveAsColorImage(const QString& filename) const;

  void saveAsDepthImage(const QString& filename) const;

  void saveAsRGBFloatImage(const QString& filename) const;

  void saveAsRGBAFloatImage(const QString& filename) const;

private:
  [[nodiscard]] bool is1DTexture() const;

  [[nodiscard]] bool is2DTexture() const;

  [[nodiscard]] bool is3DTexture() const;

  void getType();

  // Input data must match current dataFormat and dataType.
  void initializeImage(const GLvoid* data = nullptr) const;

private:
  GLenum m_textureTarget;
  glm::uvec3 m_dimension;
  GLint m_internalFormat;

  GLenum m_dataFormat;
  GLenum m_dataType;

  GLuint m_id = 0; // texture id

  int m_type = 0;
};

// provide unique texture units
// usage:

// auto textureUnitManager = std::make_unique<Z3DTextureUnitManager>();
// textureUnitManager->nextAvailableUnit();
// textureUnitManager->activateCurrentUnit();
// texture->bind();

// textureUnitManager->nextAvailableUnit();
// ...
class Z3DTextureUnitManager
{
public:
  Z3DTextureUnitManager();

  void nextAvailableUnit();

  void activateCurrentUnit();

  [[nodiscard]] GLint currentUnitNumber() const
  {
    return m_currentUnitNumber;
  }

  [[nodiscard]] GLenum currentUnitEnum() const;

  // clear assigned unit
  void reset()
  {
    m_currentUnitNumber = -1;
  }

  static GLint activeTextureUnit();

protected:
  int m_maxTextureUnits; // total number of available units
  GLint m_currentUnitNumber;
};

} // namespace nim
