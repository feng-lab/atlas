#include "z3dvolumeslicerenderer.h"

#include "z3dtexture.h"
#include "z3dvolume.h"
#include "z3dimg.h"
#include "zlog.h"

namespace nim {

Z3DVolumeSliceRenderer::Z3DVolumeSliceRenderer(Z3DRendererBase& rendererBase)
  : Z3DPrimitiveRenderer(rendererBase)
  , m_VAO(1)
{
  //  m_volumeSliceShader.bindFragDataLocation(0, "FragData0");
  //  m_volumeSliceShader.loadFromSourceFile("transform_with_3dtexture.vert", "volume_slice_with_colormap.frag",
  //                                         m_rendererBase.generateHeader() + generateHeader());

  m_scVolumeSliceShader.bindFragDataLocation(0, "FragData0");
  m_scVolumeSliceShader.loadFromSourceFile("transform_with_3dtexture.vert",
                                           "volume_slice_with_colormap_single_channel.frag",
                                           m_rendererBase.generateHeader() + generateHeader());
  m_mergeChannelShader.bindFragDataLocation(0, "FragData0");
  m_mergeChannelShader.loadFromSourceFile("pass.vert", "image2d_array_compositor.frag",
                                          m_rendererBase.generateHeader() + generateHeader());
  CHECK_GL_ERROR
}

void
Z3DVolumeSliceRenderer::setData(const Z3DImg& img, const std::vector<std::unique_ptr<ZColorMapParameter> >& colormaps)
{
  CHECK(colormaps.size() >= img.numChannels() && img.is3DData());

  m_img = &img;
  m_colormaps = &colormaps;

  if (m_img->numChannels() != m_volumeUniformNames.size()) {
    compile();
    m_volumeUniformNames.resize(m_img->numChannels());
    m_colormapUniformNames.resize(m_img->numChannels());
    for (size_t i = 0; i < m_img->numChannels(); ++i) {
      m_volumeUniformNames[i] = QString("volume_%1").arg(i + 1);
      m_colormapUniformNames[i] = QString("colormap_%1").arg(i + 1);
    }
  }
}

void Z3DVolumeSliceRenderer::addQuad(const ZMesh& quad)
{
  if (quad.empty() ||
      (quad.numVertices() != 4 && quad.numVertices() != 6) ||
      quad.numVertices() != quad.num3DTextureCoordinates()) {
    LOG(FATAL) << "Input quad should be 2D slice with 3D texture coordinates";
    return;
  }
  m_quads.push_back(quad);
}

void Z3DVolumeSliceRenderer::bindVolumes(Z3DShaderProgram& shader)
{
  size_t idx = 0;
  for (size_t i = 0; i < m_img->numChannels(); ++i) {
    // volumes
    shader.bindTexture(m_volumeUniformNames[idx], m_img->volumes()[i]->texture(),
                       GLint(GL_NEAREST), GLint(GL_NEAREST));

    // colormap
    shader.bindTexture(m_colormapUniformNames[idx++], (*m_colormaps)[i]->get().texture1D());

    CHECK_GL_ERROR
  }
}

void Z3DVolumeSliceRenderer::bindVolume(Z3DShaderProgram& shader, size_t idx)
{
  // volumes
  shader.bindTexture(m_volumeUniformNames[0], m_img->volumes()[idx]->texture(),
                     GLint(GL_NEAREST), GLint(GL_NEAREST));

  // colormap
  shader.bindTexture(m_colormapUniformNames[0], (*m_colormaps)[idx]->get().texture1D());

  CHECK_GL_ERROR
}

void Z3DVolumeSliceRenderer::compile()
{
  //m_volumeSliceShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());

  m_scVolumeSliceShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  m_mergeChannelShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
}

QString Z3DVolumeSliceRenderer::generateHeader()
{
  QString headerSource;

  if (m_img && m_img->numChannels() > 0) {
    headerSource += QString("#define NUM_VOLUMES %1\n").arg(m_img->numChannels());
  } else {
    headerSource += QString("#define NUM_VOLUMES 0\n");
    headerSource += "#define DISABLE_TEXTURE_COORD_OUTPUT\n";
  }

  // for merge shader
  headerSource += "#define MAX_PROJ_MERGE\n";

  return headerSource;
}

void Z3DVolumeSliceRenderer::render(Z3DEye eye)
{
  bool needRender = m_img && m_img->numChannels() > 0 && !m_quads.empty();
  if (!needRender)
    return;

  m_scVolumeSliceShader.bind();
  m_rendererBase.setGlobalShaderParameters(m_scVolumeSliceShader, eye);

  if (m_img->numChannels() == 1) {
    bindVolume(m_scVolumeSliceShader, 0);
    for (size_t i = 0; i < m_quads.size(); ++i)
      renderTriangleList(m_VAO, m_scVolumeSliceShader, m_quads[i]);
  } else {
    for (size_t j = 0; j < m_img->numChannels(); ++j) {
      m_layerTarget->attachSlice(j);
      m_layerTarget->bind();
      m_layerTarget->clear();

      bindVolume(m_scVolumeSliceShader, j);
      for (size_t i = 0; i < m_quads.size(); ++i)
        renderTriangleList(m_VAO, m_scVolumeSliceShader, m_quads[i]);

      m_layerTarget->release();
    }
  }

  m_scVolumeSliceShader.release();

  if (m_img->numChannels() > 1) {
    m_mergeChannelShader.bind();
    m_mergeChannelShader.bindTexture("color_texture", m_layerTarget->attachment(GL_COLOR_ATTACHMENT0));
    m_mergeChannelShader.bindTexture("depth_texture", m_layerTarget->attachment(GL_DEPTH_ATTACHMENT));
    renderScreenQuad(m_VAO, m_mergeChannelShader);
    m_mergeChannelShader.release();
  }
}

} // namespace nim

