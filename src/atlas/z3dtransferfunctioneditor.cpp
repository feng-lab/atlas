#include "z3dtransferfunctioneditor.h"

#include "z3dtransferfunction.h"
#include "zclickablelabel.h"
#include "zimg.h"
#include "zlog.h"

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QThread>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace nim {

namespace {

double normalizeIntensity(double value, int bitsStored)
{
  if (bitsStored > 0 && bitsStored <= 16) {
    const double maxCode = static_cast<double>((1u << bitsStored) - 1u);
    if (maxCode > 0.0) {
      return value / maxCode;
    }
  }
  return value;
}

} // namespace

class ZImgHistogramThread : public QThread
{
public:
  ZImgHistogramThread(std::shared_ptr<const ZImg> image, int bitsStored, QObject* parent = nullptr)
    : QThread(parent)
    , m_image(std::move(image))
    , m_bitsStored(bitsStored)
  {}

  [[nodiscard]] const std::vector<size_t>& histogram() const
  {
    return m_histogram;
  }

  [[nodiscard]] size_t maxCount() const
  {
    return m_maxCount;
  }

  [[nodiscard]] double minValue() const
  {
    return m_minValue;
  }

  [[nodiscard]] double maxValue() const
  {
    return m_maxValue;
  }

protected:
  void run() override
  {
    if (!m_image) {
      return;
    }

    double minRaw = 0.0;
    double maxRaw = 0.0;
    m_image->computeMinMax(minRaw, maxRaw);
    m_minValue = minRaw;
    m_maxValue = maxRaw;

    size_t binCount = 0;
    if (m_bitsStored > 0 && m_bitsStored <= 16) {
      binCount = static_cast<size_t>(1) << m_bitsStored;
    }

    if (binCount == 0) {
      m_histogram = m_image->histogram();
    } else {
      m_histogram = m_image->histogram(binCount);
    }

    if (!m_histogram.empty()) {
      m_maxCount = *std::max_element(m_histogram.begin(), m_histogram.end());
    } else {
      m_maxCount = 0;
    }
  }

private:
  std::shared_ptr<const ZImg> m_image;
  int m_bitsStored;
  std::vector<size_t> m_histogram;
  size_t m_maxCount = 0;
  double m_minValue = 0.0;
  double m_maxValue = 0.0;
};

// -----------------------------------------------------------------------------//
// Z3DTransferFunctionWidget
// -----------------------------------------------------------------------------//

Z3DTransferFunctionWidget::Z3DTransferFunctionWidget(Z3DTransferFunctionParameter* tf,
                                                     bool showHistogram,
                                                     QString histogramNormalizeMethod,
                                                     QString xAxisText,
                                                     QString yAxisText,
                                                     QWidget* parent)
  : QWidget(parent)
  , m_transferFunction(tf)
  , m_histogramCache()
  , m_selectedLeftPart(true)
  , m_dragging(false)
  , m_padding(36)
  , m_splitFactor(1.2)
  , m_keyCircleRadius(5)
  , m_xRange(0.0, 1.0)
  , m_yRange(0.0, 1.0)
  , m_xAxisText(std::move(xAxisText))
  , m_yAxisText(std::move(yAxisText))
  , m_keyContextMenu(this)
  , m_noKeyContextMenu(this)
  , m_deleteKeyAction(nullptr)
  , m_changeIntensityAction(nullptr)
  , m_changeOpacityAction(nullptr)
  , m_showHistogram(showHistogram)
  , m_histogramNormalizeMethod(std::move(histogramNormalizeMethod))
{
  setObjectName("TransFuncMappingCanvas");
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);

  auto* changeColorAction = new QAction(tr("Change Color"), this);
  m_keyContextMenu.addAction(changeColorAction);
  connect(changeColorAction, &QAction::triggered, this, &Z3DTransferFunctionWidget::changeCurrentColor);

  m_changeOpacityAction = new QAction(tr("Change Opacity"), this);
  m_keyContextMenu.addAction(m_changeOpacityAction);
  connect(m_changeOpacityAction, &QAction::triggered, this, &Z3DTransferFunctionWidget::changeCurrentOpacity);

  m_changeIntensityAction = new QAction(tr("Change Intensity"), this);
  m_keyContextMenu.addAction(m_changeIntensityAction);
  connect(m_changeIntensityAction, &QAction::triggered, this, &Z3DTransferFunctionWidget::changeCurrentIntensity);

  m_deleteKeyAction = new QAction(tr("Delete Key"), this);
  m_keyContextMenu.addAction(m_deleteKeyAction);
  connect(m_deleteKeyAction, &QAction::triggered, this, &Z3DTransferFunctionWidget::deleteKey);

  connect(m_transferFunction,
          &Z3DTransferFunctionParameter::valueChanged,
          this,
          qOverload<>(&Z3DTransferFunctionWidget::update));

  setSizePolicy(QSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::MinimumExpanding));
}

QRectF Z3DTransferFunctionWidget::plotRect() const
{
  const int left = m_padding;
  const int right = width() - m_padding;
  const int top = m_padding;
  const int bottom = height() - m_padding;
  return QRectF(QPointF(left, top), QPointF(right, bottom));
}

