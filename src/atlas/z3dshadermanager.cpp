#include "z3dshadermanager.h"

#include "zsysteminfo.h"
#include <QFile>
#include "zexception.h"
#include "QsLog.h"
#include "z3dshader.h"
#include <cassert>

namespace nim {

Z3DShaderManager &Z3DShaderManager::instance()
{
  static Z3DShaderManager sm;
  return sm;
}

Z3DShaderManager::Z3DShaderManager()
{
}

Z3DShader &Z3DShaderManager::shader(const QString &fn, const QString &header, const Z3DContext& context)
{
  ShaderKey key(fn, header, context);
  auto it = m_shaders.find(key);
  if (it == m_shaders.end()) {
    QString filename = ZSystemInfoInstance.shaderPath(fn);
    Z3DShader::Type type;
    if (filename.endsWith(".vert", Qt::CaseInsensitive)) {
      type = Z3DShader::Type::Vertex;
    } else if (filename.endsWith(".geom", Qt::CaseInsensitive)) {
      type = Z3DShader::Type::Geometry;
    } else if (filename.endsWith(".frag", Qt::CaseInsensitive)) {
      type = Z3DShader::Type::Fragment;
    } else {
      throw ZGLException(QString("Not supported file extension: %1. Use .vert, .geom or .frag as shader extension").arg(filename));
    }

    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
      throw ZGLException(QString("Can not open vertex shader file: %1.  Error String: %2").arg(filename).arg(file.errorString()));
    }
    QString src = header + file.readAll();

    assert(context == Z3DContext());
    auto shdr = std::make_unique<Z3DShader>(type);
    shdr->compileSourceCode(src);
    Z3DShader *res = shdr.get();
    m_shaders.emplace(key, std::move(shdr));
    return *res;
  } else {
    return *(it->second);
  }
}

bool Z3DShaderManager::ShaderKey::operator<(const Z3DShaderManager::ShaderKey &rhs) const
{
  if (filename != rhs.filename)
    return filename < rhs.filename;
  if (context != rhs.context)
    return context < rhs.context;
  return header < rhs.header;
}

} // namespace nim
