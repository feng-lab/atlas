#include "z3dvolumeslicerenderer.h"

#include "z3dtexture.h"
#include "z3dvolume.h"
#include "z3dimg.h"
#include "zlog.h"
#include <absl/strings/str_cat.h>

namespace nim {

Z3DVolumeSliceRenderer::Z3DVolumeSliceRenderer(Z3DRendererBase& rendererBase)
  : Z3DPrimitiveRenderer(rendererBase)
  , m_VAO(1)
{
  //  m_volumeSliceShader.loadFromSourceFile("transform_with_3dtexture.vert", "volume_slice_with_colormap.frag",
  //                                         m_rendererBase.generateHeader() + generateHeader());

  m_scVolumeSliceShader.loadFromSourceFile("transform_with_3dtexture.vert",
                                           "volume_slice_with_colormap_single_channel.frag",
                                           m_rendererBase.generateHeader() + generateHeader());
  m_mergeChannelShader.loadFromSourceFile("pass.vert",
                                          "image2d_array_compositor.frag",
                                          m_rendererBase.generateHeader() + generateHeader());
}

void Z3DVolumeSliceRenderer::setData(const std::vector<std::unique_ptr<Z3DVolume>>& vols,
                                     const std::vector<std::unique_ptr<ZColorMapParameter>>& colormaps)
{
  CHECK(colormaps.size() >= vols.size() && !vols.empty() && vols[0]->is3DData());

  m_vols = &vols;
  m_colormaps = &colormaps;

  if (m_vols->size() != m_volumeUniformNames.size()) {
    compile();
    m_volumeUniformNames.resize(m_vols->size());
    m_colormapUniformNames.resize(m_vols->size());
    for (size_t i = 0; i < m_vols->size(); ++i) {
      m_volumeUniformNames[i] = fmt::format("volume_{}", i + 1);
      m_colormapUniformNames[i] = fmt::format("colormap_{}", i + 1);
    }
  }
}

void Z3DVolumeSliceRenderer::addQuad(const ZMesh& quad)
{
  if (quad.empty() || (quad.numVertices() != 4 && quad.numVertices() != 6) ||
      quad.numVertices() != quad.num3DTextureCoordinates()) {
    LOG(FATAL) << "Input quad should be 2D slice with 3D texture coordinates";
    return;
  }
  m_quads.push_back(quad);
}

void Z3DVolumeSliceRenderer::bindVolumes(Z3DShaderProgram& shader) const
{
  size_t idx = 0;
  for (size_t i = 0; i < m_vols->size(); ++i) {
    // volumes
    shader.bindTexture(m_volumeUniformNames[idx], m_vols->at(i)->texture(), GLint(GL_NEAREST), GLint(GL_NEAREST));

    // colormap
    shader.bindTexture(m_colormapUniformNames[idx++], (*m_colormaps)[i]->get().texture1D());
  }
}

void Z3DVolumeSliceRenderer::bindVolume(Z3DShaderProgram& shader, size_t idx) const
{
  // volumes
  shader.bindTexture(m_volumeUniformNames[0], m_vols->at(idx)->texture(), GLint(GL_NEAREST), GLint(GL_NEAREST));

  // colormap
  shader.bindTexture(m_colormapUniformNames[0], (*m_colormaps)[idx]->get().texture1D());
}

void Z3DVolumeSliceRenderer::compile()
{
  // m_volumeSliceShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());

  m_scVolumeSliceShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  m_mergeChannelShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
}

std::string Z3DVolumeSliceRenderer::generateHeader()
{
  std::string header;
  header.reserve(128);

  if (m_vols && !m_vols->empty()) {
    fmt::format_to(std::back_inserter(header), "#define NUM_VOLUMES {}\n", m_vols->size());
  } else {
    absl::StrAppend(&header, "#define NUM_VOLUMES 0\n", "#define DISABLE_TEXTURE_COORD_OUTPUT\n");
  }

  absl::StrAppend(&header, "#define MAX_PROJ_MERGE\n");
  return header;
}

void Z3DVolumeSliceRenderer::render(Z3DEye eye)
{
  bool needRender = m_vols && !m_vols->empty() && !m_quads.empty();
  if (!needRender) {
    return;
  }

  m_scVolumeSliceShader.bind();
  m_rendererBase.setGlobalShaderParameters(m_scVolumeSliceShader, eye);

  if (m_vols->size() == 1) {
    bindVolume(m_scVolumeSliceShader, 0);
    for (const auto& quad : m_quads) {
      renderTriangleList(m_VAO, m_scVolumeSliceShader, quad);
    }
  } else {
    for (size_t j = 0; j < m_vols->size(); ++j) {
      m_layerTarget->attachSlice(j);
      m_layerTarget->bind();
      m_layerTarget->clear();

      bindVolume(m_scVolumeSliceShader, j);
      for (const auto& quad : m_quads) {
        renderTriangleList(m_VAO, m_scVolumeSliceShader, quad);
      }

      m_layerTarget->release();
    }
  }

  m_scVolumeSliceShader.release();

  if (m_vols->size() > 1) {
    m_mergeChannelShader.bind();
    m_mergeChannelShader.bindTexture("color_texture", m_layerTarget->attachment(GL_COLOR_ATTACHMENT0));
    m_mergeChannelShader.bindTexture("depth_texture", m_layerTarget->attachment(GL_DEPTH_ATTACHMENT));
    renderScreenQuad(m_VAO, m_mergeChannelShader);
    m_mergeChannelShader.release();
  }
}

} // namespace nim
