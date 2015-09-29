#ifndef Z3DTEXTURE_H
#define Z3DTEXTURE_H

#include "z3dgl.h"

namespace nim {

class Z3DTexture
{
public:
  // default min and mag filter is GL_LINEAR, default wrap is GL_CLAMP_TO_EDGE

  // construct 1d, 2d or 3d texture, texture target will be derived from dimension
  // Result Z3DTexture target will be one of GL_TEXTURE_1D, GL_TEXTURE_2D or GL_TEXTURE_3D
  Z3DTexture(const glm::ivec3& dimensions, GLenum dataFormat, GLint internalformat, GLenum dataType);
  Z3DTexture(const glm::ivec3& dimensions, GLenum dataFormat, GLint internalformat, GLenum dataType,
             GLint minFilter, GLint magFilter, GLint wrap);
  // construct any texture with user provided textureTarget
  Z3DTexture(const glm::ivec3& dimensions, GLenum textureTarget, GLenum dataFormat, GLint internalformat,
             GLenum dataType);
  Z3DTexture(const glm::ivec3& dimensions, GLenum textureTarget, GLenum dataFormat, GLint internalformat,
             GLenum dataType, GLint minFilter, GLint magFilter, GLint wrap);
  ~Z3DTexture();

  // call this only if you want to upload something. ( usually openGL will allocate texture
  // memory ) Input data must match current dataFormat and dataType.
  // Z3DTexture will **not** take ownership of the input data
  // call this before uploadTexture
  void setData(void *data) { m_data = (GLvoid*)data; }

  void uploadTexture();
  void bind() const { glBindTexture(m_textureTarget, m_id); }

  GLuint id() const { return m_id; }
  // Check if texture is in resident GL memory
  //bool isResident() const { GLboolean res; return glAreTexturesResident(1, &m_id, &res) == GL_TRUE; }
  bool is1DTexture() const;
  bool is2DTexture() const;
  bool is3DTexture() const;

  // buffer must have at least bypePerPixel(dataFormat, dataType) * numPixels() bytes space, crash otherwise
  void downloadTextureToBuffer(GLenum dataFormat, GLenum dataType, GLvoid* buffer) const;

  int textureSizeOnGPU() const;

  GLenum textureTarget() const { return m_textureTarget; }
  glm::ivec3 dimensions() const { return m_dimensions;}
  int width() const { return m_dimensions.x; }
  int height() const { return m_dimensions.y; }
  int depth() const { return m_dimensions.z; }
  size_t numPixels() const { return m_dimensions.x * m_dimensions.y * m_dimensions.z; }
  GLenum dataFormat() const { return m_dataFormat; }
  GLint internalFormat() const { return m_internalFormat; }
  GLint minFilter() const { return m_minFilter; }
  GLint magFilter() const { return m_magFilter; }
  GLenum dataType() const { return m_dataType; }

  void setFilter(GLint minFilter, GLint magFilter);
  void setMinFilter(GLint minFilter);
  void setMagFilter(GLint magFilter);
  void setWrap(GLint wrap);

  // changes made by the following four functions will take effect after next call of uploadTexture()
  void setDimensions(glm::ivec3 dimensions) { m_dimensions = dimensions; }
  void setDataFormat(GLenum format) { m_dataFormat = format; }
  void setInternalFormat(GLint internalformat) { m_internalFormat = internalformat; }
  void setDataType(GLenum dataType) { m_dataType = dataType; }

  // calculates the bytes per pixel from dataFormat and dataType
  static size_t bypePerPixel(GLenum dataFormat, GLenum dataType);
  // calculates the bytes per pixel from the internal format
  static size_t bypePerPixel(GLint internalFormat);

  void saveAsColorImage(const QString &filename) const;
  void saveAsDepthImage(const QString &filename) const;

private:
  void deriveTextureTarget();
  void applyFilter();
  void applyWrap();
  bool useMipmap() const;
  void init();

protected:
  glm::ivec3 m_dimensions;
  GLenum m_textureTarget;
  GLenum m_dataFormat;
  GLint m_internalFormat;
  GLenum m_dataType;

  GLint m_minFilter;
  GLint m_magFilter;
  GLint m_wrap;

  GLuint m_id; // texture id

  GLvoid *m_data;
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
  ~Z3DTextureUnitManager();

  void nextAvailableUnit();
  void activateCurrentUnit();
  GLint currentUnitNumber() const { return m_currentUnitNumber; }
  GLenum currentUnitEnum() const;
  // clear assigned unit
  void reset() { m_currentUnitNumber = -1; }

  static GLint activeTextureUnit();

protected:
  int m_maxTextureUnits;   // total number of available units
  GLint m_currentUnitNumber;
};

} // namespace nim

#endif // Z3DTEXTURE_H
