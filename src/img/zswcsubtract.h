#pragma once

#include "zimgprocess.h"

#include <QStringList>

namespace nim {

class ZSwcSubtract : public ZImgProcess
{
public:
  void setInputSwcFilename(const QString& fn)
  {
    m_inputSwcFilename = fn;
  }

  void setSubtractSwcFilenames(const QStringList& fns)
  {
    m_subtractSwcFilenames = fns;
  }

  void setOutputSwcFilename(const QString& fn)
  {
    m_outputSwcFilename = fn;
  }

protected:
  void doWork() override;

  void read(const json::object& jo) override;

  void write(json::object& jo) const override;

private:
  QString m_inputSwcFilename;
  QStringList m_subtractSwcFilenames;
  QString m_outputSwcFilename;
};

} // namespace nim
