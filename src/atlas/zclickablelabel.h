#pragma once

#include <QLabel>

namespace nim {

class ZColorMapParameter;

class Z3DTransferFunctionParameter;

class ZVec4Parameter;

class ZVec3Parameter;

class ZDVec4Parameter;

class ZDVec3Parameter;

class ZROIFilter;

class ZClickableLabel : public QWidget
{
Q_OBJECT
public:
  explicit ZClickableLabel(QWidget* parent = 0, Qt::WindowFlags f = 0);

signals:

  void clicked();

protected:
  void mousePressEvent(QMouseEvent* ev) override;

  bool event(QEvent* event) override;

  virtual bool getTip(const QPoint& p, QRect* r, QString* s) = 0;

  // default implement is emit the signal
  virtual void labelClicked();
};

class ZClickableColorLabel : public ZClickableLabel
{
public:
  explicit ZClickableColorLabel(ZVec4Parameter* color, QWidget* parent = 0, Qt::WindowFlags f = 0);

  explicit ZClickableColorLabel(ZVec3Parameter* color, QWidget* parent = 0, Qt::WindowFlags f = 0);

  explicit ZClickableColorLabel(ZDVec4Parameter* color, QWidget* parent = 0, Qt::WindowFlags f = 0);

  explicit ZClickableColorLabel(ZDVec3Parameter* color, QWidget* parent = 0, Qt::WindowFlags f = 0);

protected:
  void paintEvent(QPaintEvent* e) override;

  QSize minimumSizeHint() const override;

  ZVec4Parameter* m_vec4Color = nullptr;
  ZVec3Parameter* m_vec3Color = nullptr;
  ZDVec4Parameter* m_dvec4Color = nullptr;
  ZDVec3Parameter* m_dvec3Color = nullptr;

  bool getTip(const QPoint& p, QRect* r, QString* s) override;

  void labelClicked() override;

private:
  QColor toQColor();

  void fromQColor(const QColor& col);
};

class ZClickableColorMapLabel : public ZClickableLabel
{
public:
  explicit ZClickableColorMapLabel(ZColorMapParameter* colorMap, QWidget* parent = nullptr,
                                   Qt::WindowFlags f = 0);

protected:
  void paintEvent(QPaintEvent* e) override;

  QSize minimumSizeHint() const override;

  ZColorMapParameter* m_colorMap;

  bool getTip(const QPoint& p, QRect* r, QString* s) override;
};

class ZClickableTransferFunctionLabel : public ZClickableLabel
{
public:
  explicit ZClickableTransferFunctionLabel(Z3DTransferFunctionParameter* transferFunc, QWidget* parent = nullptr,
                                           Qt::WindowFlags f = 0);

protected:
  void paintEvent(QPaintEvent* e) override;

  QSize minimumSizeHint() const override;

  Z3DTransferFunctionParameter* m_transferFunction;

  bool getTip(const QPoint& p, QRect* r, QString* s) override;
};

class ZRegionViewSettingLabel : public ZClickableLabel
{
public:
  explicit ZRegionViewSettingLabel(ZROIFilter* roiFilter, QWidget* parent = nullptr,
                                   Qt::WindowFlags f = 0);

protected:
  void paintEvent(QPaintEvent* e) override;

  QSize minimumSizeHint() const override;

  ZROIFilter* m_roiFilter;

  bool getTip(const QPoint& p, QRect* r, QString* s) override;
};

} // namespace nim

