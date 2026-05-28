#pragma once

#include "zfolly.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QStringList>
#include <functional>
#include <memory>

class QPushButton;

namespace nim {

class ZDoc;

class ZImgProcess;

class ZBackgroundTask;

class ZImgProcessDialog : public QDialog
{
  Q_OBJECT

public:
  struct WorkerSpec
  {
    // Human-readable operation label (shown in Tasks UI).
    QString workerName;

    // Optional override for the Tasks UI title.
    QString taskTitle;

    // Optional success message for the Tasks UI.
    QString successMessage;

    // Create the worker on the background thread that will execute it.
    // This must not capture any Qt UI objects.
    folly::Function<std::unique_ptr<ZImgProcess>()> makeWorker;

    // Optional UI-thread callback to run after a successful completion.
    // Intended for things like loading the output object into the document.
    std::function<void(ZDoc& doc, ZBackgroundTask& task)> onSuccessUi;
  };

  explicit ZImgProcessDialog(ZDoc& doc, QWidget* parent = nullptr);

protected:
  // Subclass fills the WorkerSpec, throwing ZException on validation failures.
  virtual WorkerSpec createWorkerSpec() = 0;

  [[nodiscard]] static QString makeUniqueOutputPath(const QString& suggestedPath);
  [[nodiscard]] static QStringList makeUniqueOutputPaths(const QStringList& suggestedPaths);

  void keyPressEvent(QKeyEvent* e) override;

  QDialogButtonBox* createButtonBox(const QString& runButtonName = tr("Run"),
                                    const QString& exitButtonName = tr("Exit"));

private:
  void runWorker();

private:
  ZDoc* m_doc = nullptr;

  QPushButton* m_runButton = nullptr;
};

} // namespace nim
