#include "zpunctadetectiondialog.h"

#include "zpunctadetection.h"
#include "zimg.h"
#include "zselectfilewidget.h"
#include "zswc.h"
#include "zsysteminfo.h"
#include <QVBoxLayout>
#include <QFileInfo>
#include <QKeyEvent>
#include <QFileDialog>
#include <QMessageBox>
#include <QApplication>
#include <QPushButton>
#include <QThread>

namespace nim {

ZPunctaDetectionDialog::ZPunctaDetectionDialog(QWidget* parent)
  : ZImgProcessDialog(parent)
  , m_useCurrentActiveImage("Use Current Active Image", false)
  , m_voxelSize("Voxel Size (um)", glm::dvec3(.0, .0, .0), glm::dvec3(.0, .0, .0),
                glm::dvec3(1e6, 1e6, 1e6))
  , m_punctaChannel("Puncta Channel", nullptr, "Ch")
  , m_punctaThreshold("Puncta Threshold (-1 means auto detect)", -1, -1, 255)
  , m_somaPunctaThreshold("Soma Puncta Threshold (-1 means auto detect)", -1, -1, 255)
  , m_dendriteChannel("Tube Channel")
  , m_tubeThreshold("Tube Threshold", 100, 1, 255)
  , m_ambiguousFactor("Ambiguous Factor", 1.0, 1., 2.)
  , m_maxDistToBranchInUm("Max Distance (um) between Puncta and SWC for Puncta Assignment", 2.5, 0., 100.)
{
  init();
}

void ZPunctaDetectionDialog::createWorker(ZImgProcess*& worker, QString& workerName)
{
  focusNextChild();
  ZImgInfo imgInfo;

  if (!m_useCurrentActiveImage.get() && QFile::exists(m_inputImageFileWidget->getSelectedOpenFile())) {
    imgInfo = ZImg::readImgInfos(m_inputImageFileWidget->getSelectedOpenFile()).at(0);
  } else {
    throw ZImgException(QString("No Image to detect."));
  }
  if (imgInfo.numTimes != 1) {
    throw ZImgException(QString("Can not detect puncta from time sequence image."));
  }
  if (m_outputPunctaFileWidget->getSelectedSaveFile().isEmpty()) {
    throw ZImgException(QString("Result puncta file must be specified."));
  }
  if (m_outputLogFileWidget->getSelectedSaveFile().isEmpty()) {
    throw ZImgException(QString("Detection log file must be specified."));
  }
  int punctaChannel = m_punctaChannel.get() - 1;
  int dendriteChannel = m_dendriteChannel.associatedData() - 1;
  if (punctaChannel == dendriteChannel) {
    throw ZImgException(QString("Puncta and dendrite channels are not correct."));
  }
  if (dendriteChannel >= 0) {
    if (m_voxelSize.get().x == 0.0 || m_voxelSize.get().y == 0.0 || m_voxelSize.get().z == 0.0) {
      throw ZImgException(QString("Image Resolution is not correct."));
    }
    imgInfo.voxelSizeUnit = VoxelSizeUnit::um;
    imgInfo.voxelSizeX = m_voxelSize.get().x;
    imgInfo.voxelSizeY = m_voxelSize.get().y;
    imgInfo.voxelSizeZ = m_voxelSize.get().z;
  }

  auto workertmp = new ZPunctaDetection(m_inputImageFileWidget->getSelectedOpenFile(), imgInfo, punctaChannel);
  workertmp->setAmbiguousFactor(m_ambiguousFactor.get());
  if (dendriteChannel >= 0)
    workertmp->setDendriteChannel(dendriteChannel);
  workertmp->setSwcFiles(m_inputSwcFilesWidget->getSelectedMultipleOpenFiles());
  workertmp->setLogFile(m_outputLogFileWidget->getSelectedSaveFile());
  workertmp->setResultPunctaFilename(m_outputPunctaFileWidget->getSelectedSaveFile());
  if (dendriteChannel >= 0)
    workertmp->setResultSomaPunctaFilename(m_outputSomaPunctaFileWidget->getSelectedSaveFile());
  if (m_punctaThreshold.get() != -1)
    workertmp->setPunctaThreshold(m_punctaThreshold.get());
  if (m_somaPunctaThreshold.get() != -1)
    workertmp->setSomaPunctaThreshold(m_somaPunctaThreshold.get());
  if (dendriteChannel >= 0)
    workertmp->setDendriteThreshold(m_tubeThreshold.get());
  workertmp->setMaxDistToBranchInUm(m_maxDistToBranchInUm.get());

  worker = workertmp;
  workerName = "Puncta Detection";
}

void ZPunctaDetectionDialog::adjustInputImageWidget()
{
  m_inputImageFileWidget->setVisible(!m_useCurrentActiveImage.get());
  if (m_useCurrentActiveImage.get()) {
  } else {
    inputImageChanged();
  }
}

void ZPunctaDetectionDialog::inputImageChanged()
{
  QString fn = m_inputImageFileWidget->getSelectedOpenFile();
  if (!fn.isEmpty()) {
    try {
      ZImgInfo info = ZImg::readImgInfos(fn).at(0);
      if (info.voxelSizeUnit != VoxelSizeUnit::none) {
        updateInterface(fn, info.numChannels, info.voxelSizeXInUm(),
                        info.voxelSizeYInUm(), info.voxelSizeZInUm());
      } else {
        updateInterface(fn, info.numChannels, 0, 0, 0);
      }
    }
    catch (const ZIOException& e) {
      updateInterface(fn, 0, 0, 0, 0);
      QMessageBox::critical(this, QApplication::applicationName(), QString("Can not read input image:\n%1").arg(e.what()));
    }
  } else {
    updateInterface(fn, 0, 0, 0, 0);
  }
}

void ZPunctaDetectionDialog::detectLSMResolution()
{
  QString fileName = QFileDialog::getOpenFileName(this, "choose LSM file", "",
                                                  tr("LSM file (*.lsm)"));
  if (!fileName.isEmpty()) {
    try {
      ZImgInfo info = ZImg::readImgInfos(fileName).at(0);
      if (info.voxelSizeUnit != VoxelSizeUnit::none) {
        m_voxelSize.set(glm::dvec3(info.voxelSizeXInUm(),
                                   info.voxelSizeYInUm(),
                                   info.voxelSizeZInUm()));
      } else {
        QMessageBox::critical(this, QApplication::applicationName(),
                              "File does not contain resolution information");
      }
    }
    catch (const ZException& e) {
      QMessageBox::critical(this, QApplication::applicationName(),
                            QString("Can not detect resolution from lsm file:\n%1").arg(e.what()));
    }
  }
}

void ZPunctaDetectionDialog::dendriteChannelChanged()
{
  m_tubeThreshold.setVisible(!m_dendriteChannel.isSelected("None"));
}

void ZPunctaDetectionDialog::updateInterface(const QString& fn, size_t numChannel, double vsx, double vsy, double vsz)
{
  if (QFile::exists(fn)) {
    QFileInfo fi(fn);
    QString logFn = fi.path() + "/" + fi.baseName() + "_puncta_detection_log.txt";
    m_outputLogFileWidget->setFile(logFn);
    QString punctaFn = fi.path() + "/" + fi.baseName() + "_detected_puncta.nimp";
    m_outputPunctaFileWidget->setFile(punctaFn);
    QString spunctaFn = fi.path() + "/" + fi.baseName() + "_detected_soma_puncta.nimp";
    m_outputSomaPunctaFileWidget->setFile(spunctaFn);
  } else {
    m_outputLogFileWidget->setFile("");
    m_outputPunctaFileWidget->setFile("");
    m_outputSomaPunctaFileWidget->setFile("");
  }

  m_voxelSize.set(glm::dvec3(vsx, vsy, vsz));

  m_punctaChannel.clearOptions();
  m_dendriteChannel.clearOptions();
  m_dendriteChannel.addOptionWithData(std::make_pair<QString, int>("None", 0));
  for (int i = 0; i < static_cast<int>(numChannel); ++i) {
    m_punctaChannel.addOption(i + 1);
    m_dendriteChannel.addOptionWithData(std::make_pair(QString("Ch%1").arg(i + 1), i + 1));
  }

  m_dendriteChannel.select("None");
  if (numChannel > 2) {
    m_punctaChannel.select(2);
    m_dendriteChannel.select("Ch3");
  } else if (numChannel > 0) {
    m_punctaChannel.select(1);
  }
}

void ZPunctaDetectionDialog::init()
{
  createIOGroupBox();
  createParaGroupBox();
  adjustInputImageWidget();

  m_tubeThreshold.setVisible(false);
  connect(&m_dendriteChannel, &ZStringIntOptionParameter::valueChanged, this,
          &ZPunctaDetectionDialog::dendriteChannelChanged);

  auto mainLayout = new QVBoxLayout;
  mainLayout->addWidget(m_ioGroupBox);
  mainLayout->addWidget(m_paraGroupBox);
  mainLayout->addWidget(createButtonBox("Detect"));
  setLayout(mainLayout);

  setWindowTitle(tr("Detect Puncta"));
}

void ZPunctaDetectionDialog::createIOGroupBox()
{
  m_ioGroupBox = new QGroupBox(tr("Inputs and Outputs"), this);
  // everything
  auto alllayout = new QVBoxLayout;

  QStringList filters;
  std::vector<FileFormat> formats;
  ZImg::getQtReadNameFilter(filters, formats);
  m_inputImageFileWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::OpenSingleFile, "Input Image:",
                                                 filters[0],
                                                 ZSystemInfo::instance().lastOpenedObjPathQSettingLocation("Image"));
  alllayout->addWidget(m_inputImageFileWidget);
  connect(m_inputImageFileWidget, &ZSelectFileWidget::changed, this, &ZPunctaDetectionDialog::inputImageChanged);

