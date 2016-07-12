#ifndef Z3DTRANSFERFUNCTIONEDITOR_H
#define Z3DTRANSFERFUNCTIONEDITOR_H

#include "zglmutils.h"
#include <QWidget>
#include <QMenu>
#include "zparameter.h"
#include "zoptionparameter.h"

class QAction;
class QColor;
class QMouseEvent;
class QCheckBox;
class QLabel;
class QLayout;
class QDoubleSpinBox;
class QPushButton;

namespace nim {

class ZClickableTransferFunctionLabel;
class Z3DVolume;
class Z3DTransferFunctionParameter;
class Z3DTransferFunction;

class Z3DTransferFunctionWidget : public QWidget
{
  Q_OBJECT
public:
  Z3DTransferFunctionWidget(Z3DTransferFunctionParameter* tf, bool showHistogram = true,
                            const QString &histogramNormalizeMethod = tr("Log"), QString xAxisText = tr("Intensity"),
                            QString yAxisText = tr("Opacity"), QWidget* parent = 0);

  virtual void paintEvent(QPaintEvent* event) override;

  virtual void mousePressEvent(QMouseEvent* event) override;

  virtual void mouseReleaseEvent(QMouseEvent* event) override;

  virtual void leaveEvent(QEvent *) override;

  virtual void mouseMoveEvent(QMouseEvent* event) override;

  virtual void mouseDoubleClickEvent(QMouseEvent* event) override;

  virtual void keyReleaseEvent(QKeyEvent* event) override;

  virtual bool event(QEvent *e) override;
  bool findkey(const QPoint &pos, size_t &index, bool &isLeftPart);

  virtual QSize minimumSizeHint() const override;

  virtual QSize sizeHint() const override;

  void setTransFunc(Z3DTransferFunctionParameter* tf);

  void setHistogramNormalizeMethod(const QString &method);
  void setHistogramVisible(bool v);
  void volumeChanged(Z3DVolume *volume);

protected:
  void deleteKey();
  void changeCurrentColor();
  void changeCurrentIntensity();
  void changeCurrentOpacity();

  // Creates a new key at the given position.
  void insertNewKey(glm::dvec2 &hit);

  // Paints all keys of the transfer function.
  void paintKeys(QPainter& paint);

  // Diplays the context menu at the given mouseposition
  // for the case of a keyselection.
  void showKeyContextMenu(QMouseEvent* event, size_t selectedKeyIndex);

  // Diplays the context menu at the given mouseposition
  // for the case of no keyselection.
  void showNoKeyContextMenu(QMouseEvent* event);

  // Relative coordinates to Pixel coordinates
  glm::dvec2 relativeToPixelCoordinates(glm::dvec2 r);

  // Pixel coordinates to Relative coordinates
  glm::dvec2 pixelToRelativeCoordinates(glm::dvec2 p);

  void hideKeyInfo();
  void showKeyInfo(QPoint pos, glm::dvec2 values);

  // Re-calculated the histogram
  void updateHistogram();

protected:
  Z3DTransferFunctionParameter* m_transferFunction;
  std::unique_ptr<QPixmap> m_histogramCache;

  // variables for interaction
  bool m_selectedLeftPart;            // when selected key is split, was the left part selected?
  bool m_dragging;                    // is the user dragging a key?

  // variables for appearance of widget
  int m_padding;           // additional border of the widget
  double m_splitFactor;     // offset between splitted keys
  int m_keyCircleRadius;
  glm::dvec2 m_xRange;      // range in x direction
  glm::dvec2 m_yRange;      // range in y direction

  QString m_xAxisText;     // caption of the x axis
  QString m_yAxisText;     // caption of the y axis

  QMenu m_keyContextMenu;   // context menu for right mouse click when a key is selected
  QMenu m_noKeyContextMenu; // context menu for right mouse click when no key is selected

  QAction* m_deleteKeyAction;
  QAction* m_changeIntensityAction;
  QAction* m_changeOpacityAction;

  bool m_showHistogram;
  QString m_histogramNormalizeMethod;

  Z3DVolume* m_volume;
};


class Z3DTransferFunctionEditor : public QWidget
{
  Q_OBJECT
public:
  Z3DTransferFunctionEditor(Z3DTransferFunctionParameter* para, QWidget* parent = 0);
  virtual ~Z3DTransferFunctionEditor();

  void createWidgets();
  void createConnections();

protected:
  void changeHistogramNormalizeMethod();
  void updateFromTransferFunction();
  void volumeChanged();
  void domainMinSpinBoxChanged(double value);
  void domainMaxSpinBoxChanged(double value);

  void fitDomainToData();

  void reset();

  QLayout* createMappingLayout();

protected:
  Z3DTransferFunctionParameter* m_transferFunction;

  Z3DVolume* m_volume;

  Z3DTransferFunctionWidget* m_transferFunctionWidget;
  ZClickableTransferFunctionLabel* m_transferFunctionTexture;

  ZBoolParameter m_showHistogram;
  ZStringIntOptionParameter m_histogramNormalizeMethod;
  QLabel* m_domainMinNameLabel;
  QLabel* m_domainMaxNameLabel;
  QDoubleSpinBox* m_domainMinSpinBox;
  QDoubleSpinBox* m_domainMaxSpinBox;
  QLabel* m_dataMinValueLabel;
  QLabel* m_dataMaxValueLabel;
  QLabel* m_dataMinNameLabel;
  QLabel* m_dataMaxNameLabel;
  QPushButton* m_fitDomainToDataButton;
  QCheckBox *m_rescaleKeys;
};

} // namespace nim

#endif // Z3DTRANSFERFUNCTIONEDITOR_H
