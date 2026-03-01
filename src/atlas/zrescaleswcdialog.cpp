#include "zrescaleswcdialog.h"

#include "zdoc.h"
#include "zexception.h"
#include "zmessageboxhelpers.h"
#include "zselectfilewidget.h"
#include "zswcdoc.h"
#include "zswc.h"
#include "zswcrescale.h"
#include "zsysteminfo.h"

#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QVBoxLayout>

namespace nim {

namespace {

QDoubleSpinBox* makeDoubleSpinBox(double min, double max, int decimals, double value, QWidget* parent)
{
  auto* sb = new QDoubleSpinBox(parent);
  sb->setDecimals(decimals);
  sb->setMinimum(min);
  sb->setMaximum(max);
  sb->setValue(value);
  return sb;
}

} // namespace

ZRescaleSwcDialog::ZRescaleSwcDialog(ZDoc& doc, QWidget* parent)
  : ZImgProcessDialog(doc, parent)
  , m_doc(doc)
{
  setWindowTitle(tr("Rescale SWC"));

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(10, 10, 10, 10);
  layout->setSpacing(10);

  const QString swcStartDir = ZSystemInfo::instance().lastOpenedObjPathQSettingLocation("Swc");

  auto* ioGroup = new QGroupBox(tr("Inputs and Output"), this);
  auto* ioLayout = new QVBoxLayout(ioGroup);

  m_inputSwcWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::OpenSingleFile,
                                           tr("Input SWC:"),
                                           ZSwc::getQtReadNameFilter(),
                                           swcStartDir,
                                           QString(),
                                           QBoxLayout::LeftToRight,
                                           ioGroup);
  ioLayout->addWidget(m_inputSwcWidget);