glm::dvec2 Z3DTransferFunctionWidget::relativeToPixelCoordinates(const glm::dvec2& r) const
{
  const QRectF rect = plotRect();
  const double nx = (r.x - m_xRange[0]) / (m_xRange[1] - m_xRange[0]);
  const double ny = (r.y - m_yRange[0]) / (m_yRange[1] - m_yRange[0]);

  const double px = rect.left() + nx * rect.width();
  const double py = rect.bottom() - ny * rect.height();
  return glm::dvec2(px, py);
}

glm::dvec2 Z3DTransferFunctionWidget::pixelToRelativeCoordinates(const glm::dvec2& p) const
{
  const QRectF rect = plotRect();
  if (rect.width() <= 0.0 || rect.height() <= 0.0) {
    return glm::dvec2(0.0, 0.0);
  }

  const double nx = (p.x - rect.left()) / rect.width();
  const double ny = (rect.bottom() - p.y) / rect.height();

  const double rx = m_xRange[0] + nx * (m_xRange[1] - m_xRange[0]);
  const double ry = m_yRange[0] + ny * (m_yRange[1] - m_yRange[0]);
  return glm::dvec2(rx, ry);
}

void Z3DTransferFunctionWidget::updateHistogram()
{
  if (!m_showHistogram || m_histogramPending) {
    m_histogramCache.reset();
    return;
  }

  if (m_histogramBins.empty() || m_histogramMaxCount == 0) {
    m_histogramCache.reset();
    return;
  }

  if (m_histogramCache && m_histogramCache->size() == size()) {
    return;
  }

  m_histogramCache = std::make_unique<QPixmap>(size());
  m_histogramCache->fill(Qt::transparent);

  QPainter painter(m_histogramCache.get());
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(0, 40, 160, 120));

  const QRectF rect = plotRect();
  const size_t binCount = m_histogramBins.size();
  if (binCount == 0 || rect.width() <= 0.0 || rect.height() <= 0.0) {
    return;
  }

  for (size_t i = 0; i < binCount; ++i) {
    double value = 0.0;
    if (m_histogramMaxCount > 0) {
      const double bin = static_cast<double>(m_histogramBins[i]);
      const double maxBin = static_cast<double>(m_histogramMaxCount);
      if (m_histogramNormalizeMethod == "Log") {
        value = std::log(bin + 1.0) / std::log(maxBin + 1.0);
      } else {
        value = bin / maxBin;
      }
    }
    value = std::clamp(value, 0.0, 1.0);
    if (value <= 0.0) {
      continue;
    }

    const double x0f = static_cast<double>(i) / binCount;
    const double x1f = static_cast<double>(i + 1) / binCount;

    const glm::dvec2 topRel(m_xRange[0] + x0f * (m_xRange[1] - m_xRange[0]),
                            m_yRange[0] + value * (m_yRange[1] - m_yRange[0]));
    const glm::dvec2 bottomRel(m_xRange[0] + x1f * (m_xRange[1] - m_xRange[0]), m_yRange[0]);

    const glm::dvec2 p1 = relativeToPixelCoordinates(topRel);
    const glm::dvec2 p2 = relativeToPixelCoordinates(bottomRel);
    QRectF bar(QPointF(p1.x, p1.y), QPointF(p2.x, p2.y));
    painter.drawRect(bar.normalized());
  }
}

