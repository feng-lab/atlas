#include "zsectionsregistrationdialog.h"

#include "zselectfilewidget.h"
#include "zsectionsregistration.h"
#include "zstringutils.h"
#include "zsysteminfo.h"
#include <QApplication>
#include <QVBoxLayout>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMessageBox>
#include <QThread>

namespace nim {

ZSectionsRegistrationDialog::ZSectionsRegistrationDialog(QWidget* parent)
  : ZImgProcessDialog(parent)
  , m_useCurrentActiveImage("Use Current Active Image", false)
  , m_openLoadedStack("Open Original Image Sequence", true)
  , m_openStackAfterRegistering("Open Result Image After Registering", true)
  , m_referenceChannel("Reference Channel:")
  , m_referenceImageIndex("Reference Image Index", 0, 0, 90000)
  , m_removeBackground("Remove Background", true)
  , m_removeHighForeground("Remove High Foreground", true)
  , m_numScales("Number of Scales", 3, 1, 5)
  , m_numNeighbors("Number of Neighbors", 1, 1, 50)
  , m_allowFlip("Allow Flip", false)
  , m_brightBackground("Background is Brighter", false)
  , m_metric("Metric")
  , m_transform("Transform")
  , m_optimizer("Optimizer")
{
  m_referenceChannel.clearOptions();
  m_referenceChannel.addOptionWithData(std::make_pair<QString, int>("Auto", 0));
  m_referenceChannel.select("Auto");
  init();
}

void ZSectionsRegistrationDialog::createWorker(ZImgProcess*& worker, QString& workerName)
{
  focusNextChild();

  if (m_inputImagesFileWidget->getSelectedMultipleOpenFiles().isEmpty()) {
    throw ZException(QString("No input images. Abort."));
  }
  if (m_outputStackWidget->getSelectedSaveFile().isEmpty()) {
    throw ZException(QString("Result image file must be specified."));
  }
  if (m_outputLogFileWidget->getSelectedSaveFile().isEmpty()) {
    throw ZException(QString("Registration log file must be specified."));
  }

  int refChannel = m_referenceChannel.associatedData() - 1;

  auto workertmp = new ZSectionsRegistration(m_inputImagesFileWidget->getSelectedMultipleOpenFiles(),
                                             m_outputStackWidget->getSelectedSaveFile(),
                                             m_referenceImageIndex.get());
  if (refChannel >= 0) {
    workertmp->setReferenceChannel(refChannel);
  }
  workertmp->setRemoveBackground(m_removeBackground.get());
  workertmp->setRemoveHighForeground(m_removeHighForeground.get());
  workertmp->setAllowFlip(m_allowFlip.get());
  workertmp->setBrightBackground(m_brightBackground.get());
  workertmp->setMetric(m_metric.get());
  workertmp->setTransform(m_transform.get());
  workertmp->setOptimizer(m_optimizer.get());
  workertmp->setLogFile(m_outputLogFileWidget->getSelectedSaveFile());
  workertmp->setNumScales(m_numScales.get());
  workertmp->setNumNeighbors(m_numNeighbors.get());
  if (m_openStackAfterRegistering.get()) {
    connect(workertmp, &ZSectionsRegistration::resultReady, this, &ZSectionsRegistrationDialog::resultReady);
  }

  worker = workertmp;
  workerName = "Sections Registration";
}

void ZSectionsRegistrationDialog::adjustInputImageWidget()
{
  m_inputImagesFileWidget->setVisible(!m_useCurrentActiveImage.get());
  m_openLoadedStack.setVisible(!m_useCurrentActiveImage.get());
}

void ZSectionsRegistrationDialog::inputImagesChanged()
{
  QStringList fns = m_inputImagesFileWidget->getSelectedMultipleOpenFiles();
  if (fns.empty()) {
    return;
  }

  QString fn = fns[0];
  QFileInfo fi(fn);
  QString logFn = fi.path() + "/" + replaceLastInteger(fi.baseName(), "_all") + "_sections_registration_log.txt";
  m_outputLogFileWidget->setFile(logFn);
  QString stackFn = fi.path() + "/" + replaceLastInteger(fi.baseName(), "_all") + "_aligned_stack.nim";
  m_outputStackWidget->setFile(stackFn);

  size_t channelNumber;
  size_t numFrames;
  try {
    std::vector<ZImgInfo> info = ZImg::readImgInfos(fns, Dimension::Z, true, nullptr, FileFormat::Unknown, true);
    if (info.size() != 1 || info[0].isEmpty()) {
      throw ZIOException("Not supported image dimensions");
    }
    channelNumber = info[0].numChannels;
    numFrames = info[0].depth;
  }
  catch (const ZIOException& e) {
    QMessageBox::critical(this,
                          QApplication::applicationName(),
                          QString("Can not parse input image:\n%1").arg(e.what()));
    return;
  }

  m_referenceChannel.clearOptions();
  m_referenceChannel.addOptionWithData(std::make_pair<QString, int>("Auto", 0));
  for (size_t i = 0; i < channelNumber; ++i) {
    m_referenceChannel.addOptionWithData(std::make_pair(QString("Ch%1").arg(i + 1), i + 1));
  }
  m_referenceChannel.select("Auto");

  m_referenceImageIndex.setRange(0, numFrames - 1);
}

void ZSectionsRegistrationDialog::init()
{
  m_metric.addOptions("Mean Differences",
                      "Mean Squared Differences",
                      "Log Absolute Differences",
                      "Normalized Cross-Correlation",
                      "Normalized Mutual Information");
  m_metric.select("Log Absolute Differences");

  m_transform.addOptions("YTranslation", "Translation", "Rigid", "Similarity", "Affine");
  m_transform.select("Rigid");

  m_optimizer.addOptions("LBFGS", "BFGS", "Nonlinear Conjugate Gradient", "Steepest Descent");
  m_optimizer.select("LBFGS");

  createIOGroupBox();
  createParaGroupBox();

  auto mainLayout = new QVBoxLayout;
  mainLayout->addWidget(m_ioGroupBox);
  mainLayout->addWidget(m_paraGroupBox);
  mainLayout->addWidget(createButtonBox("Register"));
  setLayout(mainLayout);

  setWindowTitle(tr("Sections Registration"));
}

void ZSectionsRegistrationDialog::createIOGroupBox()
{
  m_ioGroupBox = new QGroupBox(tr("Inputs and Outputs"), this);
  // everything
  auto alllayout = new QVBoxLayout;

  m_inputImagesFileWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::OpenMultipleFilesWithFilter,
                                                  "Input Sections:",
                                                  tr("Images (*.nim *.tif *.tiff *.v3draw *.lsm *.jpg *.png)"),
                                                  ZSystemInfo::instance().lastOpenedObjPathQSettingLocation("Image"));
  m_inputImagesFileWidget->setCompareFunc(naturalSortLessThan);
  alllayout->addWidget(m_inputImagesFileWidget);
  connect(m_inputImagesFileWidget, &ZSelectFileWidget::changed, this, &ZSectionsRegistrationDialog::inputImagesChanged);

