#include "z3dimgslicerenderer.h"

#include "z3dtexture.h"
#include "z3dimg.h"
#include "zbenchtimer.h"
#include "zlog.h"
#include <tbb/parallel_for.h>
#include <tbb/concurrent_unordered_set.h>
#include <QApplication>

namespace nim {

Z3DImgSliceRenderer::Z3DImgSliceRenderer(Z3DRendererBase& rendererBase)
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
  m_mergeChannelShader.loadFromSourceFile("pass.vert",
                                          "image2d_array_compositor.frag",
                                          m_rendererBase.generateHeader() + generateHeader());

  m_image3DSliceWithColorMapBlockIDsShader.bindFragDataLocation(0, "FragData0");
  // m_image3DSliceWithColorMapBlockIDsShader.bindFragDataLocation(1, "FragData1");
  m_image3DSliceWithColorMapBlockIDsShader.loadFromSourceFile("transform_with_3dtexture_and_eye_coordinate.vert",
                                                              "image3d_slice_with_transfun_blockID.frag",
                                                              m_rendererBase.generateHeader() + generateHeader());
  m_image3DSliceWithColorMapShader.bindFragDataLocation(0, "FragData0");
  m_image3DSliceWithColorMapShader.loadFromSourceFile("transform_with_3dtexture_and_eye_coordinate.vert",
                                                      "image3d_slice_with_colormap.frag",
                                                      m_rendererBase.generateHeader() + generateHeader());
  CHECK_GL_ERROR
}

void Z3DImgSliceRenderer::setData(Z3DImg& img, const std::vector<std::unique_ptr<ZColorMapParameter>>& colormaps)
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

void Z3DImgSliceRenderer::addSlice(const ZMesh& slice)
{
  if (slice.empty() || slice.numVertices() != slice.num3DTextureCoordinates()) {
    LOG(FATAL) << "Input slice should be plane triangles with 3D texture coordinates";
    return;
  }
  m_slices.push_back(slice);
}

void Z3DImgSliceRenderer::bindVolumes(Z3DShaderProgram& shader) const
{
  size_t idx = 0;
  for (size_t i = 0; i < m_img->numChannels(); ++i) {
    // volumes
    shader.bindTexture(m_volumeUniformNames[idx], m_img->volumes()[i]->texture(), GLint(GL_NEAREST), GLint(GL_NEAREST));

    // colormap
    shader.bindTexture(m_colormapUniformNames[idx++], (*m_colormaps)[i]->get().texture1D());

    CHECK_GL_ERROR
  }
}

void Z3DImgSliceRenderer::bindVolume(Z3DShaderProgram& shader, size_t idx) const
{
  // volumes
  shader.bindTexture(m_volumeUniformNames[0], m_img->volumes()[idx]->texture(), GLint(GL_NEAREST), GLint(GL_NEAREST));

  // colormap
  shader.bindTexture(m_colormapUniformNames[0], (*m_colormaps)[idx]->get().texture1D());

  CHECK_GL_ERROR
}

void Z3DImgSliceRenderer::compile()
{
  // m_volumeSliceShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());

  m_scVolumeSliceShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  m_mergeChannelShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());

  m_image3DSliceWithColorMapBlockIDsShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  m_image3DSliceWithColorMapShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
}

