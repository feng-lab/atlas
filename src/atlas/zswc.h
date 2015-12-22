#ifndef ZSWC_H
#define ZSWC_H

#include <QString>
#include "ztree.hpp"
#include "zglobal.h"
#include "zglmutils.h"

namespace nim {

// http://research.mssm.edu/cnic/swc.html
struct SwcNode
{
  SwcNode(int id = -1, int type = -1, double x = 0, double y = 0, double z = 0,
          double radius = -1, int parentID = -2)
    : id(id), type(type), x(x), y(y), z(z), radius(radius), parentID(parentID), label(-1)
  {}

  int id;
  int type;
  double x;
  double y;
  double z;
  double radius;
  int parentID;   // after tree change, parentID becomes invalid, use function parentID to get corrent parentID
  int label;

//  bool operator==(const SwcNode &rhs) const
//  {
//    return id == rhs.id &&
//        type == rhs.type &&
//        x == rhs.x &&
//        y == rhs.y &&
//        z == rhs.z &&
//        radius == rhs.radius &&
//        parentID == rhs.parentID &&
//        label == rhs.label;
//  }
//  inline bool operator!=(const SwcNode &rhs) const { return !(*this == rhs); }
};

class ZSwc : public ZTree<SwcNode>
{
public:
  typedef ZSwc::Iterator SwcTreeNode;
  typedef ZSwc::ConstIterator ConstSwcTreeNode;

  ZSwc() : ZTree<SwcNode>() {}
  // might throw ZIOException
  explicit ZSwc(const QString &filename) { load(filename); }

#ifndef _USE_MSVC2013_
  ZSwc(ZSwc&&) = default;
  ZSwc& operator=(ZSwc&&) = default;
#endif
  ZSwc(const ZSwc&) = default;
  ZSwc& operator=(const ZSwc&) = default;

  inline void swap(ZSwc &other) noexcept { ZTree<SwcNode>::swap(other); }

  template<typename Iter>
  static QString toQString(const Iter& pos)
  {
    return isNull(pos) ? QString("(Empty Node)") :
                         QString("id:%1, type:%2, x:%3, y:%4, z:%5, radius:%6, parentID:%7")
                         .arg(pos->id).arg(pos->type).arg(pos->x).arg(pos->y).arg(pos->z)
                         .arg(pos->radius).arg(parentID(pos));
  }

  // pos must not be null
  template<typename Iter>
  static int parentID(const Iter& pos) { return isNull(parent(pos)) ? -1 : parent(pos)->id; }

  SwcTreeNode thickestNode();
  SwcTreeNode thickestNode(const SwcTreeNode &subtree);

  // set type of soma as somaType, other as otherType, set root to thickest node
  void labelSomaAndOthers(double radiusThre = 0, int somaType = 1, int otherType = 2);
  // before calling this, soma nodes type must be somaType and other nodes type must not be somaType
  void resortPyramidal(int basalType = 3, int apicalType = 4, int somaType = 1);
  void resortID();

  // qt style read write name filter for filedialog
  static bool canReadFile(const QString& filename) { return filename.endsWith(".swc", Qt::CaseInsensitive); }
  static bool canWriteFile(const QString& filename) { return filename.endsWith(".swc", Qt::CaseInsensitive); }
  static QString getQtReadNameFilter() { return QString("SWC files (*.swc)"); }
  static QString getQtWriteNameFilter()  { return QString("SWC files (*.swc)"); }
  // might throw ZIOException
  void load(const QString &filename);
  void save(const QString &filename) const;

  //
  void addLine(const std::vector<glm::dvec3>& line, double radius);
};

} // namespace nim

#endif // ZSWC_H
