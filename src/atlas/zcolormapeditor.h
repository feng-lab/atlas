#pragma once

#include <QWidget>

class QLabel;

class QDoubleSpinBox;

class QPushButton;

class QCheckBox;

namespace nim {

class ZColorMapParameter;

class ZColorMapWidget : public QWidget
{
Q_OBJECT
public:
  explicit ZColorMapWidget(ZColorMapParameter* colorMap, QWidget* parent = nullptr);

  void editLColor(size_t index);

  void editRColor(size_t index);

protected:
  void updateIntensityScreenWidth();

  void mousePressEvent(QMouseEvent* e) override;

  void mouseMoveEvent(QMouseEvent* e) override;

  void mouseReleaseEvent(QMouseEvent* e) override;

  void mouseDoubleClickEvent(QMouseEvent* e) override;

  void contextMenuEvent(QContextMenuEvent* e) override;

  void keyPressEvent(QKeyEvent* e) override;

  bool event(QEvent* e) override;

  void paintEvent(QPaintEvent* /*event*/) override;

  void resizeEvent(QResizeEvent* /*event*/) override;

  [[nodiscard]] QSize sizeHint() const override;

  enum FindKeyResult
  {
    NONE,
    LEFT,
    RIGHT
  };

  FindKeyResult findkey(const QPoint& pos, size_t& index, bool includeBoundKey = false) const;

  [[nodiscard]] double intensityToScreenXPosition(double intensity) const;

  [[nodiscard]] double screenXPositionToIntensity(double x) const;

  [[nodiscard]] QRect sliderBounds(size_t index) const;

private:
  ZColorMapParameter* m_colorMap;

  int m_sliderWidth;
  int m_sliderHeight;
  double m_intensityScreenWidth;
  bool m_isDragging;
  int m_dragStartX;
  size_t m_pressedIndex;
  int m_pressedPosX;
};

class ZColorMapEditor : public QWidget
{
Q_OBJECT
public:
  explicit ZColorMapEditor(ZColorMapParameter* colorMap, QWidget* parent = nullptr);

protected:
  void updateFromColorMap();

  void setDomainMin(double min);

  void setDomainMax(double max);

  void fitDomainToDataRange();

private:
  void createWidget();

private:
  ZColorMapParameter* m_colorMap;
  ZColorMapWidget* m_colorMapWidget;
  QLabel* m_dataMinNameLabel;
  QLabel* m_dataMinValueLabel;
  QLabel* m_dataMaxNameLabel;
  QLabel* m_dataMaxValueLabel;
  QLabel* m_domainMinNameLabel;
  QLabel* m_domainMaxNameLabel;
  QDoubleSpinBox* m_domainMinSpinBox;
  QDoubleSpinBox* m_domainMaxSpinBox;
  QPushButton* m_fitDomainToDataButton;
  QCheckBox* m_rescaleKeys;
};

} // namespace nim

