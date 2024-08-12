#pragma once

#include "zpunctum.h"
#include <list>

namespace nim {

class ZPuncta
{
public:
  std::list<ZPunctum> data;

  ZPuncta() = default;

  // might throw ZException
  explicit ZPuncta(const QString& filename);

  explicit ZPuncta(const std::list<ZPunctum>& p)
  {
    data = p;
  }

  ZPuncta(ZPuncta&&) = default;

  ZPuncta& operator=(ZPuncta&&) = default;

  ZPuncta(const ZPuncta&) = default;

  ZPuncta& operator=(const ZPuncta&) = default;

  void clear() noexcept
  {
    data.clear();
  }

  void swap(ZPuncta& other) noexcept
  {
    data.swap(other.data);
  }

  bool operator==(const ZPuncta& l) const
  {
    return data == l.data;
  }

  // qt style read write name filter for filedialog
  static bool canReadFile(const QString& filename);

  static bool canWriteFile(const QString& filename);

  static const QString& getQtReadNameFilter();

  static void getQtWriteNameFilter(QStringList& filters, QStringList& formats);

  // might throw ZException
  void load(const QString& filename);

  void save(const QString& filename, const QString& format = "") const;

  [[nodiscard]] std::string toString() const
  {
    return fmt::format("{} puncta", data.size());
  }
};

} // namespace nim
