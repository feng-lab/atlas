#include "zpunctadetectiondialog.h"

#include <QVBoxLayout>
#include <QGridLayout>
#include <QFileInfo>
#include <QKeyEvent>
#include <QFileDialog>
#include <QMessageBox>
#include "zselectfilewidget.h"
#include <QApplication>

#ifdef _NEUTUBE_
#include "zstack.hxx"
#include "zstackdoc.h"
#endif

#include "zpunctadetection.h"
#include <QThread>
#include "zimg.h"
#include "zimgstackinterface.h"
#include "zswc.h"

namespace nim {

#ifdef _NEUTUBE_
ZPunctaDetectionDialog::ZPunctaDetectionDialog(std::tr1::shared_ptr<ZStackDoc> doc, QWidget *parent)
  : QDialog(parent)
  , m_doc(doc)
  , m_useCurrentActiveImage("Use Current Active Image", true)
  , m_voxelSize("Voxel Size (um)", glm::dvec3(.0,.0,.0), glm::dvec3(.0,.0,.0),
                glm::dvec3(1e6,1e6,1e6))
  , m_punctaChannel("Puncta Channel:", nullptr, "Ch")
  , m_punctaThreshold("Puncta Threshold (-1 means auto detect)", -1, -1, 255)
  , m_dendriteChannel("Tube Channel:")
  , m_tubeThreshold("Tube Threshold", 100, 1, 255)
  , m_ambiguousFactor("Ambiguous Factor", 1.0, 1., 2.)
{
  init();
}
#endif

ZPunctaDetectionDialog::ZPunctaDetectionDialog(QWidget* parent)
  : QDialog(parent)
  , m_useCurrentActiveImage("Use Current Active Image", false)
  , m_voxelSize("Voxel Size (um)", glm::dvec3(.0, .0, .0), glm::dvec3(.0, .0, .0),
                glm::dvec3(1e6, 1e6, 1e6))
  , m_punctaChannel("Puncta Channel", nullptr, "Ch")
  , m_punctaThreshold("Puncta Threshold (-1 means auto detect)", -1, -1, 255)
  , m_dendriteChannel("Tube Channel")
  , m_tubeThreshold("Tube Threshold", 100, 1, 255)
  , m_ambiguousFactor("Ambiguous Factor", 1.0, 1., 2.)
{
  init();
}

void ZPunctaDetectionDialog::detect()
{
  focusNextChild();
  ZImg img;
#ifdef _NEUTUBE_
  if (m_useCurrentActiveImage.get() && m_doc && m_doc->hasStackData()) {
    img = wrapZStackAsZImg(*m_doc->stack());
  } else if (!m_useCurrentActiveImage.get() && QFile::exists(m_inputImageFileWidget->getSelectedOpenFile())) {
#endif
  if (!m_useCurrentActiveImage.get() && QFile::exists(m_inputImageFileWidget->getSelectedOpenFile())) {
    try {
      img.load(m_inputImageFileWidget->getSelectedOpenFile());
#ifndef _NEUTUBE_
      ZImg* imgToSend = new ZImg();   // will leak memory if no one receive the signal
      imgToSend->swap(img);
      img = imgToSend->createView();
      emit srcImgReady(imgToSend, m_inputImageFileWidget->getSelectedOpenFile());
#else
      ZStack* stack = imgToZStack(img);
      stack->setSource(QFile::encodeName(m_inputImageFileWidget->getSelectedOpenFile()).constData());
      emit stackDocDelivered(stack);
      img = wrapZStackAsZImg(*stack);
#endif
    }
    catch (const ZException& e) {
      QMessageBox::critical(this, qApp->applicationName(), "Read Image Error.\n" + e.what());
      return;
    }
    // todo: select time spot
    if (!img.is3DImg() && !img.is2DImg()) {
      QMessageBox::critical(this, qApp->applicationName(),
                            QString("Can not detect puncta from time sequence image"));
      return;
    }
  } else {
    QMessageBox::critical(this, qApp->applicationName(), "No Image to detect.");
    return;
  }
  if (!img.isType<uint8_t>()) {
    img = img.convertNormalizedTo<uint8_t>();
  }
  if (m_outputPunctaFileWidget->getSelectedSaveFile().isEmpty()) {
    QMessageBox::critical(this, qApp->applicationName(), "Result puncta file must be specified.");
    return;
  }
  if (m_outputLogFileWidget->getSelectedSaveFile().isEmpty()) {
    QMessageBox::critical(this, qApp->applicationName(), "Detection log file must be specified.");
    return;
  }
  int punctaChannel = m_punctaChannel.get() - 1;
  int dendriteChannel = m_dendriteChannel.associatedData() - 1;
  if (punctaChannel == dendriteChannel) {
    QMessageBox::critical(this, qApp->applicationName(), "Puncta and tube channels are not correct.");
    return;
  }
  if (dendriteChannel >= 0) {
    if (m_voxelSize.get().x == 0.0 || m_voxelSize.get().y == 0.0 || m_voxelSize.get().z == 0.0) {
      QMessageBox::critical(this, qApp->applicationName(), "Image Resolution is not correct.");
      return;
    } else {
      img.infoRef().voxelSizeUnit = VoxelSizeUnit::um;
      img.infoRef().voxelSizeX = m_voxelSize.get().x;
      img.infoRef().voxelSizeY = m_voxelSize.get().y;
      img.infoRef().voxelSizeZ = m_voxelSize.get().z;
    }
  }

  QStringList swcFiles = m_inputSwcFilesWidget->getSelectedMultipleOpenFiles();
  std::vector<ZSwc> swcTrees(swcFiles.size());
  try {
    for (int i = 0; i < swcFiles.size(); ++i) {
      swcTrees[i] = ZSwc(swcFiles.at(i));
    }
  }
  catch (const ZException& e) {
    QMessageBox::critical(this, qApp->applicationName(), "Read Swc Error.\n" + e.what());
    return;
  }

  m_isCanceled = false;
  m_hasError = false;

  ZPunctaDetection* worker = new ZPunctaDetection(img, punctaChannel);
  worker->setAmbiguousFactor(m_ambiguousFactor.get());
  if (dendriteChannel >= 0)
    worker->setDendriteChannel(dendriteChannel);
  worker->setSwcTrees(swcTrees, swcFiles);
  worker->setCancelFlag(&m_isCanceled);
  worker->setLogFile(m_outputLogFileWidget->getSelectedSaveFile());
  worker->setResultPunctaFilename(m_outputPunctaFileWidget->getSelectedSaveFile());
  if (dendriteChannel >= 0)
    worker->setResultSomaPunctaFilename(m_outputSomaPunctaFileWidget->getSelectedSaveFile());
  if (m_punctaThreshold.get() != -1)
    worker->setPunctaThreshold(m_punctaThreshold.get());
  if (dendriteChannel >= 0)
    worker->setDendriteThreshold(m_tubeThreshold.get());

  m_progressDialog = new QProgressDialog(this);
  m_progressDialog->setLabelText("Detecting Puncta...");
  m_progressDialog->setAutoReset(false);
  m_progressDialog->setAttribute(Qt::WA_DeleteOnClose);
  QObject::disconnect(m_progressDialog, &QProgressDialog::canceled, m_progressDialog, &QProgressDialog::cancel);
  connect(worker, qOverload<int>(&ZPunctaDetection::progressChanged), m_progressDialog, &QProgressDialog::setValue);
  connect(worker, &ZPunctaDetection::canceled, this, &ZPunctaDetectionDialog::processCanceled);
  connect(worker, &ZPunctaDetection::processError, this, &ZPunctaDetectionDialog::processError);
  connect(m_progressDialog, &QProgressDialog::canceled, this, &ZPunctaDetectionDialog::cancelButtonPressed);

  QThread* thread = new QThread(this);
  connect(thread, &QThread::started, worker, &ZPunctaDetection::run);
  connect(worker, &ZPunctaDetection::canceled, thread, &QThread::quit);
  connect(worker, &ZPunctaDetection::finished, thread, &QThread::quit);
  connect(worker, &ZPunctaDetection::processError, thread, &QThread::quit);
  connect(thread, &QThread::finished, worker, &ZPunctaDetection::deleteLater);
  connect(thread, &QThread::finished, m_progressDialog, &QProgressDialog::reset);
  connect(thread, &QThread::finished, this, &ZPunctaDetectionDialog::processFinished);
  connect(thread, &QThread::finished, thread, &QThread::deleteLater);
  worker->moveToThread(thread);

  thread->start();
  m_progressDialog->exec();
}

void ZPunctaDetectionDialog::processCanceled()
{
  QMessageBox::critical(this, qApp->applicationName(),
                        "Puncta Detection is canceled by user.");
}

void ZPunctaDetectionDialog::processFinished()
{
  if (!m_isCanceled && !m_hasError) {
    QMessageBox::information(this, qApp->applicationName(),
                             "Puncta Detection Finished.");
  }
}

void ZPunctaDetectionDialog::processError(const QString& e)
{
  m_hasError = true;
  QMessageBox::critical(this, qApp->applicationName(),
                        QString("Error During Detection: %1").arg(e));
}

void ZPunctaDetectionDialog::cancelButtonPressed()
{
  m_progressDialog->setLabelText("Canceling...");
  m_isCanceled = true;
}

//void ZPunctaDetectionDialog::reject()
//{
//  m_thread->wait();
//  QDialog::reject();
//}

void ZPunctaDetectionDialog::keyPressEvent(QKeyEvent* e)
{
  switch (e->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
      break;
    default:
      QDialog::keyPressEvent(e);
  }
}

//void ZPunctaDetectionDialog::closeEvent(QCloseEvent *e)
//{
//  m_thread->wait();
//  QDialog::closeEvent(e);
//}

void ZPunctaDetectionDialog::adjustInputImageWidget()
{
  m_inputImageFileWidget->setVisible(!m_useCurrentActiveImage.get());
  if (m_useCurrentActiveImage.get()) {
#ifdef _NEUTUBE_
    ZResolution res = m_doc->stackResolution();
    if (res.unit() == 'u') {
      updateInterface(QString::fromStdString(m_doc->stackSourcePath()), m_doc->stackChannelNumber(),
                      res.voxelSizeX(), res.voxelSizeY(), res.voxelSizeZ());
    } else {
      updateInterface(QString::fromStdString(m_doc->stackSourcePath()), m_doc->stackChannelNumber(),
                      0, 0, 0);
    }
#endif
  } else {
    inputImageChanged();
  }
}

void ZPunctaDetectionDialog::inputImageChanged()
{
  QString fn = m_inputImageFileWidget->getSelectedOpenFile();
  if (!fn.isEmpty()) {
    try {
      ZImgInfo info = ZImg::readImgInfo(fn).at(0);
      if (info.voxelSizeUnit != VoxelSizeUnit::none) {
        updateInterface(fn, info.numChannels, info.voxelSizeXInUm(),
                        info.voxelSizeYInUm(), info.voxelSizeZInUm());
      } else {
        updateInterface(fn, info.numChannels, 0, 0, 0);
      }
    }
    catch (const ZIOException& e) {
      updateInterface(fn, 0, 0, 0, 0);
      QMessageBox::critical(this, qApp->applicationName(), "Can not read input image.\n" + e.what());
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
      ZImgInfo info = ZImg::readImgInfo(fileName).at(0);
      if (info.voxelSizeUnit != VoxelSizeUnit::none) {
        m_voxelSize.set(glm::dvec3(info.voxelSizeXInUm(),
                                   info.voxelSizeYInUm(),
                                   info.voxelSizeZInUm()));
      } else {
        QMessageBox::critical(this, qApp->applicationName(),
                              "File does not contain resolution information");
      }
    }
    catch (const ZException& e) {
      QMessageBox::critical(this, qApp->applicationName(),
                            "Can not detect resolution from lsm file.\n" + e.what());
    }
  }
}

void ZPunctaDetectionDialog::dendriteChannelChanged()
{
  if (m_dendriteChannel.isSelected("None"))
    m_tubeThreshold.setVisible(false);
  else
    m_tubeThreshold.setVisible(true);
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
  m_dendriteChannel.addOptionWithData(qMakePair<QString, int>("None", 0));
  for (int i = 0; i < static_cast<int>(numChannel); ++i) {
    m_punctaChannel.addOption(i + 1);
    m_dendriteChannel.addOptionWithData(qMakePair(QString("Ch%1").arg(i + 1), i + 1));
  }

  m_dendriteChannel.select("None");
  if (numChannel > 0)
    m_punctaChannel.select(1);
}

void ZPunctaDetectionDialog::init()
{
  createIOGroupBox();
  createParaGroupBox();
  adjustInputImageWidget();

  m_runButton = new QPushButton(tr("Detect"), this);
  m_exitButton = new QPushButton(tr("Exit"), this);
  m_buttonBox = new QDialogButtonBox(Qt::Horizontal, this);
  m_buttonBox->addButton(m_exitButton, QDialogButtonBox::RejectRole);
  m_buttonBox->addButton(m_runButton, QDialogButtonBox::ActionRole);
  connect(m_exitButton, &QPushButton::clicked, this, &ZPunctaDetectionDialog::reject);
  connect(m_runButton, &QPushButton::clicked, this, &ZPunctaDetectionDialog::detect);

  m_tubeThreshold.setVisible(false);
  connect(&m_dendriteChannel, &ZStringIntOptionParameter::valueChanged, this,
          &ZPunctaDetectionDialog::dendriteChannelChanged);

  QVBoxLayout* mainLayout = new QVBoxLayout;
  mainLayout->addWidget(m_ioGroupBox);
  mainLayout->addWidget(m_paraGroupBox);
  mainLayout->addWidget(m_buttonBox);
  setLayout(mainLayout);

  setWindowTitle(tr("Detect Puncta"));
}

void ZPunctaDetectionDialog::createIOGroupBox()
{
  m_ioGroupBox = new QGroupBox(tr("Inputs and Outputs"), this);
  // everything
  QVBoxLayout* alllayout = new QVBoxLayout;

#ifdef _NEUTUBE_
  QHBoxLayout *hlayout;
  if (m_doc) {
    hlayout = new QHBoxLayout;
    hlayout->addWidget(m_useCurrentActiveImage.createNameLabel(), 0, Qt::AlignLeft);
    hlayout->addWidget(m_useCurrentActiveImage.createWidget(), 0, Qt::AlignLeft);
    hlayout->addStretch(1);
    alllayout->addLayout(hlayout);
    connect(&m_useCurrentActiveImage, &ZBoolParameter::valueChanged, this, &ZPunctaDetectionDialog::adjustInputImageWidget);
  }
#endif
  QStringList filters;
  QList<FileFormat> formats;
  ZImg::getQtReadNameFilter(filters, formats);
  m_inputImageFileWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::OpenSingleFile, "Input Image:",
                                                 filters[0]);
  alllayout->addWidget(m_inputImageFileWidget);
  connect(m_inputImageFileWidget, &ZSelectFileWidget::changed, this, &ZPunctaDetectionDialog::inputImageChanged);

  m_inputSwcFilesWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::OpenMultipleFiles, "Input Swcs:",
                                                tr("Swcs (*.swc)"));
  alllayout->addWidget(m_inputSwcFilesWidget);

