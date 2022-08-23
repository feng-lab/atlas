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

class Z3DMeshFilter;

class ZClickableLabel : public QWidget
{
  Q_OBJECT

public:
  explicit ZClickableLabel(QWidget* parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags());

Q_SIGNALS:

  void clicked();

protected:
  void mousePressEvent(QMouseEvent* ev) override;

  bool event(QEvent* event) override;

  virtual bool getTip(const QPoint& p, QRect* r, QString* s) = 0;

  // default implement is Q_EMIT the signal
  virtual void labelClicked();
};

class ZClickableColorLabel : public ZClickableLabel
{
public:
  explicit ZClickableColorLabel(ZVec4Parameter* color,
                                QWidget* parent = nullptr,
                                Qt::WindowFlags f = Qt::WindowFlags());

  explicit ZClickableColorLabel(ZVec3Parameter* color,
                                QWidget* parent = nullptr,
                                Qt::WindowFlags f = Qt::WindowFlags());

  explicit ZClickableColorLabel(ZDVec4Parameter* color,
                                QWidget* parent = nullptr,
                                Qt::WindowFlags f = Qt::WindowFlags());

  explicit ZClickableColorLabel(ZDVec3Parameter* color,
                                QWidget* parent = nullptr,
                                Qt::WindowFlags f = Qt::WindowFlags());

protected:
  void paintEvent(QPaintEvent* e) override;

  [[nodiscard]] QSize minimumSizeHint() const override;

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
  explicit ZClickableColorMapLabel(ZColorMapParameter* colorMap,
                                   QWidget* parent = nullptr,
                                   Qt::WindowFlags f = Qt::WindowFlags());

protected:
  void paintEvent(QPaintEvent* e) override;

  [[nodiscard]] QSize minimumSizeHint() const override;

  ZColorMapParameter* m_colorMap;

  bool getTip(const QPoint& p, QRect* r, QString* s) override;
};

class ZClickableTransferFunctionLabel : public ZClickableLabel
{
public:
  explicit ZClickableTransferFunctionLabel(Z3DTransferFunctionParameter* transferFunc,
                                           QWidget* parent = nullptr,
                                           Qt::WindowFlags f = Qt::WindowFlags());

protected:
  void paintEvent(QPaintEvent* e) override;

  [[nodiscard]] QSize minimumSizeHint() const override;

  Z3DTransferFunctionParameter* m_transferFunction;

  bool getTip(const QPoint& p, QRect* r, QString* s) override;
};

class ZRegionViewSettingLabel : public ZClickableLabel
{
public:
  explicit ZRegionViewSettingLabel(ZROIFilter* roiFilter,
                                   QWidget* parent = nullptr,
                                   Qt::WindowFlags f = Qt::WindowFlags());

  static QSize staticMinimumSizeHint()
  {
    return QSize(50, 50);
  }

protected:
  void paintEvent(QPaintEvent* e) override;

  [[nodiscard]] QSize minimumSizeHint() const override
  {
    return staticMinimumSizeHint();
  }

  ZROIFilter* m_roiFilter;

  bool getTip(const QPoint& p, QRect* r, QString* s) override;
};

class Z3DRegionViewSettingLabel : public ZClickableLabel
{
public:
  explicit Z3DRegionViewSettingLabel(Z3DMeshFilter* meshFilter,
                                     QWidget* parent = nullptr,
                                     Qt::WindowFlags f = Qt::WindowFlags());

  static QSize staticMinimumSizeHint()
  {
    return QSize(40, 40);
  }

protected:
  void paintEvent(QPaintEvent* e) override;

  [[nodiscard]] QSize minimumSizeHint() const override
  {
    return staticMinimumSizeHint();
  }

  Z3DMeshFilter* m_meshFilter;

  bool getTip(const QPoint& p, QRect* r, QString* s) override;
};

} // namespace nim