QString Z3DImgSliceRenderer::generateHeader()
{
  QString headerSource;

  size_t numLevels = 1;
  if (m_img) {
    numLevels = m_img->numLevels();
  }
  headerSource += QString("#define LEVEL_COUNT %1\n").arg(numLevels);

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

void Z3DImgSliceRenderer::render(Z3DEye eye)
{
  m_lastRenderingIsFastRendering = false;

  bool needRender = m_img && !m_slices.empty();
  if (!needRender) {
    return;
  }

  if (!m_fastRendering && m_img->isVolumeDownsampled()) {
    float n = m_rendererBase.camera().nearDist();
    glm::vec2 pixelEyeSpaceSize = m_rendererBase.camera().frustumNearPlaneSize() / glm::vec2(m_layerTarget->size());
    float ze_to_screen_pixel_voxel_size =
      -std::min(pixelEyeSpaceSize.x, pixelEyeSpaceSize.y) / n * m_rendererBase.globalParas().devicePixelRatio.get();

    LOG(INFO) << "";
    ZBenchTimer bt("render and collect blockids");

    if (m_blockIDsRenderTarget->attachment(GL_COLOR_ATTACHMENT0)->numPixels() * 4 != m_blockIDs.size()) {
      m_blockIDs.resize(m_blockIDsRenderTarget->attachment(GL_COLOR_ATTACHMENT0)->numPixels() * 4);
    }

    m_image3DSliceWithColorMapBlockIDsShader.bind();
    m_image3DSliceWithColorMapBlockIDsShader.setUniform("ze_to_screen_pixel_voxel_size", ze_to_screen_pixel_voxel_size);
    m_image3DSliceWithColorMapBlockIDsShader.setProjectionViewMatrixUniform(
      m_rendererBase.camera().projectionViewMatrix(eye));
    m_image3DSliceWithColorMapBlockIDsShader.setViewMatrixUniform(m_rendererBase.camera().viewMatrix(eye));

    // render block ids
    std::vector<uint32_t> missingBlockIDs;
    std::vector<uint32_t> usedBlockIDs;
    tbb::concurrent_unordered_set<uint32_t> ccSet;

    const GLenum g_drawBuffers[] = {GL_COLOR_ATTACHMENT0};

    m_img->bindFullResBlockIDsShader(m_image3DSliceWithColorMapBlockIDsShader);

    for (auto& slice : m_slices) {
      m_blockIDsRenderTarget->bind();
      glDrawBuffers(1, g_drawBuffers);
      glClear(GL_COLOR_BUFFER_BIT);

      renderTriangleList(m_VAO, m_image3DSliceWithColorMapBlockIDsShader, slice);

      m_blockIDsRenderTarget->release();

      m_blockIDsRenderTarget->attachment(GL_COLOR_ATTACHMENT0)
        ->downloadTextureToBuffer(GL_RGBA_INTEGER, GL_UNSIGNED_INT, m_blockIDs.data());
      tbb::parallel_for(tbb::blocked_range<std::vector<uint32_t>::iterator>(m_blockIDs.begin(), m_blockIDs.end()),
                        [&](const tbb::blocked_range<std::vector<uint32_t>::iterator>& range) {
                          ccSet.insert(range.begin(), range.end()); // inserts a sequence
                        });

      ccSet.unsafe_erase(0_u32);
      missingBlockIDs.insert(missingBlockIDs.end(), ccSet.begin(), ccSet.end());
      ccSet.clear();

      //      m_blockIDsRenderTarget->attachment(GL_COLOR_ATTACHMENT1)->downloadTextureToBuffer(GL_RGBA_INTEGER,
      //                                                                                        GL_UNSIGNED_INT,
      //                                                                                        m_blockIDs.data());
      //      tbb::parallel_for(
      //        tbb::blocked_range<std::vector<uint32_t>::iterator>(m_blockIDs.begin(), m_blockIDs.end()),
      //        [&](const tbb::blocked_range<std::vector<uint32_t>::iterator>& range) {
      //          ccSet.insert(range.begin(), range.end()); // inserts a sequence
      //        }
      //      );
      //      ccSet.unsafe_erase(0_u32);
      //      usedBlockIDs.insert(ccSet.begin(), ccSet.end());
      //      ccSet.clear();
    }

    m_image3DSliceWithColorMapBlockIDsShader.release();
    // glFinish();
    STOP_AND_LOG(bt)

    LOG(INFO) << missingBlockIDs.size() << " " << usedBlockIDs.size();

    if (!missingBlockIDs.empty()) {
      m_img->updateAndUploadPageDirectoryCaches(missingBlockIDs, usedBlockIDs);
    }

    bt.resetAndStart("render image3d slice");
    // render channels one by one
    m_image3DSliceWithColorMapShader.bind();

    m_image3DSliceWithColorMapShader.setUniform("ze_to_screen_pixel_voxel_size", ze_to_screen_pixel_voxel_size);
    m_image3DSliceWithColorMapShader.setProjectionViewMatrixUniform(m_rendererBase.camera().projectionViewMatrix(eye));
    m_image3DSliceWithColorMapShader.setViewMatrixUniform(m_rendererBase.camera().viewMatrix(eye));

    // macOS: if sets here, then the following rendering uses old page directory caches. no idea why
    // m_img->bindFullResRenderShader(m_image3DSliceWithColorMapShader);

    if (m_img->numChannels() == 1) {
      m_img->uploadImageCache(0);
      m_img->bindFullResRenderShader(m_image3DSliceWithColorMapShader);
      m_img->bindImageCacheToFullResRenderShader(m_image3DSliceWithColorMapShader, 0);
      m_image3DSliceWithColorMapShader.bindTexture("colormap", (*m_colormaps)[0]->get().texture1D());
      for (auto& slice : m_slices) {
        renderTriangleList(m_VAO, m_image3DSliceWithColorMapShader, slice);
      }
    } else {
      for (size_t i = 0; i < m_img->numChannels(); ++i) {
        m_layerTarget->attachSlice(i);

        //        if (i == 1) {
        //        m_layerTarget->saveAsColorImage("/Users/feng/Downloads/abcd_b.tif");
        //        }

        m_layerTarget->bind();
        m_layerTarget->clear();

        m_img->uploadImageCache(i);
        m_img->bindFullResRenderShader(m_image3DSliceWithColorMapShader);
        m_img->bindImageCacheToFullResRenderShader(m_image3DSliceWithColorMapShader, i);
        m_image3DSliceWithColorMapShader.bindTexture("colormap", (*m_colormaps)[i]->get().texture1D());
        for (auto& slice : m_slices) {
          renderTriangleList(m_VAO, m_image3DSliceWithColorMapShader, slice);
        }

        m_layerTarget->release();

        // if (i == 1) {
        // m_layerTarget->saveAsColorImage("/Users/feng/Downloads/abcd.tif");
        // }
      }
    }

    m_image3DSliceWithColorMapShader.release();
    // glFinish();
    STOP_AND_LOG(bt)
  } else {
    m_lastRenderingIsFastRendering = true;

    m_scVolumeSliceShader.bind();
    m_rendererBase.setGlobalShaderParameters(m_scVolumeSliceShader, eye);

    if (m_img->numChannels() == 1) {
      bindVolume(m_scVolumeSliceShader, 0);
      for (auto& slice : m_slices) {
        renderTriangleList(m_VAO, m_scVolumeSliceShader, slice);
      }
    } else {
      for (size_t j = 0; j < m_img->numChannels(); ++j) {
        m_layerTarget->attachSlice(j);
        m_layerTarget->bind();
        m_layerTarget->clear();

        bindVolume(m_scVolumeSliceShader, j);
        for (auto& slice : m_slices) {
          renderTriangleList(m_VAO, m_scVolumeSliceShader, slice);
        }

        m_layerTarget->release();
      }
    }

    m_scVolumeSliceShader.release();
  }

  if (m_img->numChannels() > 1) {
    m_mergeChannelShader.bind();
    m_mergeChannelShader.bindTexture("color_texture", m_layerTarget->attachment(GL_COLOR_ATTACHMENT0));
    m_mergeChannelShader.bindTexture("depth_texture", m_layerTarget->attachment(GL_DEPTH_ATTACHMENT));
    renderScreenQuad(m_VAO, m_mergeChannelShader);
    m_mergeChannelShader.release();
  }

  CHECK_GL_ERROR
}

} // namespace nim