  m_outputSwcWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile,
                                            tr("Output SWC:"),
                                            ZSwc::getQtWriteNameFilter(),
                                            swcStartDir,
                                            QString(),
                                            QBoxLayout::LeftToRight,
                                            ioGroup);
  ioLayout->addWidget(m_outputSwcWidget);

  m_loadResultCheck = new QCheckBox(tr("Load output SWC into the scene when finished"), ioGroup);
  m_loadResultCheck->setChecked(true);
  ioLayout->addWidget(m_loadResultCheck);

  ioGroup->setLayout(ioLayout);
  layout->addWidget(ioGroup);

  auto* settingsGroup = new QGroupBox(tr("Rescale Settings"), this);
  auto* settingsLayout = new QVBoxLayout(settingsGroup);
  settingsLayout->setSpacing(10);

  {
    auto* preRow = new QHBoxLayout();
    preRow->setSpacing(6);

    preRow->addWidget(new QLabel(tr("Translate (pre-scale):"), settingsGroup));

    preRow->addWidget(new QLabel(tr("dx:"), settingsGroup));
    m_preTranslateX = makeDoubleSpinBox(-99999.99, 99999.99, 2, 0.0, settingsGroup);
    preRow->addWidget(m_preTranslateX);

    preRow->addWidget(new QLabel(tr("dy:"), settingsGroup));
    m_preTranslateY = makeDoubleSpinBox(-99999.99, 99999.99, 2, 0.0, settingsGroup);
    preRow->addWidget(m_preTranslateY);

    preRow->addWidget(new QLabel(tr("dz:"), settingsGroup));
    m_preTranslateZ = makeDoubleSpinBox(-99999.99, 99999.99, 2, 0.0, settingsGroup);
    preRow->addWidget(m_preTranslateZ);

    preRow->addStretch(1);
    settingsLayout->addLayout(preRow);
  }

  {
    auto* scaleGroup = new QGroupBox(tr("Scale"), settingsGroup);
    auto* scaleLayout = new QVBoxLayout(scaleGroup);

    m_scaleUseResolution = new QRadioButton(tr("Scale Use Resolution"), scaleGroup);
    m_scaleUseResolution->setChecked(true);
    scaleLayout->addWidget(m_scaleUseResolution);

    auto* resGrid = new QGridLayout();
    resGrid->setHorizontalSpacing(8);
    resGrid->setVerticalSpacing(6);

    resGrid->addWidget(new QLabel(tr("Current SWC Resolution x-y:"), scaleGroup), 0, 0);
    m_currentResXY = makeDoubleSpinBox(0.00001, 999.99, 5, 1.0, scaleGroup);
    resGrid->addWidget(m_currentResXY, 0, 1);
    resGrid->addWidget(new QLabel(tr("μm/pixel"), scaleGroup), 0, 2);

    resGrid->addWidget(new QLabel(tr("Current SWC Resolution z:"), scaleGroup), 1, 0);
    m_currentResZ = makeDoubleSpinBox(0.00001, 999.99, 5, 1.0, scaleGroup);
    resGrid->addWidget(m_currentResZ, 1, 1);
    resGrid->addWidget(new QLabel(tr("μm/pixel"), scaleGroup), 1, 2);

    resGrid->addWidget(new QLabel(tr("Target Resolution x-y:"), scaleGroup), 2, 0);
    m_targetResXY = makeDoubleSpinBox(0.00001, 999.99, 5, 1.0, scaleGroup);
    resGrid->addWidget(m_targetResXY, 2, 1);
    resGrid->addWidget(new QLabel(tr("μm/pixel"), scaleGroup), 2, 2);

    resGrid->addWidget(new QLabel(tr("Target Resolution z:"), scaleGroup), 3, 0);
    m_targetResZ = makeDoubleSpinBox(0.00001, 999.99, 5, 1.0, scaleGroup);
    resGrid->addWidget(m_targetResZ, 3, 1);
    resGrid->addWidget(new QLabel(tr("μm/pixel"), scaleGroup), 3, 2);

    scaleLayout->addLayout(resGrid);

    m_scaleManually = new QRadioButton(tr("Scale Manually"), scaleGroup);
    scaleLayout->addWidget(m_scaleManually);

    auto* manualRow = new QHBoxLayout();
    manualRow->setSpacing(6);

    manualRow->addWidget(new QLabel(tr("x:"), scaleGroup));
    m_scaleX = makeDoubleSpinBox(0.0001, 999.99, 4, 1.0, scaleGroup);
    manualRow->addWidget(m_scaleX);

    manualRow->addWidget(new QLabel(tr("y:"), scaleGroup));
    m_scaleY = makeDoubleSpinBox(0.0001, 999.99, 4, 1.0, scaleGroup);
    manualRow->addWidget(m_scaleY);

    manualRow->addWidget(new QLabel(tr("z:"), scaleGroup));
    m_scaleZ = makeDoubleSpinBox(0.0001, 999.99, 4, 1.0, scaleGroup);
    manualRow->addWidget(m_scaleZ);

    manualRow->addStretch(1);
    scaleLayout->addLayout(manualRow);

    m_scaleRadiusCheck = new QCheckBox(tr("Scale radii (XY)"), scaleGroup);
    m_scaleRadiusCheck->setToolTip(tr("When enabled, node radii are scaled by sqrt(scaleX * scaleY)."));
    m_scaleRadiusCheck->setChecked(true);
    scaleLayout->addWidget(m_scaleRadiusCheck);

    scaleGroup->setLayout(scaleLayout);
    settingsLayout->addWidget(scaleGroup);

    connect(m_scaleUseResolution, &QRadioButton::clicked, this, [this] {
      updateScaleUi();
    });
    connect(m_scaleManually, &QRadioButton::clicked, this, [this] {
      updateScaleUi();
    });
  }

  {
    auto* postRow = new QHBoxLayout();
    postRow->setSpacing(6);

    postRow->addWidget(new QLabel(tr("Translate (post-scale):"), settingsGroup));

    postRow->addWidget(new QLabel(tr("dx:"), settingsGroup));
    m_postTranslateX = makeDoubleSpinBox(-99999.99, 99999.99, 2, 0.0, settingsGroup);
    postRow->addWidget(m_postTranslateX);

    postRow->addWidget(new QLabel(tr("dy:"), settingsGroup));
    m_postTranslateY = makeDoubleSpinBox(-99999.99, 99999.99, 2, 0.0, settingsGroup);
    postRow->addWidget(m_postTranslateY);

    postRow->addWidget(new QLabel(tr("dz:"), settingsGroup));
    m_postTranslateZ = makeDoubleSpinBox(-99999.99, 99999.99, 2, 0.0, settingsGroup);
    postRow->addWidget(m_postTranslateZ);

    postRow->addStretch(1);
    settingsLayout->addLayout(postRow);
  }

  settingsGroup->setLayout(settingsLayout);
  layout->addWidget(settingsGroup);

  layout->addWidget(createButtonBox(tr("Rescale"), tr("Cancel")));

  updateScaleUi();
}

