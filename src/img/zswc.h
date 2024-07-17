#pragma once

#include "zglmutils.h"
#include "ztree.hpp"

namespace nim {

// http://research.mssm.edu/cnic/swc.html
struct SwcNode
{
  explicit SwcNode(int64_t id_ = -1,
                   int64_t type_ = -1,
                   double x_ = 0,
                   double y_ = 0,
                   double z_ = 0,
                   double radius_ = -1,
                   int64_t parentID_ = -2)
    : id(id_)
    , type(type_)
    , x(x_)
    , y(y_)
    , z(z_)
    , radius(radius_)
    , parentID(parentID_)
  {}

  int64_t id = -1;
  int64_t type = -1;
  double x = 0;
  double y = 0;
  double z = 0;
  double radius = -1;
  int64_t parentID = -2; // after tree change, parentID becomes invalid, use function parentID to get corrent parentID
  int64_t label = -1;
  bool selected = false;

  [[nodiscard]] std::string toString() const
  {
    return fmt::format("id:{}, type:{}, xyz:({}, {}, {}), radius:{}, parentID:{}, label:{}",
                       id,
                       type,
                       x,
                       y,
                       z,
                       radius,
                       parentID,
                       label);
  }
};

class ZSwc : public ZTree<SwcNode>
{
public:
  using SwcTreeNode = ZSwc::Iterator;
  using ConstSwcTreeNode = ZSwc::ConstIterator;

  static constexpr int64_t SomaType = 1;
  static constexpr int64_t AxonType = 2;
  static constexpr int64_t BasalDendriteType = 3;
  static constexpr int64_t ApicalDendriteType = 4;
  static constexpr int64_t MainTrunkType = 5;
  static constexpr int64_t BasalIntermediateType = 6;
  static constexpr int64_t BasalTerminalType = 7;
  static constexpr int64_t ApicalObliqueIntermediateType = 8;
  static constexpr int64_t ApicalObliqueTerminalType = 9;
  static constexpr int64_t ApicalTuftType = 10;

  ZSwc() = default;

  // might throw ZException
  explicit ZSwc(const QString& filename)
  {
    load(filename);
  }

  ZSwc(ZSwc&&) = default;

  ZSwc& operator=(ZSwc&&) = default;

  ZSwc(const ZSwc&) = default;

  ZSwc& operator=(const ZSwc&) = default;

  void swap(ZSwc& other) noexcept
  {
    ZTree<SwcNode>::swap(other);
  }

  template<typename Iter>
  static QString toQString(const Iter& pos)
  {
    return isNull(pos) ? QString("(Empty Node)")
                       : QString("id:%1, type:%2, x:%3, y:%4, z:%5, radius:%6, parentID:%7, label:%8")
                           .arg(pos->id)
                           .arg(pos->type)
                           .arg(pos->x)
                           .arg(pos->y)
                           .arg(pos->z)
                           .arg(pos->radius)
                           .arg(parentID(pos))
                           .arg(pos->label);
  }

  // pos must not be null
  template<typename Iter>
  static int64_t parentID(const Iter& pos)
  {
    return isNull(parent(pos)) ? -1 : parent(pos)->id;
  }

  SwcTreeNode thickestNode();

  SwcTreeNode thickestNode(const SwcTreeNode& subtree);

  // set type of soma as somaType, other as otherType, set root to thickest node
  void labelSomaAndOthers(double radiusThre = 0, int64_t somaType = SomaType, int64_t otherType = AxonType);

  // before calling this, soma nodes type must be somaType and other nodes type must not be somaType
  void resortPyramidal(int64_t basalType = BasalDendriteType,
                       int64_t apicalType = ApicalDendriteType,
                       int64_t somaType = SomaType);

  void resortID();

  // qt style read write name filter for filedialog
  static bool canReadFile(const QString& filename)
  {
    return filename.endsWith(".swc", Qt::CaseInsensitive);
  }

  static bool canWriteFile(const QString& filename)
  {
    return filename.endsWith(".swc", Qt::CaseInsensitive);
  }

  static QString getQtReadNameFilter()
  {
    return {"SWC files (*.swc)"};
  }

  static QString getQtWriteNameFilter()
  {
    return {"SWC files (*.swc)"};
  }

  // might throw ZException
  void load(const QString& filename);

  void save(const QString& filename) const;

  //
  void addLine(const std::vector<glm::dvec3>& line, double radius);

  [[nodiscard]] std::string toString() const
  {
    return fmt::format("{} nodes", size());
  }
};

} // namespace nim