void Z3DTransferFunctionWidget::paintEvent(QPaintEvent* event)
{
  if (!m_transferFunction) {
    return;
  }

  event->accept();

  auto& tf = m_transferFunction->get();
  m_xRange = glm::dvec2(tf.domainMin(), tf.domainMax());
  m_yRange = glm::dvec2(0.0, 1.0);

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, false);
  painter.fillRect(rect(), Qt::white);

  if (m_showHistogram) {
    updateHistogram();
    if (m_histogramCache) {
      painter.drawPixmap(0, 0, *m_histogramCache);
    }
  }

  // grid
  painter.setPen(QColor(220, 220, 220));
  const QRectF plot = plotRect();

  const int gridLines = 10;
  for (int i = 0; i <= gridLines; ++i) {
    const double fx = static_cast<double>(i) / gridLines;
    const double fy = static_cast<double>(i) / gridLines;

    const double x = plot.left() + fx * plot.width();
    const double y = plot.bottom() - fy * plot.height();

    painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
  }

  // axes and labels
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(Qt::gray);
  painter.setBrush(Qt::NoBrush);
  painter.drawRect(plot);

  painter.setPen(Qt::gray);

  // x-axis label
  painter.drawText(QRectF(plot.left(),
                          plot.bottom() + 4.0,
                          plot.width(),
                          static_cast<double>(m_padding) * 0.6),
                   Qt::AlignHCenter | Qt::AlignVCenter,
                   m_xAxisText);

  // y-axis label and tick labels (0,1).
  painter.save();
  const double yAxisCenterY = plot.center().y();
  const double yAxisLabelX = plot.left() - 0.7 * m_padding;
  const double yLabelWidth = static_cast<double>(m_padding) * 4.0;
  const double yLabelHeight = static_cast<double>(m_padding) * 0.6;
  painter.translate(yAxisLabelX, yAxisCenterY);
  painter.rotate(-90.0);
  painter.drawText(QRectF(-yLabelWidth / 2.0,
                          -yLabelHeight / 2.0,
                          yLabelWidth,
                          yLabelHeight),
                   Qt::AlignHCenter | Qt::AlignVCenter,
                   m_yAxisText);
  painter.restore();

  painter.drawText(QRectF(0.0,
                          plot.top() - 2.0,
                          m_padding * 0.8,
                          m_padding * 0.6),
                   Qt::AlignRight | Qt::AlignTop,
                   QStringLiteral("1.0"));
  painter.drawText(QRectF(0.0,
                          plot.bottom() - 14.0,
                          m_padding * 0.8,
                          m_padding * 0.6),
                   Qt::AlignRight | Qt::AlignBottom,
                   QStringLiteral("0.0"));

  const double minIntensity = m_transferFunction->minIntensity();
  const double maxIntensity = m_transferFunction->maxIntensity();

  painter.drawText(QRectF(plot.left(),
                          height() - m_padding * 0.8,
                          m_padding * 3.0,
                          m_padding * 0.6),
                   Qt::AlignLeft | Qt::AlignTop,
                   QString::number(minIntensity, 'g', QLocale::FloatingPointShortest));
  painter.drawText(QRectF(plot.right() - m_padding * 3.0,
                          height() - m_padding * 0.8,
                          m_padding * 3.0,
                          m_padding * 0.6),
                   Qt::AlignRight | Qt::AlignTop,
                   QString::number(maxIntensity, 'g', QLocale::FloatingPointShortest));

  // mapping curve
  painter.setRenderHint(QPainter::Antialiasing, true);
  QPen curvePen(Qt::darkRed);
  curvePen.setWidthF(1.5);
  painter.setPen(curvePen);

  if (tf.numKeys() > 0) {
    glm::dvec2 prevPoint(0.0);
    bool hasPrev = false;

    for (size_t i = 0; i < tf.numKeys(); ++i) {
      const double intensity = tf.keyIntensity(i);
      const double alphaL = tf.keyFloatAlphaL(i);
      const glm::dvec2 point = relativeToPixelCoordinates(glm::dvec2(intensity, alphaL));

      if (hasPrev) {
        painter.drawLine(QPointF(prevPoint.x, prevPoint.y), QPointF(point.x, point.y));
      }
      prevPoint = point;
      hasPrev = true;

      if (tf.isKeySplit(i)) {
        const double alphaR = tf.keyFloatAlphaR(i);
        prevPoint = relativeToPixelCoordinates(glm::dvec2(intensity, alphaR));
      }
    }
  }

  paintKeys(painter);

  // outer border
  painter.setRenderHint(QPainter::Antialiasing, false);
  painter.setPen(Qt::lightGray);
  painter.setBrush(Qt::NoBrush);
  painter.drawRect(rect().adjusted(0, 0, -1, -1));
}

void Z3DTransferFunctionWidget::paintKeys(QPainter& painter)
{
  m_keyHandles.clear();

  if (!m_transferFunction) {
    return;
  }

  auto& tf = m_transferFunction->get();
  const size_t keyCount = tf.numKeys();
  if (keyCount == 0) {
    return;
  }

  const double radius = static_cast<double>(m_keyCircleRadius);

  for (size_t i = 0; i < keyCount; ++i) {
    const bool isSplit = tf.isKeySplit(i);
    const bool isSelected = tf.isKeySelected(i);

    // left or single part
    {
      const double alphaL = tf.keyFloatAlphaL(i);
      const glm::dvec2 rel(tf.keyIntensity(i), alphaL);
      const glm::dvec2 pos = relativeToPixelCoordinates(rel);

      QPen pen(Qt::darkGray);
      double radiusScale = 1.0;
      if (isSelected && (!isSplit || m_selectedLeftPart)) {
        pen.setWidth(3);
        pen.setColor(Qt::black);
        radiusScale = 1.3;
      }
      painter.setPen(pen);

      QColor color = tf.keyQColorL(i);
      color.setAlpha(255);
      painter.setBrush(color);

      if (isSplit) {
        const double width = m_splitFactor * radius * radiusScale * 2.0;
        QRectF rect(QPointF(pos.x - width * 0.5, pos.y - radius * radiusScale),
                    QSizeF(width, radius * radiusScale * 2.0));
        painter.drawPie(rect, 90 * 16, 180 * 16);
        m_keyHandles.push_back({i, true, rect});
      } else {
        QRectF rect(QPointF(pos.x - radius * radiusScale, pos.y - radius * radiusScale),
                    QSizeF(radius * radiusScale * 2.0, radius * radiusScale * 2.0));
        painter.drawEllipse(rect);
        m_keyHandles.push_back({i, false, rect});
      }
    }

    if (isSplit) {
      const double alphaR = tf.keyFloatAlphaR(i);
      const glm::dvec2 rel(tf.keyIntensity(i), alphaR);
      const glm::dvec2 pos = relativeToPixelCoordinates(rel);

      QPen pen(Qt::darkGray);
      double radiusScale = 1.0;
      if (isSelected && !m_selectedLeftPart) {
        pen.setWidth(3);
        pen.setColor(Qt::black);
        radiusScale = 1.3;
      }
      painter.setPen(pen);

      QColor color = tf.keyQColorR(i);
      color.setAlpha(255);
      painter.setBrush(color);

      const double width = m_splitFactor * radius * radiusScale * 2.0;
      QRectF rect(QPointF(pos.x - width * 0.5, pos.y - radius * radiusScale),
                  QSizeF(width, radius * radiusScale * 2.0));
      painter.drawPie(rect, 270 * 16, 180 * 16);
      m_keyHandles.push_back({i, false, rect});
    }
  }
}

