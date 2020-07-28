#pragma once

#include "ztree.hpp"
#include "zroi.h"
#include "zmesh.h"
#include <QString>
#include <QStringList>
#include <cstdint>
#include <memory>
#include <utility>

namespace nim {

struct RegionNode
{
  explicit RegionNode(int64_t id_ = -1, int64_t parentID_ = -2, int red_ = 255, int green_ = 255, int blue_ = 255,
                      QString name_ = "Undefined", QString abbreviation_ = "Undefined")
    : id(id_), parentID(parentID_), red(red_), green(green_), blue(blue_)
    , name(std::move(name_)), abbreviation(std::move(abbreviation_))
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
void readMouseBrainAtlasOntology(ZTree<RegionNode>& ontology);

void readMouseBrainAtlasOntology(const QStringList& regionAbbrevs, ZTree<RegionNode>& ontology);

// throw if abbreviation not found
int64_t idOfRegionAbbreviation(const QString& abbreviation, const ZTree<RegionNode>& ontology);

std::vector<int64_t> allIDsWithinRegionAbbreviation(const QString& abbreviation, const ZTree<RegionNode>& ontology);

// input img, an unsigned 8-bit single-channel image. Pixels with value 1 is treated as foreground. Other pixels are background.
void binaryImgToMesh(const ZImg& img, ZMesh& msh, double scale = 1.0);

// input img, an unsigned 8-bit single-channel image. Non-zero pixels are treated as 1's. Zero pixels remain 0's, so the image is treated as binary.
void binaryImgToROI(const ZImg& img, ZROI& roi, double scale = 1.0);

} // namespace nim