  //  hlayout = new QHBoxLayout;
  //  hlayout->addWidget(m_openLoadedStack.createNameLabel());
  //  hlayout->addWidget(m_openLoadedStack.createWidget());
  //  alllayout->addLayout(hlayout);

  adjustInputImageWidget();

  m_outputStackWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile,
                                              "Output Aligned Image:",
                                              tr("Stack (*.nim)"),
                                              ZSystemInfo::instance().lastOpenedObjPathQSettingLocation("Image"));
  alllayout->addWidget(m_outputStackWidget);

  m_outputLogFileWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile,
                                                "Output Log File:",
                                                tr("Log (*.txt)"),
                                                ZSystemInfo::instance().lastOpenedObjPathQSettingLocation("Image"));
  alllayout->addWidget(m_outputLogFileWidget);

  //  hlayout = new QHBoxLayout;
  //  hlayout->addWidget(m_openStackAfterRegistering.createNameLabel());
  //  hlayout->addWidget(m_openStackAfterRegistering.createWidget());
  //  alllayout->addLayout(hlayout);

  m_ioGroupBox->setLayout(alllayout);
}

void ZSectionsRegistrationDialog::createParaGroupBox()
{
  m_paraGroupBox = new QGroupBox(tr("Parameters"), this);
  // everything
  auto alllayout = new QVBoxLayout;

  auto hlayout = new QHBoxLayout;
  hlayout->addWidget(m_referenceChannel.createNameLabel());
  hlayout->addWidget(m_referenceChannel.createWidget());
  // hlayout->addStretch(1);
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  m_referenceImageIndex.setStyle("SPINBOX");
  hlayout->addWidget(m_referenceImageIndex.createNameLabel());
  hlayout->addWidget(m_referenceImageIndex.createWidget());
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_removeBackground.createNameLabel());
  hlayout->addWidget(m_removeBackground.createWidget());
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_removeHighForeground.createNameLabel());
  hlayout->addWidget(m_removeHighForeground.createWidget());
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  m_numScales.setStyle("SPINBOX");
  hlayout->addWidget(m_numScales.createNameLabel());
  hlayout->addWidget(m_numScales.createWidget());
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  m_numNeighbors.setStyle("SPINBOX");
  hlayout->addWidget(m_numNeighbors.createNameLabel());
  hlayout->addWidget(m_numNeighbors.createWidget());
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_allowFlip.createNameLabel());
  hlayout->addWidget(m_allowFlip.createWidget());
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_brightBackground.createNameLabel());
  hlayout->addWidget(m_brightBackground.createWidget());
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_metric.createNameLabel());
  hlayout->addWidget(m_metric.createWidget());
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_transform.createNameLabel());
  hlayout->addWidget(m_transform.createWidget());
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_optimizer.createNameLabel());
  hlayout->addWidget(m_optimizer.createWidget());
  alllayout->addLayout(hlayout);

  m_paraGroupBox->setLayout(alllayout);
}

} // namespace nim