bool Z3DTransferFunctionWidget::findkey(const QPoint& pos, size_t& index, bool& isLeftPart)
{
  for (const auto& handle : m_keyHandles) {
    if (handle.rect.contains(pos)) {
      index = handle.keyIndex;
      isLeftPart = handle.isLeftPart;
      return true;
    }
  }
  return false;
}

void Z3DTransferFunctionWidget::showNoKeyContextMenu(QMouseEvent* event)
{
  m_noKeyContextMenu.popup(event->globalPosition().toPoint());
}

void Z3DTransferFunctionWidget::showKeyContextMenu(QMouseEvent* event, size_t selectedKeyIndex)
{
  if (!m_transferFunction) {
    return;
  }

  auto& tf = m_transferFunction->get();
  if (selectedKeyIndex == 0 || selectedKeyIndex == tf.numKeys() - 1) {
    m_deleteKeyAction->setEnabled(false);
    m_changeIntensityAction->setEnabled(false);
  } else {
    m_deleteKeyAction->setEnabled(true);
    m_changeIntensityAction->setEnabled(true);
  }

  m_keyContextMenu.popup(event->globalPosition().toPoint());
}

void Z3DTransferFunctionWidget::mousePressEvent(QMouseEvent* event)
{
  event->accept();

  if (!m_transferFunction) {
    return;
  }

  auto& tf = m_transferFunction->get();
  tf.deselectAllKeys();

  size_t selectedKeyIndex = 0;
  bool isLeftPart = true;
  const bool hasKey = findkey(event->pos(), selectedKeyIndex, isLeftPart);
  if (hasKey) {
    tf.setKeySelected(selectedKeyIndex, true);
    m_selectedLeftPart = isLeftPart;
  }

  const glm::dvec2 hit = pixelToRelativeCoordinates(glm::dvec2(event->position().x(), event->position().y()));

  if (event->button() == Qt::RightButton) {
    if (hasKey) {
      showKeyContextMenu(event, selectedKeyIndex);
    } else {
      showNoKeyContextMenu(event);
    }
    return;
  }

  if (event->button() != Qt::LeftButton) {
    return;
  }

  glm::dvec2 clamped = hit;
  clamped.x = std::clamp(clamped.x, m_xRange[0], m_xRange[1]);
  clamped.y = std::clamp(clamped.y, m_yRange[0], m_yRange[1]);

  if (hasKey) {
    m_dragging = true;
    showKeyInfo(event->pos(), clamped);
    return;
  }

  // No key hit: insert a new one.
  insertNewKey(clamped);
  m_dragging = true;
  showKeyInfo(event->pos(), clamped);
}

void Z3DTransferFunctionWidget::mouseMoveEvent(QMouseEvent* event)
{
  event->accept();

  if (!m_dragging || !m_transferFunction) {
    return;
  }

  auto& tf = m_transferFunction->get();
  auto selected = tf.selectedKeyIndexes();
  if (selected.size() != 1) {
    return;
  }

  size_t idx = selected[0];

  glm::dvec2 hit = pixelToRelativeCoordinates(glm::dvec2(event->position().x(), event->position().y()));
  hit.x = std::clamp(hit.x, m_xRange[0], m_xRange[1]);
  hit.y = std::clamp(hit.y, m_yRange[0], m_yRange[1]);

  if (idx != 0 && idx + 1 != tf.numKeys()) {
    const double eps = 1e-7;
    const double leftBound = tf.keyIntensity(idx - 1) + eps;
    const double rightBound = tf.keyIntensity(idx + 1) - eps;
    hit.x = std::clamp(hit.x, leftBound, rightBound);
    tf.setKeyIntensity(idx, hit.x);
  }

  if (tf.isKeySplit(idx)) {
    if (m_selectedLeftPart) {
      tf.setKeyFloatAlphaL(idx, hit.y);
    } else {
      tf.setKeyFloatAlphaR(idx, hit.y);
    }
  } else {
    tf.setKeyFloatAlphaL(idx, hit.y);
  }

  showKeyInfo(event->pos(), hit);
}

void Z3DTransferFunctionWidget::mouseReleaseEvent(QMouseEvent* event)
{
  event->accept();
  if (event->button() == Qt::LeftButton) {
    m_dragging = false;
    hideKeyInfo();
  }
}

void Z3DTransferFunctionWidget::leaveEvent(QEvent* /*event*/)
{
  m_dragging = false;
  hideKeyInfo();
}

void Z3DTransferFunctionWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
  event->accept();
  if (event->button() == Qt::LeftButton) {
    changeCurrentColor();
  }
}

void Z3DTransferFunctionWidget::keyReleaseEvent(QKeyEvent* event)
{
  if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
    event->accept();
    deleteKey();
  }
}

