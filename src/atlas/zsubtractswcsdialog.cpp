#include "zsubtractswcsdialog.h"

#include "zdoc.h"
#include "zexception.h"
#include "zmessageboxhelpers.h"
#include "zselectfilewidget.h"
#include "zswcdoc.h"
#include "zswcsubtract.h"
#include "zsysteminfo.h"

#include <QApplication>
#include <QCheckBox>
#include <QFileInfo>
#include <QGroupBox>
#include <QVBoxLayout>

namespace nim {

ZSubtractSwcsDialog::ZSubtractSwcsDialog(ZDoc& doc, QWidget* parent)
  : ZImgProcessDialog(doc, parent)
  , m_doc(doc)
{
  setWindowTitle(tr("Subtract SWCs"));

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(10, 10, 10, 10);
  layout->setSpacing(10);

  auto* ioGroup = new QGroupBox(tr("Inputs and Output"), this);
  auto* ioLayout = new QVBoxLayout(ioGroup);

  const QString swcStartDir = ZSystemInfo::instance().lastOpenedObjPathQSettingLocation("Swc");

  m_subtractSwcsWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::OpenMultipleFiles,
                                               tr("Subtract SWCs:"),
                                               ZSwc::getQtReadNameFilter(),
                                               swcStartDir,
                                               QString(),
                                               QBoxLayout::LeftToRight,
                                               ioGroup);
  ioLayout->addWidget(m_subtractSwcsWidget);

  m_inputSwcWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::OpenSingleFile,
                                           tr("From SWC:"),
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

  layout->addWidget(createButtonBox(tr("Subtract"), tr("Cancel")));
}

QString ZSubtractSwcsDialog::inputSwcPath() const
{
  CHECK(m_inputSwcWidget != nullptr);
  return m_inputSwcWidget->getSelectedOpenFile();
}

QString ZSubtractSwcsDialog::outputSwcPath() const
{
  CHECK(m_outputSwcWidget != nullptr);
  return m_outputSwcWidget->getSelectedSaveFile();
}

QStringList ZSubtractSwcsDialog::subtractSwcPaths() const
{
  CHECK(m_subtractSwcsWidget != nullptr);
  return m_subtractSwcsWidget->getSelectedMultipleOpenFiles();
}

bool ZSubtractSwcsDialog::loadResultEnabled() const
{
  CHECK(m_loadResultCheck != nullptr);
  return m_loadResultCheck->isChecked();
}

ZImgProcessDialog::WorkerSpec ZSubtractSwcsDialog::createWorkerSpec()
{
  const QString output = outputSwcPath();
  if (output.trimmed().isEmpty()) {
    throw ZException("Please select an output SWC file.");
  }

  const QString input = inputSwcPath();
  if (input.trimmed().isEmpty()) {
    throw ZException("Please select an input SWC file.");
  }

  const QStringList subtractList = subtractSwcPaths();
  if (subtractList.empty()) {
    throw ZException("Please select at least one SWC to subtract.");
  }

  const bool loadResult = loadResultEnabled();

  WorkerSpec spec;
  spec.workerName = tr("Subtract SWCs");
  spec.taskTitle = tr("Subtract SWCs: %1 -> %2").arg(QFileInfo(input).fileName(), QFileInfo(output).fileName());
  spec.successMessage = tr("Subtract SWCs finished.");
  spec.makeWorker = [input, subtractList, output]() -> std::unique_ptr<ZImgProcess> {
    auto worker = std::make_unique<ZSwcSubtract>();
    worker->setInputSwcFilename(input);
    worker->setSubtractSwcFilenames(subtractList);
    worker->setOutputSwcFilename(output);
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
