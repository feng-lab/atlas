#pragma once

#include "z3dcontext.h"
#include <QString>
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
  Z3DShader& shader(const QString& fn, const std::string& header, const Z3DContextGroup& context);

private:
  struct ShaderKey
  {
    ShaderKey(QString fn, std::string hd, const Z3DContextGroup& ct)
      : context(ct)
      , filename(std::move(fn))
      , header(std::move(hd))
    {}

    Z3DContextGroup context;
    QString filename;
    std::string header;

    bool operator==(const ShaderKey& rhs) const = default;

    bool operator<(const ShaderKey& rhs) const
    {
      return std::tie(context, filename, header) < std::tie(rhs.context, rhs.filename, rhs.header);
    }
  };

  std::map<ShaderKey, std::unique_ptr<Z3DShader>> m_shaders;
};

} // namespace nim
