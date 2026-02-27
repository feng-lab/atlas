#include "zimgprocessdialog.h"

#include "zbackgroundjob.h"
#include "zbackgroundtaskmanager.h"
#include "zdoc.h"
#include "zimgprocess.h"
#include "zlog.h"
#include "zmessageboxhelpers.h"

#include <QKeyEvent>
#include <QApplication>
#include <QPushButton>

#include <folly/executors/GlobalExecutor.h>

#include <utility>

namespace nim {

ZImgProcessDialog::ZImgProcessDialog(ZDoc& doc, QWidget* parent)
  : QDialog(parent)
  , m_doc(&doc)
{
  CHECK(m_doc != nullptr);
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
  m_runButton = new QPushButton(runButtonName, this);
  auto exitButton = new QPushButton(exitButtonName, this);
  auto buttonBox = new QDialogButtonBox(Qt::Horizontal, this);
  buttonBox->addButton(exitButton, QDialogButtonBox::RejectRole);
  buttonBox->addButton(m_runButton, QDialogButtonBox::ActionRole);
  connect(exitButton, &QPushButton::clicked, this, &ZImgProcessDialog::reject);
  connect(m_runButton, &QPushButton::clicked, this, &ZImgProcessDialog::runWorker);

  return buttonBox;
}

void ZImgProcessDialog::runWorker()
{
  CHECK(m_doc != nullptr);

  try {
    focusNextChild();

    WorkerSpec spec = createWorkerSpec();
    CHECK(!spec.workerName.isEmpty());
    CHECK(spec.makeWorker);

    if (m_runButton != nullptr) {
      m_runButton->setEnabled(false);
    }

    const QString workerName = std::move(spec.workerName);
    const QString taskTitle = spec.taskTitle.isEmpty() ? workerName : std::move(spec.taskTitle);
    const QString successMessage = std::move(spec.successMessage);
    auto onSuccessUi = std::move(spec.onSuccessUi);
    auto makeWorker = std::move(spec.makeWorker);

    ZBackgroundJobSpec job;
    job.title = taskTitle;
    job.runningMessage = QStringLiteral("running");
    job.useFakeProgress = true;
    job.executor = folly::getGlobalCPUExecutor();
    job.debugLabel = workerName.toStdString();
    job.work = [workerName, successMessage, onSuccessUi = std::move(onSuccessUi), makeWorker = std::move(makeWorker)](
                 ZBackgroundJobContext ctx) mutable -> folly::coro::Task<ZBackgroundJobOutcome> {
      ZBackgroundJobOutcome outcome;

      try {
        std::unique_ptr<ZImgProcess> worker = makeWorker();
        if (!worker) {
          throw ZException(QStringLiteral("Failed to start %1: worker creation returned null.").arg(workerName));
        }

        const folly::CancellationToken token = ctx.cancellationToken();
        worker->setCancellationToken(token);
        worker->setProgressCallback([ctx](double p01) mutable {
          ctx.setProgress01(p01);
        });
        worker->run();

        outcome.state = ZBackgroundJobOutcome::State::Succeeded;
        outcome.message = successMessage;
        if (onSuccessUi) {
          outcome.uiCallback = [onSuccessUi = std::move(onSuccessUi)](ZDoc& doc, ZBackgroundTask& task) {
            onSuccessUi(doc, task);
          };
        }
      }
      catch (const ZCancellationException&) {
        outcome.state = ZBackgroundJobOutcome::State::Cancelled;
        outcome.message = QStringLiteral("cancelled");
      }
      catch (const std::exception& e) {
        const QString err = QString::fromUtf8(e.what());
        outcome.state = ZBackgroundJobOutcome::State::Failed;
        outcome.message = err;
        outcome.uiCallback = [workerName, err](ZDoc&, ZBackgroundTask&) {
          showCriticalWithDetails(QApplication::activeWindow(), QObject::tr("Error during %1").arg(workerName), err);
        };
      }
      catch (...) {
        const QString err = QStringLiteral("unknown error");
        outcome.state = ZBackgroundJobOutcome::State::Failed;
        outcome.message = err;
        outcome.uiCallback = [workerName, err](ZDoc&, ZBackgroundTask&) {
          showCriticalWithDetails(QApplication::activeWindow(), QObject::tr("Error during %1").arg(workerName), err);
        };
      }

      co_return outcome;
    };

    (void)startBackgroundJob(*m_doc, std::move(job));
    accept();
  }
  catch (const ZException& e) {
    showCriticalWithDetails(this, tr("Error"), e.what());
    if (m_runButton != nullptr) {
      m_runButton->setEnabled(true);
    }
  }
}

} // namespace nim
