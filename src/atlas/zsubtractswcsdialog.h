#pragma once

#include "zimgprocessdialog.h"

class QCheckBox;

namespace nim {

class ZDoc;
class ZSelectFileWidget;

class ZSubtractSwcsDialog : public ZImgProcessDialog
{
  Q_OBJECT

public:
  explicit ZSubtractSwcsDialog(ZDoc& doc, QWidget* parent = nullptr);

protected:
  WorkerSpec createWorkerSpec() override;

private:
  [[nodiscard]] QString inputSwcPath() const;
  [[nodiscard]] QString outputSwcPath() const;
  [[nodiscard]] QStringList subtractSwcPaths() const;
  [[nodiscard]] bool loadResultEnabled() const;

private:
  ZDoc& m_doc;

  ZSelectFileWidget* m_subtractSwcsWidget = nullptr;
  ZSelectFileWidget* m_inputSwcWidget = nullptr;
  ZSelectFileWidget* m_outputSwcWidget = nullptr;
  QCheckBox* m_loadResultCheck = nullptr;
};

} // namespace nim
