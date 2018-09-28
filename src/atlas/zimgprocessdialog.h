#pragma once

#include <QDialog>
#include <QProgressDialog>
#include <QDialogButtonBox>

namespace nim {

class ZImgProcess;

class ZImgProcessDialog : public QDialog
{
  Q_OBJECT
public:
  explicit ZImgProcessDialog(QWidget* parent = nullptr);

protected:
  // subclass create worker, throw ZImgException if error
  // worker and workerName must be assigned to valid value if no exception was thrown
  virtual void createWorker(ZImgProcess*& worker, QString& workerName) = 0;

  void processCanceled();

  void processFinished();

  void processError(const QString& e);

  void cancelButtonPressed();

  void keyPressEvent(QKeyEvent* e) override;

  QDialogButtonBox* createButtonBox(const QString& runButtonName = tr("Run"),
                                    const QString& exitButtonName = tr("Exit"));

private:
  void runWorker();

private:
  QString m_workerName;
  std::atomic<bool> m_isCanceled;
  bool m_hasError;

  QProgressDialog* m_progressDialog = nullptr;
  ZImgProcess* m_worker = nullptr;
};

} // namespace nim

