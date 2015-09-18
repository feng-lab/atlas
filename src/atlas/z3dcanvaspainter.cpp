#include "z3dgl.h"
#include "z3dcanvaspainter.h"

#include "z3dcanvas.h"
#include "z3dtexture.h"
#include "QsLog.h"
#include "z3dgpuinfo.h"
#include "zexception.h"

#include <QImage>
#include <QImageWriter>
#include <QPainter>
#include <memory>

namespace nim {

Z3DCanvasPainter::Z3DCanvasPainter(Z3DGlobalParameters &globalParas, QObject *parent)
  : Z3DBoundedFilter(globalParas, parent)
  , m_textureCopyRenderer(m_rendererBase, Z3DTextureCopyRenderer::OutputColorOption::DivideByAlpha)
  , m_canvas(NULL)
  , m_inport("Image", false, InvalidMonoViewResult)
  , m_leftEyeInport("LeftEyeImage", false, InvalidLeftEyeResult)
  , m_rightEyeInport("RightEyeImage", false, InvalidRightEyeResult)
  , m_renderToImage(false)
  , m_renderToImageFilename("")
{
  addPort(m_inport);
  addPort(m_leftEyeInport);
  addPort(m_rightEyeInport);
}

Z3DCanvasPainter::~Z3DCanvasPainter()
{
  setCanvas(nullptr);
}

void Z3DCanvasPainter::process(Z3DEye eye)
{
  if (!m_canvas)
    return;

  Z3DRenderInputPort &currentInport = (eye == Z3DEye::Mono) ?
        m_inport : (eye == Z3DEye::Left) ? m_leftEyeInport : m_rightEyeInport;

  // render to image
  if (currentInport.isReady() && m_renderToImage) {
    try {
      renderInportToImage(m_renderToImageFilename, eye);
      if (eye == Z3DEye::Mono) {
        LINFO() << "Saved rendering (" << currentInport.size().x << "," <<
                   currentInport.size().y << ")" << "to file:" << m_renderToImageFilename;
      } else if (eye == Z3DEye::Right) {
        if (m_renderToImageType == Z3DScreenShotType::HalfSideBySideStereoView) {
          LINFO() << "Saved half sbs stereo rendering (" << currentInport.size().x << "," <<
                     currentInport.size().y << ")" << "to file:" << m_renderToImageFilename;
        } else {
          LINFO() << "Saved stereo rendering (" << currentInport.size().x << "x 2," <<
                     currentInport.size().y << ")" << "to file:" << m_renderToImageFilename;
        }
      }
    }
    catch (ZException const & e) {
      LERROR() << "Exception:" << e.what();
      m_renderToImageError = e.what();
    }
    if (eye == Z3DEye::Mono || eye == Z3DEye::Right) {
      m_renderToImage = false;
    }
    return;
  }

  // render to screen
  m_canvas->getGLFocus();
  glViewport(0, 0, m_canvas->physicalSize().x, m_canvas->physicalSize().y);
  if (eye == Z3DEye::Left)
    glDrawBuffer(GL_BACK_LEFT);
  else if (eye == Z3DEye::Right)
    glDrawBuffer(GL_BACK_RIGHT);
  if (currentInport.isReady()) {
    m_rendererBase.setViewport(m_canvas->physicalSize());
    m_textureCopyRenderer.setColorTexture(currentInport.colorTexture());
    m_textureCopyRenderer.setDepthTexture(currentInport.depthTexture());
    m_rendererBase.render(eye, m_textureCopyRenderer);
    CHECK_GL_ERROR;
  } else {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    CHECK_GL_ERROR;
  }
}

bool Z3DCanvasPainter::isReady(Z3DEye) const
{
  return true;
}

bool Z3DCanvasPainter::isValid(Z3DEye) const
{
  return false;
}

void Z3DCanvasPainter::updateSize()
{
  setOutputSize(m_canvas->physicalSize());
  emit requestUpstreamSizeChange(this);
}

void Z3DCanvasPainter::onCanvasResized(int w, int h)
{
  glm::ivec2 newsize(w, h);
  setOutputSize(newsize);
  emit requestUpstreamSizeChange(this);
}

void Z3DCanvasPainter::invalidate(InvalidationState inv)
{
  if (!m_locked) {
    m_locked = true;
    m_invalidationState |= inv;
    if (m_canvas) {
      m_canvas->updateAll();
    }
    m_locked = false;
  }
}

void Z3DCanvasPainter::setCanvas(Z3DCanvas *canvas)
{
  if (canvas == m_canvas)
    return;
  if (m_canvas)
    m_canvas->disconnect(this);

  m_canvas = canvas;
  //register at new canvas:
  if (m_canvas) {
    setOutputSize(m_canvas->physicalSize());
    emit requestUpstreamSizeChange(this);
    connect(m_canvas, SIGNAL(canvasSizeChanged(int,int)), this, SLOT(onCanvasResized(int,int)));
  }

  invalidate();
}

Z3DCanvas* Z3DCanvasPainter::canvas() const
{
  return m_canvas;
}

bool Z3DCanvasPainter::renderToImage(const QString &filename, Z3DScreenShotType sst)
{
  if (!m_canvas) {
    LWARN() << "no canvas assigned";
    m_renderToImageError = "No canvas assigned";
    return false;
  }

  // enable render-to-file on next process
  m_renderToImageFilename = filename;
  m_renderToImage = true;
  m_renderToImageError.clear();
  m_renderToImageType = sst;

  // force rendering pass
  if (m_canvas->format().stereo() && sst == Z3DScreenShotType::MonoView) {
    LERROR() << "impossible configuration";
    assert(false);
  }
  if (!m_canvas->format().stereo() && sst != Z3DScreenShotType::MonoView)
    m_canvas->setFakeStereoOnce();
  m_canvas->forceUpdate();

  return (m_renderToImageError.isEmpty());
}

bool Z3DCanvasPainter::renderToImage(const QString &filename, int width, int height, Z3DScreenShotType sst)
{
  if (!m_canvas) {
    LWARN() << "no canvas assigned";
    m_renderToImageError = "No canvas assigned";
    return false;
  }

  if (m_inport.numValidInputs() == 0) {
    QApplication::processEvents();
  }

  glm::ivec2 oldDimensions = m_inport.size();
  // resize texture container to desired image dimensions and propagate change
  m_canvas->getGLFocus();
  setOutputSize(glm::ivec2(width, height));
  emit requestUpstreamSizeChange(this);

  // render with adjusted viewport size
  bool success = renderToImage(filename, sst);

  // reset texture container dimensions from canvas size
  setOutputSize(oldDimensions);
  emit requestUpstreamSizeChange(this);

  return success;
}

void Z3DCanvasPainter::renderInportToImage(const QString &filename, Z3DEye eye)
{
  const Z3DTexture* tex = imageColorTexture(eye);
  if (!tex) {
    throw ZGLException("not ready to capture image");
  }
  GLenum dataFormat = GL_BGRA;
  GLenum dataType = GL_UNSIGNED_INT_8_8_8_8_REV;

  if (eye == Z3DEye::Mono) {
    // get color buffer content
    auto colorBuffer = std::make_unique<uint8_t[]>(tex->bypePerPixel(dataFormat, dataType) * tex->numPixels());
    tex->downloadTextureToBuffer(dataFormat, dataType, colorBuffer.get());
    QImage upsideDownImage(colorBuffer.get(), tex->width(), tex->height(),
                           QImage::Format_ARGB32_Premultiplied);
    QImage image = upsideDownImage.mirrored(false, true);
    QImageWriter writer(filename);
    writer.setCompression(1);
    if (!writer.write(image)) {
      throw ZIOException(writer.errorString());
    }
  } else if (eye == Z3DEye::Right) {
    const Z3DTexture* leftTex = imageColorTexture(Z3DEye::Left);
    if (!leftTex) {
      throw ZGLException("not ready to capture image");
    }
    auto colorBuffer = std::make_unique<uint8_t[]>(leftTex->bypePerPixel(dataFormat, dataType) * leftTex->numPixels());
    leftTex->downloadTextureToBuffer(dataFormat, dataType, colorBuffer.get());
    QImage sideBySideImage(tex->width() * 2, tex->height(), QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&sideBySideImage);
    painter.scale(1, -1);
    painter.translate(0, -tex->height());
    QImage upsideDownImageLeft(colorBuffer.get(), tex->width(), tex->height(),
                               QImage::Format_ARGB32_Premultiplied);
    painter.drawImage(0, 0, upsideDownImageLeft);
    tex->downloadTextureToBuffer(dataFormat, dataType, colorBuffer.get());
    QImage upsideDownImageRight(colorBuffer.get(), tex->width(), tex->height(),
                                QImage::Format_ARGB32_Premultiplied);
    painter.drawImage(tex->width(), 0, upsideDownImageRight);

    if (m_renderToImageType == Z3DScreenShotType::HalfSideBySideStereoView) {
      QImage halfSideBySideImage = sideBySideImage.scaled(
            tex->width(), tex->height(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
      QImageWriter writer(filename);
      writer.setCompression(1);
      if(!writer.write(halfSideBySideImage)) {
        throw ZIOException(writer.errorString());
      }
    } else {
      QImageWriter writer(filename);
      writer.setCompression(1);
      if(!writer.write(sideBySideImage)) {
        throw ZIOException(writer.errorString());
      }
    }
  }
}

void Z3DCanvasPainter::setOutputSize(glm::ivec2 size)
{
  m_inport.setExpectedSize(size);
  m_leftEyeInport.setExpectedSize(size);
  m_rightEyeInport.setExpectedSize(size);
  globalCameraPara().viewportChanged(size);
}

const Z3DTexture* Z3DCanvasPainter::imageColorTexture(Z3DEye eye) const
{
  if (eye == Z3DEye::Mono && m_inport.isReady())
    return m_inport.colorTexture();
  else if (eye == Z3DEye::Left && m_leftEyeInport.isReady())
    return m_leftEyeInport.colorTexture();
  else if (eye == Z3DEye::Right && m_rightEyeInport.isReady())
    return m_rightEyeInport.colorTexture();
  else
    return NULL;
}

const Z3DTexture* Z3DCanvasPainter::imageDepthTexture(Z3DEye eye) const
{
  if (eye == Z3DEye::Mono && m_inport.isReady())
    return m_inport.depthTexture();
  else if (eye == Z3DEye::Left && m_leftEyeInport.isReady())
    return m_leftEyeInport.depthTexture();
  else if (eye == Z3DEye::Right && m_rightEyeInport.isReady())
    return m_rightEyeInport.depthTexture();
  else
    return NULL;
}

QString Z3DCanvasPainter::renderToImageError() const
{
  return m_renderToImageError;
}

} // namespace nim
