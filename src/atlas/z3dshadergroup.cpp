#include "z3dshadergroup.h"

#include "z3dgl.h"
#include "z3dgpuinfo.h"
#include "z3dshaderprogram.h"
#include "zlog.h"

namespace nim {

Z3DShaderGroup::Z3DShaderGroup(Z3DRendererBase& rendererBase)
  : m_base(rendererBase)
{}

void Z3DShaderGroup::init(const QStringList& shaderFiles,
                          const std::string& header,
                          const std::string& geomHeader,
                          const QStringList& normalShaderFiles)
{
  m_shaderFiles = shaderFiles;
  m_header = header;
  m_geomHeader = geomHeader;
  m_normalShaderFiles = normalShaderFiles;
  m_shaders[Z3DRendererBase::ShaderHookType::Normal] = std::make_unique<Z3DShaderProgram>();
  buildNormalShader(m_shaders[Z3DRendererBase::ShaderHookType::Normal].get());
}

void Z3DShaderGroup::addAllSupportedPostShaders()
{
  addDualDepthPeelingShaders();
  addWeightedAverageShaders();
  addWeightedBlendedShaders();
}

void Z3DShaderGroup::addDualDepthPeelingShaders()
{
  m_shaders[Z3DRendererBase::ShaderHookType::DualDepthPeelingInit] = std::make_unique<Z3DShaderProgram>();
  m_shaders[Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel] = std::make_unique<Z3DShaderProgram>();
  buildDualDepthPeelingInitShader(m_shaders[Z3DRendererBase::ShaderHookType::DualDepthPeelingInit].get());
  buildDualDepthPeelingPeelShader(m_shaders[Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel].get());
}

void Z3DShaderGroup::addWeightedAverageShaders()
{
  m_shaders[Z3DRendererBase::ShaderHookType::WeightedAverageInit] = std::make_unique<Z3DShaderProgram>();
  buildWeightedAverageShader(m_shaders[Z3DRendererBase::ShaderHookType::WeightedAverageInit].get());
}

void Z3DShaderGroup::addWeightedBlendedShaders()
{
  m_shaders[Z3DRendererBase::ShaderHookType::WeightedBlendedInit] = std::make_unique<Z3DShaderProgram>();
  buildWeightedBlendedShader(m_shaders[Z3DRendererBase::ShaderHookType::WeightedBlendedInit].get());
}

void Z3DShaderGroup::bind()
{
  get().bind();
  if (m_base.shaderHookType() == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
    get().bindTexture("DepthBlenderTex", m_base.shaderHookPara().dualDepthPeelingDepthBlenderTexture);
    get().bindTexture("FrontBlenderTex", m_base.shaderHookPara().dualDepthPeelingFrontBlenderTexture);
  } else if (m_base.shaderHookType() == Z3DRendererBase::ShaderHookType::WeightedBlendedInit) {
    float n = m_base.viewState().nearClip;
    float f = m_base.viewState().farClip;
    // http://www.opengl.org/archives/resources/faq/technical/depthbuffer.htm
    //  zw = a/ze + b;  ze = a/(zw - b);  a = f*n/(f-n);  b = 0.5*(f+n)/(f-n) + 0.5;
    float a = f * n / (f - n);
    float b = 0.5f * (f + n) / (f - n) + 0.5f;
    get().setUniform("ze_to_zw_b", b);
    get().setUniform("ze_to_zw_a", a);
    get().setUniform("weighted_blended_depth_scale", m_base.sceneState().weightedBlendedDepthScale);
  }
}

void Z3DShaderGroup::release()
{
  // if bind is ok, this should be fine
  get().release();
}

Z3DShaderProgram& Z3DShaderGroup::get()
{
  return *m_shaders[m_base.shaderHookType()];
}

void Z3DShaderGroup::rebuild(const std::string& header, const std::string& geomHeader)
{
  m_header = header;
  m_geomHeader = geomHeader;
  auto i = m_shaders.begin();
  while (i != m_shaders.end()) {
    i->second->removeAllShaders();
    switch (i->first) {
      case Z3DRendererBase::ShaderHookType::Normal:
        buildNormalShader(i->second.get());
        break;
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingInit:
        buildDualDepthPeelingInitShader(i->second.get());
        break;
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel:
        buildDualDepthPeelingPeelShader(i->second.get());
        break;
      case Z3DRendererBase::ShaderHookType::WeightedAverageInit:
        buildWeightedAverageShader(i->second.get());
        break;
      case Z3DRendererBase::ShaderHookType::WeightedBlendedInit:
        buildWeightedBlendedShader(i->second.get());
        break;
      default:
        break;
    }
    ++i;
  }
}

void Z3DShaderGroup::buildNormalShader(Z3DShaderProgram* shader)
{
  CHECK(shader);
  if (m_normalShaderFiles.empty()) {
    QStringList allshaders(m_shaderFiles);
    allshaders << "common.frag";
    shader->loadFromSourceFile(allshaders, m_header, m_geomHeader);
  } else {
    shader->loadFromSourceFile(m_normalShaderFiles, m_header, m_geomHeader);
  }
}

void Z3DShaderGroup::buildDualDepthPeelingInitShader(Z3DShaderProgram* shader)
{
  CHECK(shader);
  QStringList allshaders(m_shaderFiles);
  allshaders << "dual_peeling_init.frag";
  shader->loadFromSourceFile(allshaders, m_header, m_geomHeader);
}

void Z3DShaderGroup::buildDualDepthPeelingPeelShader(Z3DShaderProgram* shader)
{
  CHECK(shader);
  QStringList allshaders(m_shaderFiles);
  allshaders << "dual_peeling_peel.frag";
  shader->loadFromSourceFile(allshaders, m_header, m_geomHeader);
}

void Z3DShaderGroup::buildWeightedAverageShader(Z3DShaderProgram* shader)
{
  CHECK(shader);
  QStringList allshaders(m_shaderFiles);
  allshaders << "wavg_init.frag";
  shader->loadFromSourceFile(allshaders, m_header, m_geomHeader);
}

void Z3DShaderGroup::buildWeightedBlendedShader(Z3DShaderProgram* shader)
{
  CHECK(shader);
  QStringList allshaders(m_shaderFiles);
  allshaders << "wblended_init.frag";
  shader->loadFromSourceFile(allshaders, m_header, m_geomHeader);
}

} // namespace nim