  m_inputSwcFilesWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::OpenMultipleFiles, "Input Swcs:",
                                                tr("Swcs (*.swc)"),
                                                ZSystemInfo::instance().lastOpenedObjPathQSettingLocation("Swc"));
  alllayout->addWidget(m_inputSwcFilesWidget);

  m_outputPunctaFileWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile, "Output All Puncta File:",
                                                   tr("Nimp (*.nimp)"),
                                                   ZSystemInfo::instance().lastOpenedObjPathQSettingLocation("Puncta"));
  alllayout->addWidget(m_outputPunctaFileWidget);

  m_outputSomaPunctaFileWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile,
                                                       "Output All Soma Puncta File:",
                                                       tr("Nimp (*.nimp)"),
                                                       ZSystemInfo::instance().lastOpenedObjPathQSettingLocation(
                                                         "Puncta"));
  alllayout->addWidget(m_outputSomaPunctaFileWidget);

  m_outputLogFileWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile, "Output Log File:",
                                                tr("Log (*.txt)"),
                                                ZSystemInfo::instance().lastOpenedObjPathQSettingLocation("Image"));
  alllayout->addWidget(m_outputLogFileWidget);

  m_ioGroupBox->setLayout(alllayout);
}

void ZPunctaDetectionDialog::createParaGroupBox()
{
  m_paraGroupBox = new QGroupBox(tr("Parameters"), this);
  // everything
  auto alllayout = new QVBoxLayout;

  m_voxelSize.setSingleStep(1e-6);
  m_voxelSize.setDecimal(6);
  m_voxelSize.setWidgetOrientation(Qt::Horizontal);
  m_voxelSize.setStyle("SPINBOX");
  std::vector<QString> name{"x", "y", "z"};
  m_voxelSize.setNameForEachValue(name);

  auto hlayout = new QHBoxLayout;
  hlayout->addWidget(m_voxelSize.createNameLabel());
  hlayout->addWidget(m_voxelSize.createWidget(), 0, Qt::AlignLeft);
  m_detectResolutionButton = new QPushButton(tr("Detect From File"), this);
  connect(m_detectResolutionButton, &QPushButton::clicked, this, &ZPunctaDetectionDialog::detectLSMResolution);
  hlayout->addWidget(m_detectResolutionButton);
  alllayout->addLayout(hlayout);

#if 0
  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_punctaChannel.createNameLabel());
  hlayout->addWidget(m_punctaChannel.createWidget());
  hlayout->addStretch(1);
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_punctaThreshold.createNameLabel());
  hlayout->addWidget(m_punctaThreshold.createWidget());
  //hlayout->addStretch(1);
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_dendriteChannel.createNameLabel());
  hlayout->addWidget(m_dendriteChannel.createWidget());
  hlayout->addStretch(1);
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_tubeThreshold.createNameLabel());
  hlayout->addWidget(m_tubeThreshold.createWidget());
  //hlayout->addStretch(1);
  alllayout->addLayout(hlayout);

  //hlayout = new QHBoxLayout;
  //hlayout->addWidget(m_ambiguousFactor.createNameLabel());
  //hlayout->addWidget(m_ambiguousFactor.createWidget());
  //alllayout->addLayout(hlayout);
