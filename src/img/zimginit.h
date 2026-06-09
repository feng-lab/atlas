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

  // Explicit runtime teardown must run before process/static destruction begins.
  static void shutdown() noexcept;

private:
  ZImgInit(const QString& resourcesDIR, const QString& jreDIR, const QString& jarsDIR, bool verbose);
};

} // namespace nim
