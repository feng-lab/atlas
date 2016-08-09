#pragma once

#include <QString>

namespace nim {

class ZFileUtils
{
public:
  ZFileUtils();

  ~ZFileUtils();

  static void showInGraphicalShell(const QString& filePath);
};

} // namespace nim

