#ifndef ZPUNCTA_H
#define ZPUNCTA_H

#include <QLinkedList>
#include "zpunctum.h"
#include "zglmutils.h"
#include "zglobal.h"

namespace nim {

// Iterators pointing to an item in a QLinkedList remain valid as long as the item exists,
// whereas iterators to a QList can become invalid after any insertion or removal
class ZPuncta : public QLinkedList<ZPunctum>
{
  friend class ZPunctaIO;
public:
  inline ZPuncta() { }
  inline explicit ZPuncta(const ZPunctum& i) { append(i); }
  // might throw ZIOException
  explicit ZPuncta(const QString &filename);

  ZPuncta(ZPuncta&&) = default;
  ZPuncta& operator=(ZPuncta&&) = default;
  ZPuncta(const ZPuncta&) = default;
  ZPuncta& operator=(const ZPuncta&) = default;

  inline void swap(ZPuncta &other) noexcept { QLinkedList<ZPunctum>::swap(other); }
  bool operator==(const ZPuncta &l) const { return QLinkedList<ZPunctum>::operator ==(l); }
  inline bool operator!=(const ZPuncta &l) const { return !(*this == l); }

  // qt style read write name filter for filedialog
  static bool canReadFile(const QString& filename);
  static bool canWriteFile(const QString& filename);
  static const QString& getQtReadNameFilter();
  static void getQtWriteNameFilter(QStringList &filters, QStringList &formats);
  // might throw ZIOException
  void load(const QString &filename);
  void save(const QString &filename, const QString& format = "") const;
};

} // namespace nim

#endif // ZPUNCTA_H
