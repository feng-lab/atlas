#include "zimgprocessdialog.h"

#include "zimgprocess.h"
#include "zlog.h"
#include <QKeyEvent>
#include <QMessageBox>
#include <QThread>
#include <QApplication>
#include <QPushButton>

namespace nim {

ZImgProcessDialog::ZImgProcessDialog(QWidget* parent)
  : QDialog(parent)
{}

void ZImgProcessDialog::processFinished()
{
  if (!m_hasError) {
    QMessageBox::information(this, QApplication::applicationName(), QString("%1 Finished.").arg(m_workerName));
  }
}

void ZImgProcessDialog::processError(const QString& e)
{
  m_hasError = true;
  QMessageBox::critical(this, QApplication::applicationName(), QString("Error during %1: %2").arg(m_workerName, e));
}

void ZImgProcessDialog::cancelButtonPressed()
{
  m_progressDialog->setLabelText(QString("%1 Canceling...").arg(m_workerName));
  if (m_cancellationSource) {
    m_cancellationSource->requestCancellation();
  }
}

void ZImgProcessDialog::keyPressEvent(QKeyEvent* e)
{
  switch (e->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
      break;
    default:
      QDialog::keyPressEvent(e);
  }
}

QDialogButtonBox* ZImgProcessDialog::createButtonBox(const QString& runButtonName, const QString& exitButtonName)
{
  auto runButton = new QPushButton(runButtonName, this);
  auto exitButton = new QPushButton(exitButtonName, this);
  auto buttonBox = new QDialogButtonBox(Qt::Horizontal, this);
  buttonBox->addButton(exitButton, QDialogButtonBox::RejectRole);
  buttonBox->addButton(runButton, QDialogButtonBox::ActionRole);
  connect(exitButton, &QPushButton::clicked, this, &ZImgProcessDialog::reject);
  connect(runButton, &QPushButton::clicked, this, &ZImgProcessDialog::runWorker);

  return buttonBox;
}

void ZImgProcessDialog::runWorker()
{
  try {
    m_cancellationSource = std::make_unique<folly::CancellationSource>();
    m_hasError = false;

    m_worker = nullptr;
    m_workerName.clear();
    createWorker(m_worker, m_workerName);
    CHECK(m_worker);
    CHECK(!m_workerName.isEmpty());

    m_worker->setCancellationToken(m_cancellationSource->getToken());

    m_progressDialog = new QProgressDialog(this);
    m_progressDialog->setLabelText(QString("%1 Running...").arg(m_workerName));
    m_progressDialog->setAutoReset(false);
    m_progressDialog->setAttribute(Qt::WA_DeleteOnClose);
    QObject::disconnect(m_progressDialog, &QProgressDialog::canceled, m_progressDialog, &QProgressDialog::cancel);
    connect(m_worker, qOverload<int>(&ZImgProcess::progressChanged), m_progressDialog, &QProgressDialog::setValue);
    connect(m_worker, &ZImgProcess::processError, this, &ZImgProcessDialog::processError);
    connect(m_progressDialog, &QProgressDialog::canceled, this, &ZImgProcessDialog::cancelButtonPressed);

    auto thread = new QThread(this);
    connect(thread, &QThread::started, m_worker, &ZImgProcess::run);
    connect(m_worker, &ZImgProcess::finished, thread, &QThread::quit);
    connect(m_worker, &ZImgProcess::processError, thread, &QThread::quit);
    connect(thread, &QThread::finished, m_worker, &ZImgProcess::deleteLater);
    connect(thread, &QThread::finished, m_progressDialog, &QProgressDialog::reset);
    connect(thread, &QThread::finished, this, &ZImgProcessDialog::processFinished);
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    m_worker->moveToThread(thread);

    thread->start();
    m_progressDialog->exec();
  }
  catch (const ZException& e) {
    QMessageBox::critical(this, QApplication::applicationName(), QString("%1 Error.\n").arg(m_workerName) + e.what());
    return;
  }
}

} // namespace nim