void ZRescaleSwcDialog::updateScaleUi()
{
  CHECK(m_scaleUseResolution != nullptr);
  CHECK(m_scaleManually != nullptr);

  const bool useRes = m_scaleUseResolution->isChecked();
  const bool useManual = m_scaleManually->isChecked();
  CHECK(useRes != useManual);

  CHECK(m_currentResXY != nullptr);
  CHECK(m_currentResZ != nullptr);
  CHECK(m_targetResXY != nullptr);
  CHECK(m_targetResZ != nullptr);
  m_currentResXY->setEnabled(useRes);
  m_currentResZ->setEnabled(useRes);
  m_targetResXY->setEnabled(useRes);
  m_targetResZ->setEnabled(useRes);

  CHECK(m_scaleX != nullptr);
  CHECK(m_scaleY != nullptr);
  CHECK(m_scaleZ != nullptr);
  m_scaleX->setEnabled(useManual);
  m_scaleY->setEnabled(useManual);
  m_scaleZ->setEnabled(useManual);
}

QString ZRescaleSwcDialog::inputSwcPath() const
{
  CHECK(m_inputSwcWidget != nullptr);
  return m_inputSwcWidget->getSelectedOpenFile();
}

QString ZRescaleSwcDialog::outputSwcPath() const
{
  CHECK(m_outputSwcWidget != nullptr);
  return m_outputSwcWidget->getSelectedSaveFile();
}

bool ZRescaleSwcDialog::loadResultEnabled() const
{
  CHECK(m_loadResultCheck != nullptr);
  return m_loadResultCheck->isChecked();
}

ZImgProcessDialog::WorkerSpec ZRescaleSwcDialog::createWorkerSpec()
{
  const QString output = outputSwcPath();
  if (output.trimmed().isEmpty()) {
    throw ZException("Please select an output SWC file.");
  }

  const QString input = inputSwcPath();
  if (input.trimmed().isEmpty()) {
    throw ZException("Please select an input SWC file.");
  }

  SwcRescaleSettings settings;

  CHECK(m_preTranslateX != nullptr);
  CHECK(m_preTranslateY != nullptr);
  CHECK(m_preTranslateZ != nullptr);
  settings.preTranslateX = m_preTranslateX->value();
  settings.preTranslateY = m_preTranslateY->value();
  settings.preTranslateZ = m_preTranslateZ->value();

  CHECK(m_scaleUseResolution != nullptr);
  CHECK(m_scaleManually != nullptr);
  if (m_scaleUseResolution->isChecked()) {
    CHECK(m_targetResXY != nullptr);
    CHECK(m_currentResXY != nullptr);
    CHECK(m_targetResZ != nullptr);
    CHECK(m_currentResZ != nullptr);
    // UI values are in μm/pixel. SWC coordinates are in pixel space, so scaling to
    // a new pixel size uses current/target (inverse of pixel-per-μm semantics).
    settings.scaleX = m_currentResXY->value() / m_targetResXY->value();
    settings.scaleY = settings.scaleX;
    settings.scaleZ = m_currentResZ->value() / m_targetResZ->value();
  } else if (m_scaleManually->isChecked()) {
    CHECK(m_scaleX != nullptr);
    CHECK(m_scaleY != nullptr);
    CHECK(m_scaleZ != nullptr);
    settings.scaleX = m_scaleX->value();
    settings.scaleY = m_scaleY->value();
    settings.scaleZ = m_scaleZ->value();
  } else {
    CHECK(false) << "Scale method radio state is invalid.";
  }

  CHECK(m_postTranslateX != nullptr);
  CHECK(m_postTranslateY != nullptr);
  CHECK(m_postTranslateZ != nullptr);
  settings.postTranslateX = m_postTranslateX->value();
  settings.postTranslateY = m_postTranslateY->value();
  settings.postTranslateZ = m_postTranslateZ->value();

  CHECK(m_scaleRadiusCheck != nullptr);
  settings.scaleRadius = m_scaleRadiusCheck->isChecked();

  const bool loadResult = loadResultEnabled();

  WorkerSpec spec;
  spec.workerName = tr("Rescale SWC");
  spec.taskTitle = tr("Rescale SWC: %1 -> %2").arg(QFileInfo(input).fileName(), QFileInfo(output).fileName());
  spec.successMessage = tr("Rescale SWC finished.");
  spec.makeWorker = [input, output, settings]() -> std::unique_ptr<ZImgProcess> {
    auto worker = std::make_unique<ZSwcRescale>();
    worker->setInputSwcFilename(input);
    worker->setOutputSwcFilename(output);
    worker->setSettings(settings);
    return worker;
  };

  if (loadResult) {
    spec.onSuccessUi = [output](ZDoc& doc, ZBackgroundTask&) {
      QString err;
      const size_t id = doc.swcDoc().loadFile(output, err);
      if (id == 0) {
        showCriticalWithDetails(QApplication::activeWindow(),
                                QObject::tr("Can not load output SWC"),
                                QObject::tr("SWC: %1\n%2").arg(output, err));
      }
    };
  }

  return spec;
}

} // namespace nim
