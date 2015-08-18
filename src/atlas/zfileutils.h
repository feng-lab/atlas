#ifndef ZFILEUTILS_H
#define ZFILEUTILS_H

#include <QString>

namespace nim {

class ZFileUtils
{
public:
  ZFileUtils();
  ~ZFileUtils();

  static void showInGraphicalShell(const QString &filePath);
};

} // namespace nim

#endif // ZFILEUTILS_H