  m_outputPunctaFileWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile, "Output All Puncta File:",
                                                   tr("Nimp (*.nimp)"));
  alllayout->addWidget(m_outputPunctaFileWidget);

  m_outputSomaPunctaFileWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile,
                                                       "Output All Soma Puncta File:",
                                                       tr("Nimp (*.nimp)"));
  alllayout->addWidget(m_outputSomaPunctaFileWidget);

  m_outputLogFileWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile, "Output Log File:",
                                                tr("Log (*.txt)"));
  alllayout->addWidget(m_outputLogFileWidget);

  m_ioGroupBox->setLayout(alllayout);
}

void ZPunctaDetectionDialog::createParaGroupBox()
{
  m_paraGroupBox = new QGroupBox(tr("Parameters"), this);
  // everything
  QVBoxLayout* alllayout = new QVBoxLayout;

  m_voxelSize.setSingleStep(1e-6);
  m_voxelSize.setDecimal(6);
  m_voxelSize.setWidgetOrientation(Qt::Horizontal);
  m_voxelSize.setStyle("SPINBOX");
  QStringList name;
  name.push_back("x");
  name.push_back("y");
  name.push_back("z");
  m_voxelSize.setNameForEachValue(name);

  QHBoxLayout* hlayout = new QHBoxLayout;
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
  QGridLayout* glayout = new QGridLayout;
  glayout->addWidget(m_punctaChannel.createNameLabel(), 0, 0);
  glayout->addWidget(m_punctaChannel.createWidget(), 0, 1);

  glayout->addWidget(m_punctaThreshold.createNameLabel(), 1, 0);
  glayout->addWidget(m_punctaThreshold.createWidget(), 1, 1);

  glayout->addWidget(m_dendriteChannel.createNameLabel(), 2, 0);
  glayout->addWidget(m_dendriteChannel.createWidget(), 2, 1);

  glayout->addWidget(m_tubeThreshold.createNameLabel(), 3, 0);
  glayout->addWidget(m_tubeThreshold.createWidget(), 3, 1);

  //glayout->addWidget(m_ambiguousFactor.createNameLabel(), 4, 0);
  //glayout->addWidget(m_ambiguousFactor.createWidget(), 4, 1);
  alllayout->addLayout(glayout);
#endif

  m_paraGroupBox->setLayout(alllayout);
}

} // namespace nim
