#pragma once

#include "zglmutils.h"
#include "zoptionparameter.h"
#include <QMenu>
#include <QWidget>
#include <QRectF>
#include <memory>
#include <vector>

class QAction;
class QColor;
class QMouseEvent;
class QCheckBox;
class QLabel;
class QLayout;
class QDoubleSpinBox;
class QPushButton;
class QPainter;

namespace nim {

class ZClickableTransferFunctionLabel;
class Z3DTransferFunctionParameter;
class Z3DTransferFunction;
class ZImg;
class ZImgHistogramThread;

class Z3DTransferFunctionWidget : public QWidget
{
  Q_OBJECT

public:
  explicit Z3DTransferFunctionWidget(Z3DTransferFunctionParameter* tf,
                                     bool showHistogram = true,
                                     QString histogramNormalizeMethod = tr("Log"),
                                     QString xAxisText = tr("Intensity"),
                                     QString yAxisText = tr("Opacity"),
                                     QWidget* parent = nullptr);

  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void keyReleaseEvent(QKeyEvent* event) override;
  bool event(QEvent* e) override;

  // Hit-testing utility used by mouse/tooltip handlers.
  bool findkey(const QPoint& pos, size_t& index, bool& isLeftPart);

  [[nodiscard]] QSize minimumSizeHint() const override;
  [[nodiscard]] QSize sizeHint() const override;

  void setTransFunc(Z3DTransferFunctionParameter* tf);

  void setHistogramNormalizeMethod(const QString& method);
  void setHistogramVisible(bool v);
  void setHistogramData(std::vector<size_t> bins, size_t maxCount);
  void clearHistogram();
  void setHistogramPending(bool pending);

protected Q_SLOTS:
  void deleteKey();
  void changeCurrentColor();
  void changeCurrentIntensity();
  void changeCurrentOpacity();

protected:
  // Visual handle for a (possibly split) key used for hit-testing.
  struct KeyHandle
  {
    size_t keyIndex = 0;
    bool isLeftPart = false;
    QRectF rect;
  };

  void insertNewKey(const glm::dvec2& hit);
  void paintKeys(QPainter& paint);

  void showKeyContextMenu(QMouseEvent* event, size_t selectedKeyIndex);
  void showNoKeyContextMenu(QMouseEvent* event);

  // Relative [domain, opacity] coordinates <-> widget pixels.
  glm::dvec2 relativeToPixelCoordinates(const glm::dvec2& r) const;
  glm::dvec2 pixelToRelativeCoordinates(const glm::dvec2& p) const;
  QRectF plotRect() const;

  void hideKeyInfo();
  void showKeyInfo(const QPoint& pos, const glm::dvec2& values);

  void updateHistogram();

  [[nodiscard]] double keyIntensityToRealIntensity(double keyInten) const;
  [[nodiscard]] double realIntensityToKeyIntensity(double realInten) const;

protected:
  Z3DTransferFunctionParameter* m_transferFunction = nullptr;
  std::unique_ptr<QPixmap> m_histogramCache;

  // interaction state
  bool m_selectedLeftPart = true;
  bool m_dragging = false;

  // appearance
  int m_padding = 36;
  double m_splitFactor = 1.2;
  int m_keyCircleRadius = 5;
  glm::dvec2 m_xRange; // [domainMin, domainMax]
  glm::dvec2 m_yRange; // [0, 1] opacity

  QString m_xAxisText;
  QString m_yAxisText;

  QMenu m_keyContextMenu;
  QMenu m_noKeyContextMenu;
  QAction* m_deleteKeyAction = nullptr;
  QAction* m_changeIntensityAction = nullptr;
  QAction* m_changeOpacityAction = nullptr;

  bool m_showHistogram = true;
  QString m_histogramNormalizeMethod;

  std::vector<size_t> m_histogramBins;
  size_t m_histogramMaxCount = 0;
  bool m_histogramPending = false;

  std::vector<KeyHandle> m_keyHandles;
};

class Z3DTransferFunctionEditor : public QWidget
{
  Q_OBJECT

public:
  explicit Z3DTransferFunctionEditor(Z3DTransferFunctionParameter* para, QWidget* parent = nullptr);
  ~Z3DTransferFunctionEditor() override;

  void createWidgets();
  void createConnections();

protected Q_SLOTS:
  void changeHistogramNormalizeMethod();
  void updateFromTransferFunction();
  void imageChanged();
  void histogramComputationFinished();
  void stopHistogramThread();
  void domainMinSpinBoxChanged(double min);
  void domainMaxSpinBoxChanged(double max);
  void fitDomainToData();
  void reset();

protected:
  QLayout* createMappingLayout();

  Z3DTransferFunctionParameter* m_transferFunction = nullptr;
  std::shared_ptr<const ZImg> m_image;

  Z3DTransferFunctionWidget* m_transferFunctionWidget = nullptr;
  ZClickableTransferFunctionLabel* m_transferFunctionTexture = nullptr;

  ZBoolParameter m_showHistogram;
  ZStringIntOptionParameter m_histogramNormalizeMethod;
  QLabel* m_domainMinNameLabel = nullptr;
  QLabel* m_domainMaxNameLabel = nullptr;
  QDoubleSpinBox* m_domainMinSpinBox = nullptr;
  QDoubleSpinBox* m_domainMaxSpinBox = nullptr;
  QLabel* m_dataMinValueLabel = nullptr;
  QLabel* m_dataMaxValueLabel = nullptr;
  QLabel* m_dataMinNameLabel = nullptr;
  QLabel* m_dataMaxNameLabel = nullptr;
  QPushButton* m_fitDomainToDataButton = nullptr;
  QCheckBox* m_rescaleKeys = nullptr;

  double m_dataMinValue = 0.0;
  double m_dataMaxValue = 0.0;
  bool m_hasDataMinMax = false;

  std::unique_ptr<ZImgHistogramThread> m_histogramThread;
};

} // namespace nim
