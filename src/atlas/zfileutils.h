#pragma once

#include <QString>

namespace nim {

class ZFileUtils
{
public:
  ZFileUtils() = default;

  static void showInGraphicalShell(const QString& filePath);
};

} // namespace nim

