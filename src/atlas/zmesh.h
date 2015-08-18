#ifndef ZMESH_H
#define ZMESH_H

#include <vector>
#include "z3dgl.h"
#include "zglobal.h"
#include <H5Cpp.h>

namespace nim {

class ZMesh
{
public:
  // one of GL_TRIANGLES, GL_TRIANGLE_STRIP and GL_TRIANGLE_FAN
  explicit ZMesh(GLenum type = GL_TRIANGLES);
  // might throw ZIOException
  explicit ZMesh(const QString &filename);
  virtual ~ZMesh();

#ifndef _USE_MSVC2013_
  ZMesh(ZMesh&&) = default;
  ZMesh& operator=(ZMesh&&) = default;
#endif
  ZMesh(const ZMesh&) = default;
  ZMesh& operator=(const ZMesh&) = default;

  void swap(ZMesh &rhs) noexcept;

  // qt style read write name filter for filedialog
  static bool canReadFile(const QString& filename);
  static bool canWriteFile(const QString& filename);
  static const QString& getQtReadNameFilter();
  static void getQtWriteNameFilter(QStringList &filters, QList<std::string> &formats);
  // might throw ZIOException
  void load(const QString &filename);
  void save(const QString &filename, const std::string& format = "") const;

  void load(H5::Group &grp);
  void save(H5::Group &grp) const;

  std::vector<double> boundBox() const;
  std::vector<double> boundBox(const glm::mat4& transform) const;

  GLenum type() const { return m_type; }
  QString typeAsString() const;
  void setType(GLenum type) { m_type = type; assert(m_type == GL_TRIANGLES || m_type == GL_TRIANGLE_FAN || m_type == GL_TRIANGLE_STRIP); }
  const std::vector<glm::vec3>& vertices() const { return m_vertices; }
  void setVertices(const std::vector<glm::vec3> &vertices) { m_vertices = vertices; }
  std::vector<glm::dvec3> doubleVertices() const;
  void setVertices(const std::vector<glm::dvec3> &vertices);
  const std::vector<float>& textureCoordinates1D() const { return m_1DTextureCoordinates; }
  void setTextureCoordinates(const std::vector<float> &textureCoordinates) { m_1DTextureCoordinates = textureCoordinates; }
  const std::vector<glm::vec2>& textureCoordinates2D() const { return m_2DTextureCoordinates; }
  void setTextureCoordinates(const std::vector<glm::vec2> &textureCoordinates) { m_2DTextureCoordinates = textureCoordinates; }
  const std::vector<glm::vec3>& textureCoordinates3D() const { return m_3DTextureCoordinates; }
  void setTextureCoordinates(const std::vector<glm::vec3> &textureCoordinates) { m_3DTextureCoordinates = textureCoordinates; }
  const std::vector<glm::vec3>& normals() const { return m_normals; }
  void setNormals(const std::vector<glm::vec3> &normals) { m_normals = normals; }
  const std::vector<glm::vec4>& colors() const { return m_colors; }
  void setColors(const std::vector<glm::vec4> &colors) { m_colors = colors; }
  const std::vector<GLuint>& indices() const { return m_indices; }
  void setIndices(const std::vector<GLuint> &indices) { m_indices = indices; }
  bool hasIndices() const { return !m_indices.empty(); }

  // use ref to interpolate texture coordinate and colors. all vertices should be on ref surface
  void interpolate(const ZMesh &ref);

  // return true if no vertex
  bool empty() const { return m_vertices.empty(); }

  void clear();

  size_t numVertices() const { return m_vertices.size(); }
  size_t numTriangles() const;
  size_t numColors() const { return m_colors.size(); }
  size_t numNormals() const { return m_normals.size(); }
  size_t num1DTextureCoordinates() const { return m_1DTextureCoordinates.size(); }
  size_t num2DTextureCoordinates() const { return m_2DTextureCoordinates.size(); }
  size_t num3DTextureCoordinates() const { return m_3DTextureCoordinates.size(); }
  std::vector<glm::vec3> triangleVertices(size_t index) const;
  std::vector<glm::uvec3> triangleIndices() const;
  glm::uvec3 triangleIndices(size_t index) const;
  glm::vec3 triangleVertex(size_t triangleIndex, size_t vertexIndex) const;