bool Z3DTransferFunctionWidget::event(QEvent* e)
{
  if (e->type() == QEvent::ToolTip) {
    auto* helpEvent = static_cast<QHelpEvent*>(e);
    size_t index = 0;
    bool isLeft = true;
    QRect tipRect;
    QString tipText;

    if (findkey(helpEvent->pos(), index, isLeft) && m_transferFunction) {
      auto& tf = m_transferFunction->get();
      const double keyInten = tf.keyIntensity(index);
      const double realInten = keyIntensityToRealIntensity(keyInten);

      const glm::dvec2 rel(tf.keyIntensity(index), tf.keyFloatAlphaL(index));
      const glm::dvec2 pos = relativeToPixelCoordinates(rel);
      tipRect = QRect(static_cast<int>(pos.x - m_keyCircleRadius),
                      static_cast<int>(pos.y - m_keyCircleRadius),
                      m_keyCircleRadius * 2,
                      m_keyCircleRadius * 2);

      QColor color;
      double opacity = 0.0;
      if (m_transferFunction->get().isKeySplit(index)) {
        if (isLeft) {
          color = tf.keyQColorL(index);
          opacity = tf.keyFloatAlphaL(index);
          tipText = QStringLiteral("Key %1 Left\nIntensity: %2\nColor: %3\nOpacity: %4")
                      .arg(index + 1)
                      .arg(realInten)
                      .arg(color.name())
                      .arg(opacity);
        } else {
          color = tf.keyQColorR(index);
          opacity = tf.keyFloatAlphaR(index);
          tipText = QStringLiteral("Key %1 Right\nIntensity: %2\nColor: %3\nOpacity: %4")
                      .arg(index + 1)
                      .arg(realInten)
                      .arg(color.name())
                      .arg(opacity);
        }
      } else {
        color = tf.keyQColorL(index);
        opacity = tf.keyFloatAlphaL(index);
        tipText = QStringLiteral("Key %1\nIntensity: %2\nColor: %3\nOpacity: %4")
                    .arg(index + 1)
                    .arg(realInten)
                    .arg(color.name())
                    .arg(opacity);
      }

      QToolTip::showText(helpEvent->globalPos(), tipText, this, tipRect);
    } else {
      QToolTip::hideText();
    }
    return true;
  }
  return QWidget::event(e);
}

QSize Z3DTransferFunctionWidget::minimumSizeHint() const
{
  return QSize(300, 185);
}

QSize Z3DTransferFunctionWidget::sizeHint() const
{
  return QSize(300, 185);
}

void Z3DTransferFunctionWidget::hideKeyInfo()
{
  QToolTip::hideText();
}

void Z3DTransferFunctionWidget::showKeyInfo(const QPoint& pos, const glm::dvec2& values)
{
  const double realIntensity = keyIntensityToRealIntensity(values.x);
  QToolTip::showText(mapToGlobal(pos),
                     QStringLiteral("Intensity: %1\nOpacity: %2").arg(realIntensity).arg(values.y));
}

void Z3DTransferFunctionWidget::setHistogramData(std::vector<size_t> bins, size_t maxCount)
{
  m_histogramBins = std::move(bins);
  m_histogramMaxCount = maxCount;
  m_histogramPending = false;
  m_histogramCache.reset();
  update();
}

void Z3DTransferFunctionWidget::clearHistogram()
{
  m_histogramBins.clear();
  m_histogramMaxCount = 0;
  m_histogramPending = false;
  m_histogramCache.reset();
  update();
}

void Z3DTransferFunctionWidget::setHistogramPending(bool pending)
{
  if (m_histogramPending == pending) {
    return;
  }
  m_histogramPending = pending;
  if (pending) {
    m_histogramBins.clear();
    m_histogramMaxCount = 0;
    m_histogramCache.reset();
  }
  update();
}

void Z3DTransferFunctionWidget::setTransFunc(Z3DTransferFunctionParameter* tf)
{
  m_transferFunction = tf;
  update();
}

double Z3DTransferFunctionWidget::keyIntensityToRealIntensity(double keyInten) const
{
  const double dmin = m_transferFunction->get().domainMin();
  const double dmax = m_transferFunction->get().domainMax();
  const double minInten = m_transferFunction->minIntensity();
  const double maxInten = m_transferFunction->maxIntensity();
  return minInten + (keyInten - dmin) / (dmax - dmin) * (maxInten - minInten);
}

double Z3DTransferFunctionWidget::realIntensityToKeyIntensity(double realInten) const
{
  const double dmin = m_transferFunction->get().domainMin();
  const double dmax = m_transferFunction->get().domainMax();
  const double minInten = m_transferFunction->minIntensity();
  const double maxInten = m_transferFunction->maxIntensity();
  return dmin + (realInten - minInten) / (maxInten - minInten) * (dmax - dmin);
}

void Z3DTransferFunctionWidget::setHistogramNormalizeMethod(const QString& method)
{
  if (m_histogramNormalizeMethod != method) {
    m_histogramNormalizeMethod = method;
    m_histogramCache.reset();
    update();
  }
}