#else
  auto glayout = new QGridLayout;
  glayout->addWidget(m_punctaChannel.createNameLabel(), 0, 0);
  glayout->addWidget(m_punctaChannel.createWidget(), 0, 1);

  glayout->addWidget(m_punctaThreshold.createNameLabel(), 1, 0);
  glayout->addWidget(m_punctaThreshold.createWidget(), 1, 1);

  glayout->addWidget(m_somaPunctaThreshold.createNameLabel(), 2, 0);
  glayout->addWidget(m_somaPunctaThreshold.createWidget(), 2, 1);

  glayout->addWidget(m_dendriteChannel.createNameLabel(), 3, 0);
  glayout->addWidget(m_dendriteChannel.createWidget(), 3, 1);

  glayout->addWidget(m_tubeThreshold.createNameLabel(), 4, 0);
  glayout->addWidget(m_tubeThreshold.createWidget(), 4, 1);

  glayout->addWidget(m_maxDistToBranchInUm.createNameLabel(), 4, 0);
  glayout->addWidget(m_maxDistToBranchInUm.createWidget(), 4, 1);

  //glayout->addWidget(m_ambiguousFactor.createNameLabel(), 5, 0);
  //glayout->addWidget(m_ambiguousFactor.createWidget(), 5, 1);
  alllayout->addLayout(glayout);
#endif

  m_paraGroupBox->setLayout(alllayout);
}

} // namespace nim
