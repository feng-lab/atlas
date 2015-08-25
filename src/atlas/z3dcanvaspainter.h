#ifndef Z3DCANVASPAINTER_H
#define Z3DCANVASPAINTER_H

#include "z3dboundedfilter.h"
#include "znumericparameter.h"
#include "zexception.h"
#include <QString>
#include "z3dtexturecopyrenderer.h"
#include "z3drenderport.h"

class Z3DTexture;

namespace nim {

class Z3DCanvas;

class Z3DCanvasPainter : public Z3DBoundedFilter
{
  Q_OBJECT
public:
  Z3DCanvasPainter(Z3DGlobalParameters &globalParas, QObject *parent = nullptr);
  ~Z3DCanvasPainter();

  virtual void invalidate(InvalidationState inv = InvalidAllResult) override;

  void setCanvas(Z3DCanvas* canvas);

  Z3DCanvas *canvas() const;

  const Z3DTexture* imageColorTexture(Z3DEye eye) const;
  const Z3DTexture* imageDepthTexture(Z3DEye eye) const;

  bool renderToImage(const QString &filename, Z3DScreenShotType sst);
  bool renderToImage(const QString &filename, int width, int height, Z3DScreenShotType sst);

  QString renderToImageError() const;

  void setLock(bool v) { m_locked = v; }

protected slots:
  void onCanvasResized(int w, int h);

protected:
  virtual void process(Z3DEye eye) override;

  virtual bool isReady(Z3DEye) const override;
  virtual bool isValid(Z3DEye eye) const override;

  virtual void updateSize() override;

  void renderInportToImage(const QString& filename, Z3DEye eye);

private:
  void setOutputSize(glm::ivec2 size);

  Z3DTextureCopyRenderer m_textureCopyRenderer;

  Z3DCanvas* m_canvas;
  Z3DRenderInputPort m_inport;
  Z3DRenderInputPort m_leftEyeInport;
  Z3DRenderInputPort m_rightEyeInport;
  bool m_renderToImage;
  QString m_renderToImageFilename;
  QString m_renderToImageError;
  Z3DScreenShotType m_renderToImageType;

  bool m_locked = false;
};

} // namespace nim

#endif // Z3DCANVASPAINTER_H
