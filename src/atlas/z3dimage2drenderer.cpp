#include "z3dimage2drenderer.h"

#include "z3dtexture.h"
#include "z3dvolume.h"

namespace nim {

Z3DImage2DRenderer::Z3DImage2DRenderer(Z3DRendererBase &rendererBase)
  : Z3DPrimitiveRenderer(rendererBase)
  , m_image2DShader()
  , m_VAO(1)
{
  m_image2DShader.bindFragDataLocation(0, "FragData0");
  m_image2DShader.loadFromSourceFile("transform_with_2dtexture.vert", "image2d_with_colormap.frag", m_rendererBase.generateHeader() + generateHeader());
  CHECK_GL_ERROR;
}

void Z3DImage2DRenderer::setChannels(const std::vector<std::unique_ptr<Z3DVolume>> &volsIn,
                                     const std::vector<std::unique_ptr<ZColorMapParameter>> &colormapsIn)
{
  assert(colormapsIn.size() >= volsIn.size());
  for (size_t i=0; i<volsIn.size(); ++i) {
    assert(volsIn[i]->is2DData());
  }
  std::vector<Z3DVolume*> vols;
  std::vector<ZColorMapParameter*> colormaps;
  for (size_t i=0; i<volsIn.size(); ++i) {
    vols.push_back(volsIn[i].get());
    colormaps.push_back(colormapsIn[i].get());
  }

  if (m_volumes != vols) {
    m_volumes = vols;
    compile();
  }
  m_colormaps = colormaps;

  m_volumeUniformNames.resize(m_volumes.size());
  m_colormapUniformNames.resize(m_volumes.size());
  for (size_t i=0; i<m_volumes.size(); ++i) {
    m_volumeUniformNames[i] = QString("volume_struct_%1").arg(i+1);
    m_colormapUniformNames[i] = QString("colormap_%1").arg(i+1);
  }
}

void Z3DImage2DRenderer::addQuad(const ZMesh &quad)
{
  if (quad.empty() ||
      (quad.numVertices() != 4 && quad.numVertices() != 6) ||
      quad.numVertices() != quad.num2DTextureCoordinates()) {
    LFATAL() << "Input quad should be 2D slice with 2D texture coordinates";
    return;
  }
  m_quads.push_back(quad);
}

void Z3DImage2DRenderer::bindVolumes(Z3DShaderProgram &shader)
{
  size_t idx = 0;
  for (size_t i=0; i < m_volumes.size(); ++i) {
    Z3DVolume *volume = m_volumes[i];
    if (!volume)
      continue;

    // volumes
    shader.bindVolume(m_volumeUniformNames[idx], volume, (GLint)GL_NEAREST, (GLint)GL_NEAREST);

    // colormap
    shader.bindTexture(m_colormapUniformNames[idx++], m_colormaps[i]->get().texture1D());

    CHECK_GL_ERROR;
  }
}

bool Z3DImage2DRenderer::hasVolume() const
{
  for (size_t i=0; i<m_volumes.size(); ++i)
    if (m_volumes[i])
      return true;
  return false;
}

void Z3DImage2DRenderer::compile()
{
  m_image2DShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
}

QString Z3DImage2DRenderer::generateHeader()
{
  QString headerSource;

  if (hasVolume()) {
    headerSource += QString("#define NUM_VOLUMES %1\n").arg(m_volumes.size());
  } else {
    headerSource += QString("#define NUM_VOLUMES 0\n");
    headerSource += "#define DISABLE_TEXTURE_COORD_OUTPUT\n";
  }

  return headerSource;
}

void Z3DImage2DRenderer::render(Z3DEye eye)
{
  bool needRender = hasVolume() && !m_quads.empty();
  if (!needRender)
    return;

  m_image2DShader.bind();
  m_rendererBase.setGlobalShaderParameters(m_image2DShader, eye);

  bindVolumes(m_image2DShader);

  for (size_t i=0; i<m_quads.size(); ++i)
    renderTriangleList(m_VAO, m_image2DShader, m_quads[i]);

  m_image2DShader.release();
}

} // namespace nim
