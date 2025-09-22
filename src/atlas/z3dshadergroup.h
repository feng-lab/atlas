#pragma once

#include "z3drendererbase.h"
#include "z3dshaderprogram.h"
#include <map>

namespace nim {

class Z3DShaderGroup
{
public:
  explicit Z3DShaderGroup(Z3DRendererBase& rendererBase);

  void init(const QStringList& shaderFiles,
            const std::string& header,
            const std::string& geomHeader = "",
            const QStringList& normalShaderFiles = QStringList());

  void addAllSupportedPostShaders();

  void addDualDepthPeelingShaders();

  void addWeightedAverageShaders();

  void addWeightedBlendedShaders();

  void bind();

  void release();

  Z3DShaderProgram& get();

  void rebuild(const std::string& header, const std::string& geomHeader = "");

private:
  void buildNormalShader(Z3DShaderProgram* shader);

  void buildDualDepthPeelingInitShader(Z3DShaderProgram* shader);

  void buildDualDepthPeelingPeelShader(Z3DShaderProgram* shader);

  void buildWeightedAverageShader(Z3DShaderProgram* shader);

  void buildWeightedBlendedShader(Z3DShaderProgram* shader);

private:
  QStringList m_shaderFiles;
  std::string m_header;
  std::string m_geomHeader;
  Z3DRendererBase& m_base;
  QStringList m_normalShaderFiles;
  std::map<Z3DRendererBase::ShaderHookType, std::unique_ptr<Z3DShaderProgram>> m_shaders;
};

} // namespace nim
