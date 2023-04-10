#include "zanimationexportwidget.h"

#include "zselectfilewidget.h"
#include "ztheme.h"
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QLabel>
#include <QGroupBox>
#include <QScrollBar>
#include <QApplication>

namespace nim {

ZAnimationExportWidget::ZAnimationExportWidget(bool is2DAni, QWidget* parent)
  : QScrollArea(parent)
  , m_group(false)
  , m_captureStereoImage("Stereo", false)
  , m_stereoImageType("Stereo Type")
  , m_useWindowSize("Use Window Size", false)
  , m_customSize("Custom Image Size",
                 glm::ivec2(3840, 2160),
                 glm::ivec2(128, 128),
                 glm::ivec2(7680, 4320))
  , m_framePerSecond("Frames per Second", 30, 12, 60)
  , m_startTime("Start", 0.0, 0.0, 99999.0)
  , m_endTime("End", -1., -1., 99999.0)
  , m_is2DAnimation(is2DAni)
{
  m_customSize.setStyle("SPINBOX");
  std::vector<QString> names{"Width:", "Height:"};
  m_customSize.setNameForEachValue(names);
  m_stereoImageType.addOptions("Full Side-By-Side", "Half Side-By-Side");
  m_stereoImageType.select("Half Side-By-Side");
  m_framePerSecond.setStyle("SPINBOX");
  m_startTime.setStyle("SPINBOX");
  m_startTime.setDecimal(3);
  m_startTime.setSingleStep(.001);
  m_startTime.setSuffix(" secs");
  m_endTime.setStyle("SPINBOX");
  m_endTime.setDecimal(3);
  m_endTime.setSingleStep(.001);
  m_endTime.setSuffix(" secs");
  createWidget();
  connect(&m_captureStereoImage, &ZBoolParameter::valueChanged, this, &ZAnimationExportWidget::adjustWidget);
  connect(&m_useWindowSize, &ZBoolParameter::valueChanged, this, &ZAnimationExportWidget::updateImageSizeWidget);
  connect(m_captureButton, &QPushButton::clicked, this, &ZAnimationExportWidget::captureButtonPressed);
  adjustWidget();
  updateImageSizeWidget();
}

QSize ZAnimationExportWidget::sizeHint() const
{
  QSize res = QScrollArea::sizeHint();
  res.setWidth(res.width() + verticalScrollBar()->sizeHint().width());
  return res;
}

void ZAnimationExportWidget::captureButtonPressed()
{
  if (m_filenameWidget->getSelectedSaveFile().isEmpty()) {
    QMessageBox::critical(this, QApplication::applicationName(), "Output filename does not exist");
    return;
  }

  if (m_is2DAnimation) {
    if (m_useWindowSize.get()) {
      Q_EMIT export2DAnimation(m_filenameWidget->getSelectedSaveFile(),
                               m_framePerSecond.get(),
                               m_startTime.get(),
                               m_endTime.get());
    } else {
      glm::ivec2 size = m_customSize.get();
      Q_EMIT exportFixedSize2DAnimation(m_filenameWidget->getSelectedSaveFile(),
                                        m_framePerSecond.get(),
                                        m_startTime.get(),
                                        m_endTime.get(),
                                        size.x,
                                        size.y);
    }
  } else {
    Z3DScreenShotType sst;
    if (m_captureStereoImage.get()) {
      if (m_stereoImageType.isSelected("Half Side-By-Side")) {
        sst = Z3DScreenShotType::HalfSideBySideStereoView;
      } else {
        sst = Z3DScreenShotType::FullSideBySideStereoView;
      }
    } else {
      sst = Z3DScreenShotType::MonoView;
    }

    if (m_useWindowSize.get()) {
      Q_EMIT export3DAnimation(m_filenameWidget->getSelectedSaveFile(),
                               m_framePerSecond.get(),
                               m_startTime.get(),
                               m_endTime.get(),
                               sst);
    } else {
      glm::ivec2 size = m_customSize.get();
      Q_EMIT exportFixedSize3DAnimation(m_filenameWidget->getSelectedSaveFile(),
                                        m_framePerSecond.get(),
                                        m_startTime.get(),
                                        m_endTime.get(),
                                        size.x,
                                        size.y,
                                        sst);
    }
  }
}

void ZAnimationExportWidget::updateImageSizeWidget()
{
  m_customSize.setEnabled(!m_useWindowSize.get());
}

void ZAnimationExportWidget::adjustWidget()
{
  m_framePerSecond.setVisible(true);
  m_startTime.setVisible(true);
  m_endTime.setVisible(true);
  m_captureButton->setVisible(true);
  m_filenameWidget->setEnabled(true);
  m_stereoImageType.setVisible(m_captureStereoImage.get());
}

void ZAnimationExportWidget::createWidget()
{
  auto lo = new QVBoxLayout;

  QHBoxLayout* hlo;
  QWidget* wg;

  if (!m_is2DAnimation) {
    hlo = new QHBoxLayout;
    hlo->addWidget(m_captureStereoImage.createNameLabel());
    wg = m_captureStereoImage.createWidget();
    wg->setMinimumWidth(125);
    wg->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    hlo->addWidget(wg);
    lo->addLayout(hlo);

    hlo = new QHBoxLayout;
    hlo->addWidget(m_stereoImageType.createNameLabel());
    wg = m_stereoImageType.createWidget();
    wg->setMinimumWidth(125);
    wg->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    hlo->addWidget(wg);
    lo->addLayout(hlo);
  }

  hlo = new QHBoxLayout;
  hlo->addWidget(m_useWindowSize.createNameLabel());
  wg = m_useWindowSize.createWidget();
  wg->setMinimumWidth(125);
  wg->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
  hlo->addWidget(wg);
  lo->addLayout(hlo);

  hlo = new QHBoxLayout;
  hlo->addWidget(m_customSize.createNameLabel());
  wg = m_customSize.createWidget();
  wg->setMinimumWidth(125);
  wg->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
  hlo->addWidget(wg);
  lo->addLayout(hlo);

  hlo = new QHBoxLayout;
  hlo->addWidget(m_framePerSecond.createNameLabel());
  wg = m_framePerSecond.createWidget();
  wg->setMinimumWidth(125);
  wg->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
  hlo->addWidget(wg);
  lo->addLayout(hlo);

  hlo = new QHBoxLayout;
  hlo->addWidget(m_startTime.createNameLabel());
  wg = m_startTime.createWidget();
  wg->setMinimumWidth(125);
  wg->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
  hlo->addWidget(wg);
  lo->addLayout(hlo);

  hlo = new QHBoxLayout;
  hlo->addWidget(m_endTime.createNameLabel());
  wg = m_endTime.createWidget();
  wg->setMinimumWidth(125);
  wg->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
  hlo->addWidget(wg);
  lo->addLayout(hlo);

  hlo = new QHBoxLayout;
  int left;
  int top;
  int right;
  int bottom;
  hlo->getContentsMargins(&left, &top, &right, &bottom);
  // hlo->setContentsMargins(left+20, top, right, bottom);

  m_filenameWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile,
                                           "filename:",
                                           tr("Video File (*.mp4 *.mov)"),
                                           QString("Animation/exportPath"),
                                           QString(),
                                           QBoxLayout::LeftToRight,
                                           this);
  hlo->addWidget(m_filenameWidget);
  lo->addLayout(hlo);

  m_captureButton = new QPushButton(tr("Export"), this);
  m_captureButton->setIcon(ZTheme::instance().icon(ZTheme::CamcoderIcon));
  m_captureButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  lo->addWidget(m_captureButton, 0, Qt::AlignHCenter | Qt::AlignVCenter);

  auto resWgt = new QWidget(this);
  if (m_group) {
    m_groupBox = new QGroupBox(tr("capture"), this);
    m_groupBox->setLayout(lo);
    hlo = new QHBoxLayout;
    hlo->addWidget(m_groupBox);
    resWgt->setLayout(hlo);
  } else {
    resWgt->setLayout(lo);
  }

  setWidget(resWgt);
  ensureWidgetVisible(m_captureButton);
}

} // namespace nim
