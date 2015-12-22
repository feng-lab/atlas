#ifndef ZREGIONONTOLOGY_H
#define ZREGIONONTOLOGY_H

#include <cstdint>
#include <memory>
#include <QString>
#include <QStringList>
#include "ztree.hpp"
#include "zroi.h"
#include "zmesh.h"

namespace nim {

struct RegionNode
{
  RegionNode(int64_t id = -1, int64_t parentID = -1, int red = 0, int green = 0, int blue = 0,
             QString name = "", QString abbreviation = "")
    : id(id), parentID(parentID), red(red), green(green), blue(blue)
    , name(name), abbreviation(abbreviation)
  {}

  int64_t id;
  int64_t parentID;
  int red;
  int green;
  int blue;
  QString name;
  QString abbreviation;
  std::shared_ptr<ZROI> roi;
  std::shared_ptr<ZMesh> mesh;

//  bool operator==(const RegionNode &rhs) const
//  {
//    return id == rhs.id &&
//        parentID == rhs.parentID &&
//        red == rhs.red &&
//        green == rhs.green &&
//        blue == rhs.blue &&
//        name == rhs.name &&
//        abbreviation == rhs.abbreviation;
//  }
//  inline bool operator!=(const RegionNode &rhs) const { return !(*this == rhs); }
};

// ZIOException
void readMouseBrainAtlasOntology(ZTree<RegionNode> &ontology);
void readMouseBrainAtlasOntology(const QStringList& regionAbbrevs, ZTree<RegionNode> &ontology);

// throw if abbreviation not found
int64_t idOfRegionAbbreviation(const QString &abbreviation, const ZTree<RegionNode> &ontology);
std::vector<int64_t> allIDsWithinRegionAbbreviation(const QString &abbreviation, const ZTree<RegionNode> &ontology);

// input img, an unsigned 8-bit single-channel image. Pixels with value 1 is treated as foreground. Other pixels are background.
void binaryImgToMesh(const ZImg &img, ZMesh &msh);
// input img, an unsigned 8-bit single-channel image. Non-zero pixels are treated as 1's. Zero pixels remain 0's, so the image is treated as binary.
void binaryImgToROI(const ZImg &img, ZROI &roi);

} // namespace nim

#endif // ZREGIONONTOLOGY_H
