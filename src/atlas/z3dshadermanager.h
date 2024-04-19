#pragma once

#include "zexception.h"
#include "z3dcontext.h"
#include <map>
#include <memory>
#include <utility>

namespace nim {

class Z3DShader;

// throw ZException if compile error
class Z3DShaderManager
{
public:
  static Z3DShaderManager& instance();

  // return reference because it is always valid
  Z3DShader& shader(const QString& fn, const QString& header, const Z3DContextGroup& context);

private:
  struct ShaderKey
  {
    ShaderKey(QString fn, QString hd, const Z3DContextGroup& ct)
      : context(ct)
      , filename(std::move(fn))
      , header(std::move(hd))
    {}

    Z3DContextGroup context;
    QString filename;
    QString header;

    auto operator<=>(const ShaderKey& rhs) const = default;
  };

  std::map<ShaderKey, std::unique_ptr<Z3DShader>> m_shaders;
};

} // namespace nim
