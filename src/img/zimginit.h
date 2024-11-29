#pragma once

#include <QString>

namespace nim {

class ZImgInit
{
public:
  static const ZImgInit& instance(const QString& resourcesDIR = "",
                                  const QString& jreDIR = "",
                                  const QString& jarsDIR = "",
                                  bool verbose = true);

private:
  ZImgInit(const QString& resourcesDIR, const QString& jreDIR, const QString& jarsDIR, bool verbose);

  ~ZImgInit();
};

} // namespace nim