void Z3DTransferFunctionWidget::setHistogramVisible(bool v)
{
  if (v != m_showHistogram) {
    m_showHistogram = v;
    update();
  }
}

void Z3DTransferFunctionWidget::deleteKey()
{
  if (!m_transferFunction) {
    return;
  }

  auto& tf = m_transferFunction->get();
  if (tf.numKeys() < 3) {
    return;
  }

  const std::vector<size_t> selected = tf.selectedKeyIndexes();
  if (selected.size() != 1) {
    return;
  }

  const size_t idx = selected.front();
  if (idx == 0 || idx == tf.numKeys() - 1) {
    return;
  }

  tf.removeSelectedKeys();
}

void Z3DTransferFunctionWidget::changeCurrentColor()
{
  if (!m_transferFunction) {
    return;
  }

  auto& tf = m_transferFunction->get();
  const std::vector<size_t> selected = tf.selectedKeyIndexes();
  if (selected.size() != 1) {
    return;
  }

  const size_t idx = selected.front();

  const bool isSplit = tf.isKeySplit(idx);
  QColor currentColor = isSplit && !m_selectedLeftPart ? tf.keyQColorR(idx) : tf.keyQColorL(idx);

  QColor newColor = QColorDialog::getColor(currentColor, QApplication::activeWindow());
  if (!newColor.isValid()) {
    return;
  }

  // Preserve opacity: color changes should not move the key vertically.
  if (isSplit) {
    if (m_selectedLeftPart) {
      newColor.setAlpha(tf.keyAlphaL(idx));
      tf.setKeyColorL(idx, newColor);
    } else {
      newColor.setAlpha(tf.keyAlphaR(idx));
      tf.setKeyColorR(idx, newColor);
    }
  } else {
    newColor.setAlpha(tf.keyAlphaL(idx));
    tf.setKeyColorL(idx, newColor);
  }
}

void Z3DTransferFunctionWidget::changeCurrentIntensity()
{
  if (!m_transferFunction) {
    return;
  }

  auto& tf = m_transferFunction->get();
  const std::vector<size_t> selected = tf.selectedKeyIndexes();
  if (selected.size() != 1) {
    return;
  }

  const size_t idx = selected.front();
  if (idx == 0 || idx == tf.numKeys() - 1) {
    return;
  }

  const double current = tf.keyIntensity(idx);
  const double minAllowed = tf.keyIntensity(idx - 1);
  const double maxAllowed = tf.keyIntensity(idx + 1);

  bool ok = false;
  const double newVal = QInputDialog::getDouble(this,
                                                tr("Change Intensity"),
                                                tr("Intensity in [0, 1]"),
                                                current,
                                                minAllowed,
                                                maxAllowed,
                                                4,
                                                &ok);
  if (!ok) {
    return;
  }

  const double eps = 1e-7;
  const double clamped = std::clamp(newVal, minAllowed + eps, maxAllowed - eps);
  tf.setKeyIntensity(idx, clamped);
}

void Z3DTransferFunctionWidget::changeCurrentOpacity()
{
  if (!m_transferFunction) {
    return;
  }

  auto& tf = m_transferFunction->get();
  const std::vector<size_t> selected = tf.selectedKeyIndexes();
  if (selected.size() != 1) {
    return;
  }

  const size_t idx = selected.front();
  const bool isSplit = tf.isKeySplit(idx);

  double currentOpacity = tf.keyFloatAlphaL(idx);
  if (isSplit && !m_selectedLeftPart) {
    currentOpacity = tf.keyFloatAlphaR(idx);
  }

  bool ok = false;
  const double newOpacity = QInputDialog::getDouble(this,
                                                    tr("Change Opacity"),
                                                    tr("Opacity in [0, 1]"),
                                                    currentOpacity,
                                                    0.0,
                                                    1.0,
                                                    3,
                                                    &ok);
  if (!ok) {
    return;
  }

  const double clamped = std::clamp(newOpacity, 0.0, 1.0);
  if (isSplit) {
    if (m_selectedLeftPart) {
      tf.setKeyFloatAlphaL(idx, clamped);
    } else {
      tf.setKeyFloatAlphaR(idx, clamped);
    }
  } else {
    tf.setKeyFloatAlphaL(idx, clamped);
  }
}

void Z3DTransferFunctionWidget::insertNewKey(const glm::dvec2& hit)
{
  if (!m_transferFunction) {
    return;
  }

  auto& tf = m_transferFunction->get();
  const double x = std::clamp(hit.x, m_xRange[0], m_xRange[1]);
  const double y = std::clamp(hit.y, m_yRange[0], m_yRange[1]);

  tf.addKeyAtIntensity(x, y, true);
}

// -----------------------------------------------------------------------------//
// Z3DTransferFunctionEditor
// -----------------------------------------------------------------------------//

Z3DTransferFunctionEditor::Z3DTransferFunctionEditor(Z3DTransferFunctionParameter* para, QWidget* parent)
  : QWidget(parent)
  , m_transferFunction(para)
  , m_showHistogram("Show Histogram: ", true)
  , m_histogramNormalizeMethod("Histogram Normalize Method: ")
{
  CHECK(m_transferFunction);

  setWindowFlag(Qt::Window, true);

  m_histogramNormalizeMethod.addOptions("Linear", "Log");
  m_histogramNormalizeMethod.select("Log");

  createWidgets();
  createConnections();
  updateFromTransferFunction();
}

