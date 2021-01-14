#pragma once

#include "z3dgl.h"
#include <QStringList>
#include <vector>

namespace nim {

class ZMesh;

class ZMeshIO
{
public:
  static ZMeshIO& instance();

  ZMeshIO();

  [[nodiscard]] bool canReadFile(const QString& filename) const;

  [[nodiscard]] bool canWriteFile(const QString& filename) const;

  [[nodiscard]] const QString& getQtReadNameFilter() const
  { return m_readFilter; }

  void getQtWriteNameFilter(QStringList& filters, std::vector<std::string>& formats);

  static void load(const QString& filename, ZMesh& mesh);

  void save(const ZMesh& mesh, const QString& filename, std::string format) const;

private:
  static void readAllenAtlasMesh(const QString& filename, std::vector<glm::vec3>& normals,
                                 std::vector<glm::vec3>& vertices, std::vector<GLuint>& indices);

private:
  QStringList m_readExts;
  QStringList m_writeExts;
  QString m_readFilter;
  QStringList m_writeFilters;
  std::vector<std::string> m_writeFormats;
};

} // namespace nim