  void transformVerticesByMatrix(const glm::mat4 &tfmat);

  std::vector<ZMesh> split(size_t numTriangle = 100000) const;

  void generateNormals(bool useAreaWeight = true);

  double volume() const;

  // a list of cubes with normal
  static ZMesh createCubesWithNormal(const std::vector<glm::vec3>& coordLlfs,
                                     const std::vector<glm::vec3>& coordUrbs);

  // a cube with six surfaces
  static ZMesh createCube(
      glm::vec3 coordLlf = glm::vec3(0.f, 0.f, 0.f),
      glm::vec3 coordUrb = glm::vec3(1.f, 1.f, 1.f),
      glm::vec3 texLlf = glm::vec3(0.f, 0.f, 0.f),
      glm::vec3 texUrb = glm::vec3(1.f, 1.f, 1.f));

  // one slice from a cube, it is a x slice if alongDim == 0, a y slice if alongDim == 1,
  // a z slice if alongDim == 2
  static ZMesh createCubeSlice(
      float coordIn3rdDim,
      float texCoordIn3rdDim,
      int alongDim = 2,     // 0, 1, or 2
      glm::vec2 coordlow = glm::vec2(0.f, 0.f),
      glm::vec2 coordhigh = glm::vec2(1.f, 1.f),
      glm::vec2 texlow = glm::vec2(0.f, 0.f),
      glm::vec2 texhigh = glm::vec2(1.f, 1.f));

  // one slice from a cube, it is a x slice if alongDim == 0, a y slice if alongDim == 1,
  // a z slice if alongDim == 2
  static ZMesh createCubeSliceWith2DTexture(
      float coordIn3rdDim,
      int alongDim = 2,     // 0, 1, or 2
      glm::vec2 coordlow = glm::vec2(0.f, 0.f),
      glm::vec2 coordhigh = glm::vec2(1.f, 1.f),
      glm::vec2 texlow = glm::vec2(0.f, 0.f),
      glm::vec2 texhigh = glm::vec2(1.f, 1.f));

  // a 2d image quad with 2d texture coordinates
  static ZMesh createImageSlice(
      float coordIn3rdDim,
      glm::vec2 coordlow = glm::vec2(0.f, 0.f),
      glm::vec2 coordhigh = glm::vec2(1.f, 1.f),
      glm::vec2 texlow = glm::vec2(0.f, 0.f),
      glm::vec2 texhigh = glm::vec2(1.f, 1.f));

  // create a serie of slices from a cube, slices are cut along a specified dimension
  // if number of slices if 1, first slice will be returned (use first coordinate)
  // if number of slices is 2, first and last slices will be returned (use first and last coordinate)
  // other slices are interpolated between first and last slice
  // assume that the last slice is nearest to camera, then created triangles will face camera if first
  // coordinate is smaller than last coordinatesin the two fixed dimensions.
  // in cut dimension, last coordinate can be smaller than first coordinate to create inverse order series
  static ZMesh createCubeSerieSlices(
      int numSlices,
      int alongDim = 2,     // 0, 1, or 2
      glm::vec3 coordfirst = glm::vec3(0.f, 0.f, 0.f),
      glm::vec3 coordlast = glm::vec3(1.f, 1.f, 1.f),
      glm::vec3 texfirst = glm::vec3(0.f, 0.f, 0.f),
      glm::vec3 texlast = glm::vec3(1.f, 1.f, 1.f));

private:
  void appendTriangle(const ZMesh &mesh, glm::uvec3 triangle);

  double signedVolumeOfTriangle(const glm::vec3 &v1,
                                const glm::vec3 &v2,
                                const glm::vec3 &v3) const;

  size_t numCoverCubes(double cubeEdgeLength);

private:
  friend class ZMeshIO;

  GLenum m_type;

  std::vector<glm::vec3> m_vertices;
  std::vector<float> m_1DTextureCoordinates;
  std::vector<glm::vec2> m_2DTextureCoordinates;
  std::vector<glm::vec3> m_3DTextureCoordinates;
  std::vector<glm::vec3> m_normals;
  std::vector<glm::vec4> m_colors;
  std::vector<GLuint> m_indices;
};

} // namespace nim

#endif // ZMESH_H
