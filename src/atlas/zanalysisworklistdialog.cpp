#include "zanalysisworklistdialog.h"

#include "zlog.h"
#include "zanalysisworklistmodel.h"
#include "zstyleditemdelegate.h"
#include "zfileutils.h"
#include <QtWidgets>
#include <QTableView>

namespace nim {

ZAnalysisWorklistDialog::ZAnalysisWorklistDialog(QWidget* parent)
  : QDialog(parent)
{
  setFocusPolicy(Qt::StrongFocus);
  setAcceptDrops(true);
  createWidget();
}

void ZAnalysisWorklistDialog::reject()
{
  if (!m_saveButton->isEnabled() || m_model->worklist().empty()) {
    QDialog::reject();
    return;
  }

  int ans = QMessageBox::question(this, tr("Confirm"), tr("There are unsaved worklist changes. Exit anyway?"),
                                  QMessageBox::Cancel | QMessageBox::Ok, QMessageBox::Ok);
  if (ans == QMessageBox::Ok) {
    QDialog::reject();
  }
}

void ZAnalysisWorklistDialog::onNew()
{
  m_model->reset();
  m_saveButton->setEnabled(true);
  setWindowTitle("~UntitledWorklist.csv");
  m_filename.clear();
}

void ZAnalysisWorklistDialog::onOpen()
{
  QString fileName =
    QFileDialog::getOpenFileName(this, tr("Choose worklist file"),
                                 m_filename,
                                 tr("Worklist file (*.csv)"),
                                 nullptr);

  if (!fileName.isEmpty()) {
    QString res = m_model->setSource(fileName);
    if (!res.isEmpty()) {
      QMessageBox::critical(this, QApplication::applicationName(), res);
    }
    m_filename = fileName;
    m_saveButton->setEnabled(false);
    setWindowTitle(m_filename);
  }
}

void ZAnalysisWorklistDialog::onSave()
{
  if (m_filename.isEmpty() && !m_model->worklist().empty()) {
    m_filename =
      ZFileUtils::getSaveFileName(this, tr("Save worklist file as..."),
                                  m_filename,
                                  tr("Worklist file (*.csv)"),
                                  nullptr);
  }
  if (!m_filename.isEmpty()) {
    QString res = m_model->toCSV(m_filename, true);
    if (!res.isEmpty()) {
      QMessageBox::critical(this, QApplication::applicationName(), res);
    }
    m_saveButton->setEnabled(false);
    setWindowTitle(m_filename);
  }
}

void ZAnalysisWorklistDialog::onSaveAs()
{
  QString fileName =
    ZFileUtils::getSaveFileName(this, tr("Save worklist file as..."),
                                m_filename,
                                tr("Worklist file (*.csv)"),
                                nullptr);

  if (!fileName.isEmpty()) {
    m_filename = fileName;
    QString res = m_model->toCSV(m_filename, true);
    if (!res.isEmpty()) {
      QMessageBox::critical(this, QApplication::applicationName(), res);
    }
    m_saveButton->setEnabled(false);
    setWindowTitle(m_filename);
  }
}

void ZAnalysisWorklistDialog::onGenerate()
{
  onSave();

  const auto& list = m_model->worklist();
  if (!list.empty()) {
    ZGenerateAnalysisTextFile gen;
    QProgressDialog progress("Generating analysis text files...", "Cancel", 0, list.size(), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.show();
    try {
      for (size_t i = 0; i < list.size(); ++i) {
        progress.setValue(i);
        if (progress.wasCanceled())
          break;

        QApplication::processEvents();
        gen.generate(list[i]);
      }
      progress.setValue(list.size());
      QMessageBox::information(this, QApplication::applicationName(), "Analysis files generated!");
    }
    catch (const ZException& e) {
      progress.setValue(list.size());
      LOG(ERROR) << "Error while generating analysis files: " << e.what();
      QMessageBox::critical(this, QApplication::applicationName(),
                            QString("Error while generating analysis files:\n%1").arg(e.what()));
    }
  } else {
    QMessageBox::information(this, QApplication::applicationName(), "Empty list.\nNo work to do.");
  }
}

void ZAnalysisWorklistDialog::dataModified()
{
  m_saveButton->setEnabled(true);
  if (!m_filename.isEmpty() && windowTitle().compare(QString("%1 *").arg(m_filename)) != 0) {
    setWindowTitle(QString("%1 *").arg(m_filename));
  }
}

void ZAnalysisWorklistDialog::createWidget()
{
  auto vlayout = new QVBoxLayout;
  auto hlayout = new QHBoxLayout;
  auto newButton = new QPushButton(tr("new"), this);
  connect(newButton, &QPushButton::clicked, this, &ZAnalysisWorklistDialog::onNew);
  hlayout->addWidget(newButton);
  auto openButton = new QPushButton(tr("open"), this);
  connect(openButton, &QPushButton::clicked, this, &ZAnalysisWorklistDialog::onOpen);
  hlayout->addWidget(openButton);
  m_saveButton = new QPushButton(tr("save"), this);
  connect(m_saveButton, &QPushButton::clicked, this, &ZAnalysisWorklistDialog::onSave);
  hlayout->addWidget(m_saveButton);
  auto saveAsButton = new QPushButton(tr("save as..."), this);
  connect(saveAsButton, &QPushButton::clicked, this, &ZAnalysisWorklistDialog::onSaveAs);
  hlayout->addWidget(saveAsButton);

  m_view = new QTableView(this);
  m_view->setAcceptDrops(true);
  m_view->setDropIndicatorShown(true);
  m_view->setItemDelegate(new ZStyledItemDelegate(this));
  m_model = new ZAnalysisWorklistModel(this);
  m_view->setModel(m_model);

  vlayout->addLayout(hlayout);
  vlayout->addWidget(m_view);

  auto runButton = new QPushButton(tr("Generate"), this);
  auto exitButton = new QPushButton(tr("Exit"), this);
  auto buttonBox = new QDialogButtonBox(Qt::Horizontal, this);
  buttonBox->addButton(exitButton, QDialogButtonBox::RejectRole);
  buttonBox->addButton(runButton, QDialogButtonBox::ActionRole);
  connect(exitButton, &QPushButton::clicked, this, &ZAnalysisWorklistDialog::reject);
  connect(runButton, &QPushButton::clicked, this, &ZAnalysisWorklistDialog::onGenerate);
  vlayout->addWidget(buttonBox);

  setLayout(vlayout);

  setWindowTitle("~UntitledWorklist.csv");
  connect(m_model, &ZAnalysisWorklistModel::dataChanged, this, &ZAnalysisWorklistDialog::dataModified);
}

} // namespace nim