Z3DTransferFunctionEditor::~Z3DTransferFunctionEditor()
{
  stopHistogramThread();
}

QLayout* Z3DTransferFunctionEditor::createMappingLayout()
{
  m_transferFunctionWidget =
    new Z3DTransferFunctionWidget(m_transferFunction, m_showHistogram.get(), m_histogramNormalizeMethod.get(), tr("Intensity"), tr("Opacity"));
  m_transferFunctionWidget->setMinimumWidth(140);

  // histogram controls
  auto* histogramRow = new QHBoxLayout();
  histogramRow->addWidget(m_showHistogram.createNameLabel());
  histogramRow->addWidget(m_showHistogram.createWidget());
  histogramRow->addStretch();
  histogramRow->addWidget(m_histogramNormalizeMethod.createNameLabel());
  histogramRow->addWidget(m_histogramNormalizeMethod.createWidget());

  // data bounds
  auto* dataRow = new QHBoxLayout();
  m_dataMinNameLabel = new QLabel(tr("Data Min: "), this);
  m_dataMinNameLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  m_dataMaxNameLabel = new QLabel(tr("Data Max: "), this);
  m_dataMaxNameLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  m_dataMinValueLabel = new QLabel(this);
  m_dataMaxValueLabel = new QLabel(this);
  m_dataMinValueLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
  m_dataMaxValueLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);

  dataRow->addSpacing(6);
  dataRow->addWidget(m_dataMinNameLabel);
  dataRow->addWidget(m_dataMinValueLabel);
  dataRow->addStretch();
  dataRow->addWidget(m_dataMaxNameLabel);
  dataRow->addWidget(m_dataMaxValueLabel);
  dataRow->addStretch();
  dataRow->addSpacing(21);

  // domain settings (normalized [0,1] range).
  auto* domainRow = new QHBoxLayout();
  m_domainMinSpinBox = new QDoubleSpinBox(this);
  m_domainMaxSpinBox = new QDoubleSpinBox(this);

  m_domainMinSpinBox->setRange(0.0, 0.999);
  m_domainMinSpinBox->setSingleStep(0.001);
  m_domainMinSpinBox->setDecimals(3);
  m_domainMinSpinBox->setKeyboardTracking(false);

  m_domainMaxSpinBox->setRange(0.001, 1.0);
  m_domainMaxSpinBox->setSingleStep(0.001);
  m_domainMaxSpinBox->setDecimals(3);
  m_domainMaxSpinBox->setKeyboardTracking(false);

  m_fitDomainToDataButton = new QPushButton(tr("Fit to Data"), this);

  m_domainMinNameLabel = new QLabel(tr("TF Start: "), this);
  m_domainMaxNameLabel = new QLabel(tr("TF End: "), this);
  m_domainMinNameLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  m_domainMaxNameLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

  m_rescaleKeys = new QCheckBox(tr("Rescale Keys"), this);
  m_rescaleKeys->setToolTip(tr("If enabled, keys are rescaled when the domain changes. "
                               "Otherwise, keys outside the new domain are removed."));
  m_rescaleKeys->setChecked(false);

  domainRow->addWidget(m_domainMinNameLabel);
  domainRow->addWidget(m_domainMinSpinBox);
  domainRow->addWidget(m_domainMaxNameLabel);
  domainRow->addWidget(m_domainMaxSpinBox);
  domainRow->addWidget(m_fitDomainToDataButton);
  domainRow->addWidget(m_rescaleKeys);

  // preview + reset row
  auto* bottomRow = new QHBoxLayout();
  m_transferFunctionTexture = new ZClickableTransferFunctionLabel(m_transferFunction, this);
  auto* resetButton = new QPushButton(tr("Reset"), this);
  connect(resetButton, &QPushButton::clicked, this, &Z3DTransferFunctionEditor::reset);

  bottomRow->addWidget(m_transferFunctionTexture, 1);
  bottomRow->addStretch();
  bottomRow->addWidget(resetButton);

  auto* mainLayout = new QVBoxLayout();
  mainLayout->setContentsMargins(0, 0, 0, 0);
  mainLayout->setSpacing(4);
  mainLayout->addWidget(m_transferFunctionWidget, 1);
  mainLayout->addLayout(histogramRow);
  mainLayout->addLayout(dataRow);
  mainLayout->addLayout(domainRow);
  mainLayout->addLayout(bottomRow);

  return mainLayout;
}

void Z3DTransferFunctionEditor::createWidgets()
{
  QLayout* layout = createMappingLayout();
  layout->setContentsMargins(5, 5, 5, 5);
  setLayout(layout);
}

