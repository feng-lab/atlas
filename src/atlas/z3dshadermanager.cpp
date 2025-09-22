#include "z3dshadermanager.h"

#include "zsysteminfo.h"
#include "zlog.h"
#include "z3dshader.h"
#include "zioutils.h"

namespace nim {

Z3DShaderManager& Z3DShaderManager::instance()
{
  static Z3DShaderManager sm;
  return sm;
}

Z3DShader& Z3DShaderManager::shader(const QString& fn, const std::string& header, const Z3DContextGroup& context)
{
  ShaderKey key(fn, header, context);
  auto lb = m_shaders.lower_bound(key);
  if (lb == m_shaders.end() || m_shaders.key_comp()(key, lb->first)) {
    QString filename = ZSystemInfo::instance().shaderPath(fn);
    Z3DShader::Type type;
    if (filename.endsWith(".vert", Qt::CaseInsensitive)) {
      type = Z3DShader::Type::Vertex;
    } else if (filename.endsWith(".geom", Qt::CaseInsensitive)) {
      type = Z3DShader::Type::Geometry;
    } else if (filename.endsWith(".frag", Qt::CaseInsensitive)) {
      type = Z3DShader::Type::Fragment;
    } else {
      throw ZException(
        fmt::format("Not supported file extension: {}. Use .vert, .geom or .frag as shader extension", filename));
    }

    auto fileSource = readFileIntoQByteArray(filename);
    std::string src;
    src.reserve(header.size() + fileSource.size());
    src.append(header);
    src.append(fileSource.constData(), fileSource.size());

    CHECK(context == Z3DContextGroup());
    auto shdr = std::make_unique<Z3DShader>(type);
    shdr->compileSourceCode(src);
    Z3DShader* res = shdr.get();
    m_shaders.emplace_hint(lb, key, std::move(shdr));
    return *res;
  }
  return *(lb->second);
}

} // namespace nim
