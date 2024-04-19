#pragma once

#include "zpunctum.h"
#include <list>

namespace nim {

class ZPuncta
{
public:
  std::list<ZPunctum> data;

  ZPuncta() = default;

  // might throw ZIOException
  explicit ZPuncta(const QString& filename);

  explicit ZPuncta(const std::list<ZPunctum>& p)
  {
    data = p;
  }

  ZPuncta(ZPuncta&&) = default;

  ZPuncta& operator=(ZPuncta&&) = default;

  ZPuncta(const ZPuncta&) = default;

  ZPuncta& operator=(const ZPuncta&) = default;

  inline void clear() noexcept
  {
    data.clear();
  }

  inline void swap(ZPuncta& other) noexcept
  {
    data.swap(other.data);
  }

  inline bool operator==(const ZPuncta& l) const
  {
    return data == l.data;
  }

  // qt style read write name filter for filedialog
  static bool canReadFile(const QString& filename);

  static bool canWriteFile(const QString& filename);

  static const QString& getQtReadNameFilter();

  static void getQtWriteNameFilter(QStringList& filters, QStringList& formats);

  // might throw ZIOException
  void load(const QString& filename);

  void save(const QString& filename, const QString& format = "") const;

  [[nodiscard]] inline QString toQString() const
  {
    return QString("%1 puncta").arg(data.size());
  }

  [[nodiscard]] inline std::string toString() const
  {
    return fmt::format("{} puncta", data.size());
  }
};

} // namespace nim