void Z3DTransferFunctionEditor::createConnections()
{
  connect(m_transferFunction,
          &Z3DTransferFunctionParameter::valueChanged,
          this,
          &Z3DTransferFunctionEditor::updateFromTransferFunction);

  connect(m_domainMinSpinBox,
          qOverload<double>(&QDoubleSpinBox::valueChanged),
          this,
          &Z3DTransferFunctionEditor::domainMinSpinBoxChanged);
  connect(m_domainMaxSpinBox,
          qOverload<double>(&QDoubleSpinBox::valueChanged),
          this,
          &Z3DTransferFunctionEditor::domainMaxSpinBoxChanged);

  connect(m_fitDomainToDataButton, &QPushButton::clicked, this, &Z3DTransferFunctionEditor::fitDomainToData);
  connect(&m_showHistogram,
          &ZBoolParameter::boolChanged,
          m_transferFunctionWidget,
          &Z3DTransferFunctionWidget::setHistogramVisible);
  connect(&m_histogramNormalizeMethod,
          &ZStringIntOptionParameter::valueChanged,
          this,
          &Z3DTransferFunctionEditor::changeHistogramNormalizeMethod);
}

void Z3DTransferFunctionEditor::changeHistogramNormalizeMethod()
{
  m_transferFunctionWidget->setHistogramNormalizeMethod(m_histogramNormalizeMethod.get());
}

void Z3DTransferFunctionEditor::updateFromTransferFunction()
{
  CHECK(m_transferFunction);

  auto newImage = m_transferFunction->image();
  if (newImage != m_image) {
    m_image = std::move(newImage);
    imageChanged();
  }

  m_domainMinSpinBox->setValue(m_transferFunction->get().domainMin());
  m_domainMaxSpinBox->setValue(m_transferFunction->get().domainMax());
}

void Z3DTransferFunctionEditor::fitDomainToData()
{
  if (m_hasDataMinMax && m_dataMaxValue > m_dataMinValue) {
    const double minVal = m_dataMinValue;
    const double maxVal = std::max(m_dataMaxValue, m_dataMinValue + 0.001);
    m_transferFunction->get().setDomain(glm::dvec2(minVal, maxVal), m_rescaleKeys->isChecked());
  }
}

void Z3DTransferFunctionEditor::reset()
{
  if (m_transferFunction) {
    m_transferFunction->get().resetToDefault();
  }
}

void Z3DTransferFunctionEditor::domainMinSpinBoxChanged(double min)
{
  if (m_transferFunction->get().isValidDomainMin(min)) {
    m_transferFunction->get().setDomainMin(min, m_rescaleKeys->isChecked());
  } else {
    QMessageBox::critical(this, QApplication::applicationName(), tr("Invalid transfer function range start"));
    m_domainMinSpinBox->setValue(m_transferFunction->get().domainMin());
  }
}

void Z3DTransferFunctionEditor::domainMaxSpinBoxChanged(double max)
{
  if (m_transferFunction->get().isValidDomainMax(max)) {
    m_transferFunction->get().setDomainMax(max, m_rescaleKeys->isChecked());
  } else {
    QMessageBox::critical(this, QApplication::applicationName(), tr("Invalid transfer function range end"));
    m_domainMaxSpinBox->setValue(m_transferFunction->get().domainMax());
  }
}

void Z3DTransferFunctionEditor::stopHistogramThread()
{
  if (m_histogramThread) {
    if (m_histogramThread->isRunning()) {
      m_histogramThread->requestInterruption();
      m_histogramThread->wait();
    }
    m_histogramThread.reset();
  }
}

void Z3DTransferFunctionEditor::imageChanged()
{
  stopHistogramThread();

  if (!m_image) {
    m_dataMinValueLabel->setText(QStringLiteral("-"));
    m_dataMaxValueLabel->setText(QStringLiteral("-"));
    m_transferFunctionWidget->clearHistogram();
    m_hasDataMinMax = false;
    return;
  }

  const int bits = m_transferFunction->bitsStored();
  double minRaw = 0.0;
  double maxRaw = 0.0;
  m_image->computeMinMax(minRaw, maxRaw);

  m_dataMinValue = normalizeIntensity(minRaw, bits);
  m_dataMaxValue = normalizeIntensity(maxRaw, bits);
  m_hasDataMinMax = std::isfinite(m_dataMinValue) && std::isfinite(m_dataMaxValue);

  if (m_hasDataMinMax) {
    m_dataMinValueLabel->setText(QString::number(m_dataMinValue));
    m_dataMaxValueLabel->setText(QString::number(m_dataMaxValue));
  } else {
    m_dataMinValueLabel->setText(QStringLiteral("-"));
    m_dataMaxValueLabel->setText(QStringLiteral("-"));
  }

  m_transferFunctionWidget->setHistogramPending(true);

  m_histogramThread = std::make_unique<ZImgHistogramThread>(m_image, bits, this);
  connect(m_histogramThread.get(),
          &QThread::finished,
          this,
          &Z3DTransferFunctionEditor::histogramComputationFinished);
  m_histogramThread->start();
}

void Z3DTransferFunctionEditor::histogramComputationFinished()
{
  if (!m_histogramThread) {
    return;
  }

  const auto& binsRef = m_histogramThread->histogram();
  std::vector<size_t> bins(binsRef.begin(), binsRef.end());
  const size_t maxCount = m_histogramThread->maxCount();

  stopHistogramThread();

  if (bins.empty() || maxCount == 0) {
    m_transferFunctionWidget->clearHistogram();
    return;
  }

  m_transferFunctionWidget->setHistogramData(std::move(bins), maxCount);
}

} // namespace nim
