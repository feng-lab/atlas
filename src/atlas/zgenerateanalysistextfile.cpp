#include "zgenerateanalysistextfile.h"

#include "zimg.h"
#include "zimggraph.h"
#include "zimgautothreshold.h"
#include "zglmutils.h"
#include "zioutils.h"
#include "zcsvtable.h"
#include "zlog.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>

namespace nim {
namespace {

QStringList toQStringList(const ZCsvRow& row)
{
  QStringList fields;
  for (const auto& field : row) {
    fields << QString::fromStdString(field);
  }
  return fields;
}

} // namespace

void ZGenerateAnalysisTextFile::generate(const ZAnalysisTextFileInput& input)
{
  m_input = input;
  generate();
}

void ZGenerateAnalysisTextFile::generate(const QString& imgFilename,
                                         const QString& swcFilename,
                                         const QString& punctaFilename)
{
  m_input.imgFilename = imgFilename;
  m_input.swcFilename = swcFilename;
  m_input.punctaFilename = punctaFilename;
  generate();
}

void ZGenerateAnalysisTextFile::generate(const QString& worklistFile)
{
  checkFileExist(worklistFile);

  QStringList header;
  header << "# imageName"
         << "swcName"
         << "punctaName"
         << "voxelSizeXInUm"
         << "voxelSizeYInUm"
         << "voxelSizeZInUm"
         << "dendriteChannel"
         << "axonChannel(can be empty)"
         << "maxDistToBranch"
         << "bluenessExtend"
         << "outputFolder(can be empty)"
         << "doPyramidalFunctionalSeparation(yes or no)"
         << "doPyramidalSubclassSeparation(yes or no)"
         << "somaPunctaName";

  const ZCsvTable allLines = readCsvTable(worklistFile);
  if (allLines.empty()) {
    throw ZException(fmt::format("Can not parse file ({}) or file is empty", worklistFile));
  }

  for (const auto& row : allLines) {
    const QStringList list = toQStringList(row);
    if (list.empty() || list.at(0).startsWith("#")) {
      continue;
    }
    auto throwLineParseError = [&]() -> void {
      throw ZException(fmt::format("Can not parse line ({}) with format <{}>", list.join(','), header.join(',')));
    };
    if (list.size() != header.size()) {
      throw ZException(
        fmt::format("Wrong number of items in line ({}), expected format: <{}>", list.join(','), header.join(',')));
    }
    bool ok = false;
    m_input.imgFilename = list[0];
    m_input.swcFilename = list[1];
    m_input.punctaFilename = list[2];
    if (!list[3].isEmpty()) {
      m_input.voxelSizeX = list[3].toDouble(&ok);
      if (!ok) {
        throwLineParseError();
      }
    }
    if (!list[4].isEmpty()) {
      m_input.voxelSizeY = list[4].toDouble(&ok);
      if (!ok) {
        throwLineParseError();
      }
    }
    if (!list[5].isEmpty()) {
      m_input.voxelSizeZ = list[5].toDouble(&ok);
      if (!ok) {
        throwLineParseError();
      }
    }
    m_input.dendriteChannel = list[6].toInt(&ok);
    if (!ok) {
      throwLineParseError();
    }
    if (!list[7].isEmpty()) {
      m_input.axonChannel = list[7].toInt(&ok);
      if (!ok) {
        throwLineParseError();
      }
    }
    m_input.maxDistToBranch = list[8].toDouble(&ok);
    if (!ok) {
      throwLineParseError();
    }
    m_input.bluenessExtend = list[9].toDouble(&ok);
    if (!ok) {
      throwLineParseError();
    }
    m_input.outputFolder = list[10];
    if (list[11].compare("yes", Qt::CaseInsensitive) == 0) {
      m_input.doPyramidalFunctionalSeparation = true;
    } else if (list[11].compare("no", Qt::CaseInsensitive) == 0) {
      m_input.doPyramidalFunctionalSeparation = false;
    } else {
      throwLineParseError();
    }
    if (list[12].compare("yes", Qt::CaseInsensitive) == 0) {
      m_input.doPyramidalSubclassSeparation = true;
    } else if (list[12].compare("no", Qt::CaseInsensitive) == 0) {
      m_input.doPyramidalSubclassSeparation = false;
    } else {
      throwLineParseError();
    }
    m_input.somaPunctaFilename = list[13];

    generate();
  }
}

void ZGenerateAnalysisTextFile::generate()
{
  m_input.imgFilename = m_input.imgFilename.trimmed();
  m_input.swcFilename = m_input.swcFilename.trimmed();
  m_input.punctaFilename = m_input.punctaFilename.trimmed();
  m_input.outputFolder = m_input.outputFolder.trimmed();
  m_input.somaPunctaFilename = m_input.somaPunctaFilename.trimmed();

  checkFileExist(m_input.imgFilename);
  checkFileExist(m_input.swcFilename);
  checkFileExist(m_input.punctaFilename);
  checkFileExist(m_input.somaPunctaFilename);

  ZImgInfo info = ZImg::readImgInfos(m_input.imgFilename).at(0);
  if (info.voxelSizeUnit != VoxelSizeUnit::none) {
    if (m_input.voxelSizeX != -1) {
      m_input.voxelSizeX = info.voxelSizeXInUm();
    } else {
      if (m_input.voxelSizeX != info.voxelSizeXInUm()) {
        throw ZException(fmt::format("voxel size x {} doesn't match voxel size x from img {}",
                                     m_input.voxelSizeX,
                                     info.voxelSizeXInUm()));
      }
    }
    if (m_input.voxelSizeY != -1) {
      m_input.voxelSizeY = info.voxelSizeYInUm();
    } else {
      if (m_input.voxelSizeY != info.voxelSizeYInUm()) {
        throw ZException(fmt::format("voxel size y {} doesn't match voxel size y from img {}",
                                     m_input.voxelSizeY,
                                     info.voxelSizeYInUm()));
      }
    }
    if (m_input.voxelSizeZ != -1) {
      m_input.voxelSizeZ = info.voxelSizeZInUm();
    } else {
      if (m_input.voxelSizeZ != info.voxelSizeZInUm()) {
        throw ZException(fmt::format("voxel size z {} doesn't match voxel size z from img {}",
                                     m_input.voxelSizeZ,
                                     info.voxelSizeZInUm()));
      }
    }
  }
  if (m_input.voxelSizeX == -1 || m_input.voxelSizeY == -1 || m_input.voxelSizeZ == -1) {
    throw ZException(fmt::format("need valid voxel size information: {}, {}, {}",
                                 m_input.voxelSizeX,
                                 m_input.voxelSizeY,
                                 m_input.voxelSizeZ));
  }

  if (m_input.dendriteChannel < 0 || m_input.dendriteChannel >= static_cast<int>(info.numChannels)) {
    throw ZException(
      fmt::format("invalid dendrite channel {} of file {}", m_input.dendriteChannel, m_input.imgFilename));
  }
  if (m_input.axonChannel >= 0 && m_input.axonChannel >= static_cast<int>(info.numChannels)) {
    throw ZException(fmt::format("invalid axon channel {} of file {}", m_input.axonChannel, m_input.imgFilename));
  }
  if (m_input.dendriteChannel == m_input.axonChannel) {
    throw ZException(fmt::format("dendrite channel and axon channel are both {}", m_input.dendriteChannel));
  }

  ZSwc tree(m_input.swcFilename);
  if (tree.numRoots() != 1) {
    throw ZException(fmt::format("wrong swc file {} with {} roots.", m_input.swcFilename, tree.numRoots()));
  }

  QFileInfo swcFileInfo(m_input.swcFilename);
  if (m_input.outputFolder.isEmpty()) {
    QDir swcFileDir = swcFileInfo.dir();
    m_input.outputFolder = swcFileDir.filePath(swcFileInfo.completeBaseName());
    swcFileDir.mkdir(swcFileInfo.completeBaseName());
  } else {
    QDir outDir = QDir(m_input.outputFolder);
    m_input.outputFolder = outDir.filePath(swcFileInfo.completeBaseName());
    outDir.mkpath(swcFileInfo.completeBaseName());
  }
  QDir outputDir(m_input.outputFolder);
  if (!outputDir.exists()) {
    throw ZException(fmt::format("output folder {} does not exist and can not be created", m_input.outputFolder));
  }

  m_processedSwcFilename = outputDir.filePath(swcFileInfo.fileName());
  if (m_input.doPyramidalFunctionalSeparation || m_input.doPyramidalSubclassSeparation) {
    if (inputSwcIsPyramidal()) { // already pyramidal, don't need process
      tree.setAsRoot(tree.thickestNode());
      tree.resortID();
      tree.save(m_processedSwcFilename);
      // QFile::copy(m_input.swcFilename, m_processedSwcFilename);
    } else { // make it pyramidal
      throw ZException(fmt::format("input SWC {} is not pyramidal SWC", m_input.swcFilename));
      //      // mark soma from swc nodes
      //      tree.labelSomaAndOthers(3.0 / m_input.voxelSizeX);  // soma radius at least 3um
      //      tree.resortPyramidal();
      //      m_processedSwcFilename = outputDir.filePath(swcFileInfo.completeBaseName()) + "Py.swc";
      //      tree.resortID();
      //      tree.save(m_processedSwcFilename);
    }
  } else {
    // mark soma from swc nodes
    tree.labelSomaAndOthers(0); // 3.0 / m_input.voxelSizeX);  // soma radius at least 3um
    tree.resortID();
    tree.save(m_processedSwcFilename);
  }

  std::map<ConstSwcTreeNode, double> nodeToBlueness;
  std::map<ConstSwcTreeNode, size_t> nodeToLayer;
  std::map<ConstSwcTreeNode, size_t> nodeToSubclass;

  // blueness
  getAxonFeature(tree, nodeToBlueness);
  m_bluenessSwcFilename = m_processedSwcFilename;
  m_bluenessSwcFilename.replace(".swc", "_blueness.swc", Qt::CaseInsensitive);
  writeFeatureSwc(tree, nodeToBlueness, m_bluenessSwcFilename);
  // layer
  QString layerSwcName = m_input.swcFilename;
  layerSwcName.replace(".swc", "_layer.swc", Qt::CaseInsensitive);
  QFileInfo layerSwcFileInfo(layerSwcName);
  if (layerSwcFileInfo.exists()) {
    m_layerSwcFilename = outputDir.filePath(layerSwcFileInfo.fileName());
    QFile::copy(layerSwcName, m_layerSwcFilename);
    ZSwc layerTree(m_layerSwcFilename);
    if (layerTree.numRoots() != 1) {
      throw ZException(fmt::format("wrong layer swc file {} with {} roots.", m_layerSwcFilename, layerTree.numRoots()));
    }
    // mark soma from swc nodes
    layerTree.setAsRoot(layerTree.thickestNode());
    layerTree.resortID();
    layerTree.save(m_layerSwcFilename);
    getLayerFeature(tree, layerTree, nodeToLayer);

    QString layerWithSomaSwcName = m_input.swcFilename;
    layerWithSomaSwcName.replace(".swc", "_layer_with_soma.swc", Qt::CaseInsensitive);
    QString layerWithSomaSwcFilename = outputDir.filePath(QFileInfo(layerWithSomaSwcName).fileName());
    ConstSwcTreeNode tn = tree.begin();
    SwcTreeNode layerTn = layerTree.begin();
    while (tn != tree.end()) {
      if (glm::length(glm::dvec3(tn->x - layerTn->x, tn->y - layerTn->y, tn->z - layerTn->z)) > 1.) {
        LOG(WARNING) << "node " << tn->x << " " << tn->y << " " << tn->z << " " << tn->radius;
        LOG(WARNING) << "layer node " << layerTn->x << " " << layerTn->y << " " << layerTn->z << " " << layerTn->radius;
        throw ZException("wrong layer node match");
      }
      if (tn->type == ZSwc::SomaType) {
        layerTn->type = 6;
      }
      ++tn;
      ++layerTn;
    }
    layerTree.save(layerWithSomaSwcFilename);
  }
  // subclass
  if (m_input.doPyramidalSubclassSeparation) {
    QString subclassSwcName = m_input.swcFilename;
    subclassSwcName.replace(".swc", "_subclass.swc", Qt::CaseInsensitive);
    QFileInfo subclassSwcFileInfo(subclassSwcName);
    if (subclassSwcFileInfo.exists()) {
      m_subclassSwcFilename = outputDir.filePath(subclassSwcFileInfo.fileName());
      QFile::copy(subclassSwcName, m_subclassSwcFilename);
      ZSwc subclassTree(m_subclassSwcFilename);
      if (subclassTree.numRoots() != 1) {
        throw ZException(
          fmt::format("wrong subclass swc file {} with {} roots.", m_subclassSwcFilename, subclassTree.numRoots()));
      }
      subclassTree.setAsRoot(subclassTree.thickestNode());
      subclassTree.resortID();
      subclassTree.save(m_subclassSwcFilename);
      getSubclassFeature(tree, subclassTree, nodeToSubclass);
    } else {
      throw ZException(fmt::format("Can not find subclass SWC {}.", m_subclassSwcFilename));
    }
  }

  mergeSoma(tree, nodeToBlueness, nodeToLayer);
  removeSmallLeafBranch(tree, 6, 5);

  generateAnalysisFiles(tree, nodeToBlueness, nodeToLayer, nodeToSubclass);
}

void ZGenerateAnalysisTextFile::checkFileExist(const QString& filename) const
{
  if (!QFile::exists(filename)) {
    throw ZException(fmt::format("file {} doesn't exist", filename));
  }
}

void ZGenerateAnalysisTextFile::getAxonFeature(const ZSwc& tree,
                                               std::map<ConstSwcTreeNode, double>& nodeToBlueness) const
{
  if (m_input.axonChannel < 0) {
    return;
  }
  ZImg axonImg(m_input.imgFilename,
               ZImgRegion(0, -1, 0, -1, 0, -1, m_input.axonChannel, m_input.axonChannel + 1, 0, 1));
  for (ConstSwcTreeNode tn = tree.begin(); tn != tree.end(); ++tn) {
    if (ZSwc::isRoot(tn)) {
      double maxr = tn->radius + m_input.bluenessExtend / m_input.voxelSizeX;
      double zscale = m_input.voxelSizeZ / m_input.voxelSizeX;
      index_t left = std::floor(tn->x - maxr);
      index_t right = std::ceil(tn->x + maxr);
      index_t up = std::floor(tn->y - maxr);
      index_t down = std::ceil(tn->y + maxr);
      index_t zup = std::floor(tn->z - maxr / zscale);
      index_t zdown = std::ceil(tn->z + maxr / zscale);

      left = std::max(0_z, left);
      right = std::min(right, static_cast<index_t>(axonImg.width()) - 1);
      up = std::max(0_z, up);
      down = std::min(down, static_cast<index_t>(axonImg.height()) - 1);
      zup = std::max(0_z, zup);
      zdown = std::min(zdown, static_cast<index_t>(axonImg.depth()) - 1);

      index_t count = 0;
      double intensity = 0.0;
      for (auto z = zup; z <= zdown; z++) {
        for (auto y = up; y <= down; y++) {
          for (auto x = left; x <= right; x++) {
            if ((x == roundTo<index_t>(tn->x) && y == roundTo<index_t>(tn->y) && z == roundTo<index_t>(tn->z)) ||
                pointSphereDist(x, y, z, tn) <= m_input.bluenessExtend) {
              count++;
              intensity += axonImg.value<double>(x, y, z);
            }
          }
        }
      }

      if (count > 0) {
        intensity /= count;
      } else {
        LOG(WARNING) << "node " << tn->x << " " << tn->y << " " << tn->z << " " << tn->radius;
        LOG(WARNING) << "img info " << axonImg.info();
        throw ZException("Swc root don't overlap with img?");
      }

      nodeToBlueness[tn] = intensity;
    } else {
      ConstSwcTreeNode parent = ZSwc::parent(tn);
      double maxr = std::max(tn->radius + m_input.bluenessExtend / m_input.voxelSizeX,
                             parent->radius + m_input.bluenessExtend / m_input.voxelSizeX);
      double zscale = m_input.voxelSizeZ / m_input.voxelSizeX;
      index_t left = std::floor(std::min(tn->x - maxr, parent->x - maxr));
      index_t right = std::ceil(std::max(tn->x + maxr, parent->x + maxr));
      index_t up = std::floor(std::min(tn->y - maxr, parent->y - maxr));
      index_t down = std::ceil(std::max(tn->y + maxr, parent->y + maxr));
      index_t zup = std::floor(std::min(tn->z - maxr / zscale, parent->z - maxr / zscale));
      index_t zdown = std::ceil(std::max(tn->z + maxr / zscale, parent->z + maxr / zscale));

      left = std::max(0_z, left);
      right = std::min(right, static_cast<index_t>(axonImg.width()) - 1);
      up = std::max(0_z, up);
      down = std::min(down, static_cast<index_t>(axonImg.height()) - 1);
      zup = std::max(0_z, zup);
      zdown = std::min(zdown, static_cast<index_t>(axonImg.depth()) - 1);

      index_t count = 0;
      double intensity = 0.0;
      for (auto z = zup; z <= zdown; z++) {
        for (auto y = up; y <= down; y++) {
          for (auto x = left; x <= right; x++) {
            if ((x == roundTo<index_t>(tn->x) && y == roundTo<index_t>(tn->y) && z == roundTo<index_t>(tn->z)) ||
                (x == roundTo<index_t>(parent->x) && y == roundTo<index_t>(parent->y) &&
                 z == roundTo<index_t>(parent->z)) ||
                pointFrustumConeDist(x, y, z, tn, parent) <= m_input.bluenessExtend) {
              count++;
              intensity += axonImg.value<double>(x, y, z);
            }
          }
        }
      }

      if (count > 0) {
        intensity /= count;
      } else {
        LOG(WARNING) << "node " << tn->x << " " << tn->y << " " << tn->z << " " << tn->radius;
        LOG(WARNING) << "parent node " << parent->x << " " << parent->y << " " << parent->z << " " << parent->radius;
        LOG(WARNING) << "img info " << axonImg.info();
        throw ZException("Swc seg don't overlap with img?");
      }

      nodeToBlueness[tn] = intensity;
    }
  }
}

void ZGenerateAnalysisTextFile::getLayerFeature(const ZSwc& tree,
                                                const ZSwc& layerTree,
                                                std::map<ConstSwcTreeNode, size_t>& nodeToLayer) const
{
  ConstSwcTreeNode tn = tree.begin();
  ConstSwcTreeNode layerTn = layerTree.begin();
  while (tn != tree.end()) {
    if (glm::length(glm::dvec3(tn->x - layerTn->x, tn->y - layerTn->y, tn->z - layerTn->z)) > 1.) {
      LOG(WARNING) << "node " << tn->x << " " << tn->y << " " << tn->z << " " << tn->radius;
      LOG(WARNING) << "layer node " << layerTn->x << " " << layerTn->y << " " << layerTn->z << " " << layerTn->radius;
      throw ZException("wrong layer node match");
    }
    nodeToLayer[tn] = layerTn->type;
    ++tn;
    ++layerTn;
  }
}

void ZGenerateAnalysisTextFile::getSubclassFeature(const ZSwc& tree,
                                                   const ZSwc& subclassTree,
                                                   std::map<ConstSwcTreeNode, size_t>& nodeToSubclass) const
{
  ConstSwcTreeNode tn = tree.begin();
  ConstSwcTreeNode layerTn = subclassTree.begin();
  while (tn != tree.end()) {
    if (glm::length(glm::dvec3(tn->x - layerTn->x, tn->y - layerTn->y, tn->z - layerTn->z)) > 1.) {
      LOG(WARNING) << "node " << tn->x << " " << tn->y << " " << tn->z << " " << tn->radius;
      LOG(WARNING) << "subclass node " << layerTn->x << " " << layerTn->y << " " << layerTn->z << " "
                   << layerTn->radius;
      throw ZException("wrong subclass node match");
    }
    nodeToSubclass[tn] = layerTn->type;
    ++tn;
    ++layerTn;
  }
}

void ZGenerateAnalysisTextFile::writeFeatureSwc(const ZSwc& treeIn,
                                                const std::map<ConstSwcTreeNode, double>& nodeToFeature,
                                                const QString& outSwcName) const
{
  ZSwc tree = treeIn;
  SwcTreeNode tn = tree.begin();
  ConstSwcTreeNode tnIn = treeIn.begin();
  while (tn != tree.end()) {
    tn->type = roundTo<int>(nodeToFeature.at(tnIn));
    ++tn;
    ++tnIn;
  }
  tree.resortID();
  tree.save(outSwcName);
}

double ZGenerateAnalysisTextFile::pointFrustumConeDist(double x,
                                                       double y,
                                                       double z,
                                                       const ConstSwcTreeNode& start,
                                                       const ConstSwcTreeNode& end,
                                                       double* fracOut) const
{
  double dist;
  glm::dvec3 pt(x, y, z);
  glm::dvec3 bot(start->x, start->y, start->z);
  glm::dvec3 top(end->x, end->y, end->z);
  glm::dvec3 res(m_input.voxelSizeX, m_input.voxelSizeY, m_input.voxelSizeZ);
  pt *= res;
  bot *= res;
  top *= res;
  double normtb = glm::dot(top - bot, top - bot);
  double normbp = glm::dot(bot - pt, bot - pt);
  double normtp = glm::dot(top - pt, top - pt);
  double dotbptb = glm::dot(bot - pt, top - bot);
  double frac = -dotbptb / normtb;
  if (fracOut) {
    *fracOut = frac;
  }
  if (frac < 0) {
    dist = std::sqrt(normbp) - start->radius * m_input.voxelSizeX;
  } else if (frac > 1) {
    dist = std::sqrt(normtp) - end->radius * m_input.voxelSizeX;
  } else {
    double radius = m_input.voxelSizeX * ((1 - frac) * start->radius + frac * end->radius);
    dist = std::sqrt(normbp - dotbptb * dotbptb / normtb) - radius;
  }
  return dist;
}

double ZGenerateAnalysisTextFile::pointSphereDist(double x,
                                                  double y,
                                                  double z,
                                                  const ZGenerateAnalysisTextFile::ConstSwcTreeNode& tn) const
{
  glm::dvec3 pt(x, y, z);
  glm::dvec3 center(tn->x, tn->y, tn->z);
  glm::dvec3 res(m_input.voxelSizeX, m_input.voxelSizeY, m_input.voxelSizeZ);
  return glm::distance(pt * res, center * res);
}

double ZGenerateAnalysisTextFile::punctaFrustumConeDist(const ZPunctum& punctum,
                                                        const ConstSwcTreeNode& start,
                                                        const ConstSwcTreeNode& end,
                                                        double* frac) const
{
  return pointFrustumConeDist(punctum.x(), punctum.y(), punctum.z(), start, end, frac) -
         punctum.radius() * m_input.voxelSizeX;
}

double ZGenerateAnalysisTextFile::treeNodeDist(const ConstSwcTreeNode& tn, const ConstSwcTreeNode& ptn) const
{
  glm::dvec3 bot(tn->x, tn->y, tn->z);
  glm::dvec3 top(ptn->x, ptn->y, ptn->z);
  glm::dvec3 res(m_input.voxelSizeX, m_input.voxelSizeY, m_input.voxelSizeZ);
  bot *= res;
  top *= res;
  return glm::length(bot - top);
}

bool ZGenerateAnalysisTextFile::inputSwcIsPyramidal() const
{
  ZSwc tree(m_input.swcFilename);
  for (SwcTreeNode tn = tree.begin(); tn != tree.end(); ++tn) {
    if (tn->type != ZSwc::SomaType && tn->type != ZSwc::BasalDendriteType && tn->type != ZSwc::ApicalDendriteType) {
      return false;
    }
  }
  return true;
}

void ZGenerateAnalysisTextFile::mergeSoma(ZSwc& tree,
                                          std::map<ConstSwcTreeNode, double>& nodeToBlueness,
                                          std::map<ConstSwcTreeNode, size_t>& nodeToLayer) const
{
  SwcTreeNode tn = tree.begin();
  SwcTreeNode next;
  SwcTreeNode somaNode;
  SwcTreeNode parent;
  std::map<size_t, size_t> layerCount;
  while (tn != tree.end()) {
    next = tn;
    ++next;
    if (ZSwc::isRoot(tn)) {
      somaNode = tn;
      size_t layer = nodeToLayer[somaNode];
      layerCount[layer]++;
      tn = next;
      continue;
    }
    parent = ZSwc::parent(tn);
    if (ZSwc::isRoot(parent)) {
      if (parent->type == ZSwc::SomaType && tn->type == ZSwc::SomaType) {
        size_t layer = nodeToLayer[tn];
        layerCount[layer]++;

        tree.erase(tn);
      }
    }
    tn = next;
  }

  size_t bestCount = 0;
  for (const auto& lc : layerCount) {
    if (lc.second > bestCount) {
      nodeToLayer[somaNode] = lc.first;
      bestCount = lc.second;
    }
  }

  // get blueness of soma node
  if (m_input.axonChannel >= 0) {
    ZImg axonImg(m_input.imgFilename,
                 ZImgRegion(0, -1, 0, -1, 0, -1, m_input.axonChannel, m_input.axonChannel + 1, 0, 1));
    double maxr = somaNode->radius + m_input.bluenessExtend / m_input.voxelSizeX;
    double zscale = m_input.voxelSizeZ / m_input.voxelSizeX;
    index_t left = std::floor(somaNode->x - maxr);
    index_t right = std::ceil(somaNode->x + maxr);
    index_t up = std::floor(somaNode->y - maxr);
    index_t down = std::ceil(somaNode->y + maxr);
    index_t zup = std::floor(somaNode->z - maxr / zscale);
    index_t zdown = std::ceil(somaNode->z + maxr / zscale);

    left = std::max(0_z, left);
    right = std::min(right, static_cast<index_t>(axonImg.width()) - 1);
    up = std::max(0_z, up);
    down = std::min(down, static_cast<index_t>(axonImg.height()) - 1);
    zup = std::max(0_z, zup);
    zdown = std::min(zdown, static_cast<index_t>(axonImg.depth()) - 1);

    index_t count = 0;
    double intensity = 0.0;
    glm::dvec3 center(somaNode->x, somaNode->y, somaNode->z);
    glm::dvec3 res(m_input.voxelSizeX, m_input.voxelSizeY, m_input.voxelSizeZ);
    center *= res;
    for (auto z = zup; z <= zdown; z++) {
      for (auto y = up; y <= down; y++) {
        for (auto x = left; x <= right; x++) {
          glm::dvec3 pt(x, y, z);
          pt *= res;
          if (glm::length(pt - center) <= maxr * m_input.voxelSizeX) {
            count++;
            intensity += axonImg.value<double>(x, y, z);
          }
        }
      }
    }

    if (count > 0) {
      intensity /= count;
    }
    nodeToBlueness[somaNode] = intensity;
  }
}

void ZGenerateAnalysisTextFile::removeSmallLeafBranch(ZSwc& tree, int numNodeThre, double lengthThre) const
{
  ZSwc::LeafIterator tn = tree.beginLeaf();
  while (tn != tree.endLeaf()) {
    ZSwc::LeafIterator tnnext = tn;
    ++tnnext;
    double branchLength = 0;
    int nNodes = -1; // ancestor iterator starts from input iter now
    SwcTreeNode nodeBeforeBranchNode;
    ZSwc::AncestorIterator lasttn = tn;
    for (ZSwc::AncestorIterator tmptn = tree.beginAncestor(tn); tmptn != tree.endAncestor(tn); ++tmptn) {
      nNodes++;
      glm::dvec3 bot(tmptn->x, tmptn->y, tmptn->z);
      glm::dvec3 top(lasttn->x, lasttn->y, lasttn->z);
      glm::dvec3 res(m_input.voxelSizeX, m_input.voxelSizeY, m_input.voxelSizeZ);
      bot *= res;
      top *= res;
      branchLength += glm::length(bot - top);
      if (ZSwc::isBranchNode(tmptn)) {
        nodeBeforeBranchNode = lasttn;
        break;
      }
      lasttn = tmptn;
    }
    if (nNodes < numNodeThre && branchLength < lengthThre && !ZSwc::isNull(nodeBeforeBranchNode)) {
      tree.eraseSubtree(nodeBeforeBranchNode);
    }
    tn = tnnext;
  }
}

size_t ZGenerateAnalysisTextFile::labelBranch(const ZSwc& tree,
                                              std::map<ConstSwcTreeNode, size_t>& nodeToBranchId,
                                              std::map<size_t, size_t>& branchIdToParentBranchId,
                                              std::map<ConstSwcTreeNode, double>& nodeDistToParent,
                                              std::map<ConstSwcTreeNode, double>& nodeDistToBranchStart,
                                              std::map<ConstSwcTreeNode, double>& nodeDistToSoma,
                                              std::map<ConstSwcTreeNode, int>& nodeTopologyType,
                                              std::vector<Branch>& branches) const
{
  nodeToBranchId.clear();
  branchIdToParentBranchId.clear();
  nodeDistToParent.clear();
  nodeDistToBranchStart.clear();
  nodeDistToSoma.clear();
  branches.clear();

  size_t label = 0;

  branchIdToParentBranchId[0] = 0;

  for (ConstSwcTreeNode tn = tree.begin(); tn != tree.end(); ++tn) {
    if (ZSwc::isRoot(tn)) {
      nodeToBranchId[tn] = 0;
      nodeDistToParent[tn] = 0;
      nodeDistToBranchStart[tn] = 0;
      nodeDistToSoma[tn] = 0;
      nodeTopologyType[tn] = 1;
      continue;
    } else if (ZSwc::isBranchNode(tn)) {
      nodeTopologyType[tn] = 2;
    } else if (ZSwc::isLeaf(tn)) {
      nodeTopologyType[tn] = 3;
    } else {
      nodeTopologyType[tn] = 0;
    }
    ConstSwcTreeNode parent = ZSwc::parent(tn);

    if (ZSwc::isRoot(parent) || ZSwc::isBranchNode(parent)) {
      // new branch
      nodeToBranchId[tn] = ++label;
      branchIdToParentBranchId[label] = nodeToBranchId[parent];
      double distToParent = treeNodeDist(tn, parent);
      nodeDistToParent[tn] = distToParent;
      nodeDistToBranchStart[tn] = distToParent;
      nodeDistToSoma[tn] = nodeDistToSoma[parent] + distToParent;
    } else {
      nodeToBranchId[tn] = nodeToBranchId[parent];
      double distToParent = treeNodeDist(tn, parent);
      nodeDistToParent[tn] = distToParent;
      nodeDistToBranchStart[tn] = nodeDistToBranchStart[parent] + distToParent;
      nodeDistToSoma[tn] = nodeDistToSoma[parent] + distToParent;
    }
  }

  branches.resize(label);
  for (ConstSwcTreeNode tn = tree.begin(); tn != tree.end(); ++tn) {
    size_t branchId = nodeToBranchId[tn];
    if (branchId > 0) {
      Branch& branch = branches[branchId - 1];
      branch.id = branchId;
      ConstSwcTreeNode parent = ZSwc::parent(tn);
      if (nodeToBranchId[parent] != branch.id) { // duplicate parent
        branch.nodes.push_back(parent);
      }
      branch.nodes.push_back(tn);
      branch.length = std::max(branch.length, nodeDistToBranchStart[tn]);
    }
  }

  return label;
}

ZGenerateAnalysisTextFile::ConstSwcTreeNode
ZGenerateAnalysisTextFile::getNodeSegOfPunctum(const ZSwc& tree,
                                               const ZPunctum& punctum,
                                               size_t numBranches,
                                               const std::map<ConstSwcTreeNode, size_t>& nodeToBranchId) const
{
  std::vector<double> distToBranch(numBranches, std::numeric_limits<double>::max());
  std::vector<ConstSwcTreeNode> nearestNodeOfBranch(numBranches);
  for (ConstSwcTreeNode tn = tree.begin(); tn != tree.end(); ++tn) {
    size_t branchId = nodeToBranchId.at(tn);
    if (branchId > 0) {
      double dist = punctaFrustumConeDist(punctum, ZSwc::parent(tn), tn);
      if (dist < distToBranch[branchId - 1]) {
        distToBranch[branchId - 1] = dist;
        nearestNodeOfBranch[branchId - 1] = tn;
      }
    }
  }
  double distToTree = distToBranch[0];
  ConstSwcTreeNode res = nearestNodeOfBranch[0];
  std::vector<ConstSwcTreeNode> nodesWithinRange;
  for (size_t i = 0; i < numBranches; ++i) {
    if (i > 0 && distToBranch[i] < distToTree) {
      distToTree = distToBranch[i];
      res = nearestNodeOfBranch[i];
    }
    if (distToBranch[i] <= m_input.maxDistToBranch) {
      nodesWithinRange.push_back(nearestNodeOfBranch[i]);
    }
  }

  if (distToTree - punctum.radius() * m_input.voxelSizeX <= 0.0) { // puncta inside branch, no more check
    return res;
  } else if (nodesWithinRange.empty()) { // zero branch in range
    // throw ZException("Need check");
    LOG(WARNING) << "Check Punctum from (no branch) " << m_input.punctaFilename << " : " << punctum.x() << " "
                 << punctum.y() << " " << punctum.z() << " " << punctum.radius() << " " << punctum.maxIntensity() << " "
                 << punctum.meanIntensity();
    return ConstSwcTreeNode();
  } else if (nodesWithinRange.size() == 1) {
    return res;
  }

  // more than one neighbour branches
  res = intensityWeightedNearestNode(punctum.x(), punctum.y(), punctum.z(), nodesWithinRange);

  //  if (nearestNode(punctum.x(), punctum.y(), punctum.z(), nodesWithinRange) != res) {
  //    LOG(WARNING) << "Punctum assign example from " << m_input.punctaFilename << " : "
  //            << punctum.x() << " " << punctum.y() << " " << punctum.z() << " " << punctum.radius() << " "
  //            << punctum.maxIntensity() << " " << punctum.meanIntensity();
  //  }
  return res;
}

ZGenerateAnalysisTextFile::ConstSwcTreeNode
ZGenerateAnalysisTextFile::intensityWeightedNearestNode(double x,
                                                        double y,
                                                        double z,
                                                        const std::vector<ConstSwcTreeNode>& nodes) const
{
  // first crop out the region
  auto left = roundTo<index_t>(x);
  auto right = roundTo<index_t>(x);
  auto up = roundTo<index_t>(y);
  auto down = roundTo<index_t>(y);
  auto zup = roundTo<index_t>(z);
  auto zdown = roundTo<index_t>(z);
  for (auto node : nodes) {
    ConstSwcTreeNode parent = ZSwc::parent(node);
    left = std::min(std::min(left, roundTo<index_t>(node->x)), roundTo<index_t>(parent->x));
    right = std::max(std::max(right, roundTo<index_t>(node->x)), roundTo<index_t>(parent->x));
    up = std::min(std::min(up, roundTo<index_t>(node->y)), roundTo<index_t>(parent->y));
    down = std::max(std::max(down, roundTo<index_t>(node->y)), roundTo<index_t>(parent->y));
    zup = std::min(std::min(zup, roundTo<index_t>(node->z)), roundTo<index_t>(parent->z));
    zdown = std::max(std::max(zdown, roundTo<index_t>(node->z)), roundTo<index_t>(parent->z));
  }
  ZImgInfo imgInfo = ZImg::readImgInfos(m_input.imgFilename).at(0);
  left = std::max(0_z, left);
  right = std::min(right, static_cast<index_t>(imgInfo.width) - 1);
  up = std::max(0_z, up);
  down = std::min(down, static_cast<index_t>(imgInfo.height) - 1);
  zup = std::max(0_z, zup);
  zdown = std::min(zdown, static_cast<index_t>(imgInfo.depth) - 1);
  ZImgRegion
    rgn(left, right + 1, up, down + 1, zup, zdown + 1, m_input.dendriteChannel, m_input.dendriteChannel + 1, 0, 1);
  ZImg img(m_input.imgFilename, rgn);
  img.infoRef().voxelSizeUnit = VoxelSizeUnit::um;
  img.infoRef().voxelSizeX = m_input.voxelSizeX;
  img.infoRef().voxelSizeY = m_input.voxelSizeY;
  img.infoRef().voxelSizeZ = m_input.voxelSizeZ;

  ZImgGraph imgGraph(img);
  imgGraph.setConnectivity(26);
  ZImgAutoThreshold imgAutoThre;
  double cent1 = 0;
  double cent2 = 0;
  auto thre1 = imgAutoThre.centroidThre<double>(cent1, cent2, img);
  double scale = cent2 - cent1;
  if (scale < 1.0) {
    scale = 1.0;
  }
  scale /= 9.2;
  imgGraph.build(ZImgGraph::EdgeWeight2(thre1, scale));

  ZVoxelCoordinate startCoord(roundTo<int>(x) - left, roundTo<int>(y) - up, roundTo<int>(z) - zup);
  std::vector<double> dist = imgGraph.shortestPaths(startCoord);

  std::vector<double> nodeMinDists(nodes.size(), std::numeric_limits<double>::max());
  for (size_t v = 0; v < dist.size(); ++v) {
    ZVoxelCoordinate coord = img.indexToCoord(v);
    index_t nodeIdx = -1;
    for (size_t i = nodes.size(); i-- > 0;) {
      if (pointFrustumConeDist(coord.x + left, coord.y + up, coord.z + zup, nodes[i], ZSwc::parent(nodes[i])) <= 0.0) {
        nodeIdx = i;
        break;
      }
    }
    if (nodeIdx > -1) {
      nodeMinDists[nodeIdx] = std::min(nodeMinDists[nodeIdx], dist[v]);
    }
  }

  size_t minIndex = std::ranges::min_element(nodeMinDists) - nodeMinDists.begin();
  // VLOG(1) << " min dist " << nodeMinDists[minIndex];
  // nodeMinDists[minIndex] = std::numeric_limits<double>::max();
  // VLOG(1) << " second min dist " << *std::ranges::min_element(nodeMinDists);

  return nodes[minIndex];
}

ZGenerateAnalysisTextFile::ConstSwcTreeNode
ZGenerateAnalysisTextFile::nearestNode(double x, double y, double z, const std::vector<ConstSwcTreeNode>& nodes) const
{
  double dist = std::numeric_limits<double>::max();
  ConstSwcTreeNode res;
  for (auto node : nodes) {
    double nodeDist = pointFrustumConeDist(x, y, z, node, ZSwc::parent(node));
    if (nodeDist < dist) {
      dist = nodeDist;
      res = node;
    }
  }
  CHECK(!ZSwc::isNull(res));
  return res;
}

QDir ZGenerateAnalysisTextFile::getSubDir(const QString& subFoldername) const
{
  QDir outputDir(m_input.outputFolder);
  QFileInfo folderInfo(outputDir.filePath(subFoldername));
  if (!folderInfo.exists() || !folderInfo.isDir()) {
    QFile::remove(folderInfo.absoluteFilePath());
    outputDir.mkpath(subFoldername);
  }
  QDir res = QDir(folderInfo.absoluteFilePath());
  if (!res.exists()) {
    throw ZException(fmt::format("Can not create {} for writing", folderInfo.absoluteFilePath()));
  }
  return res;
}

void ZGenerateAnalysisTextFile::generateAnalysisFiles(const ZSwc& tree,
                                                      const std::map<ConstSwcTreeNode, double>& nodeToBlueness,
                                                      const std::map<ConstSwcTreeNode, size_t>& nodeToLayer,
                                                      const std::map<ConstSwcTreeNode, size_t>& nodeToSubclass) const
{
  QDir outputDir(m_input.outputFolder);
  QFileInfo swcFileInfo(m_processedSwcFilename);
  QFileInfo punctaFileInfo(m_input.punctaFilename);
  QFileInfo somaPunctaFileInfo(m_input.somaPunctaFilename);
  QString branchFilename = outputDir.filePath(swcFileInfo.fileName()) + "_branch.txt";
  QString punctaFilename = outputDir.filePath(punctaFileInfo.fileName()) + "_puncta.txt";
  QString somaPunctaFilename = outputDir.filePath(somaPunctaFileInfo.fileName()) + "_puncta.txt";

  ZPuncta punctaList(m_input.somaPunctaFilename);
  std::ofstream somaPunctaStream = openOFStream(somaPunctaFilename);
  somaPunctaStream << "# puncta id, x, y, z, volsize, meanIntensity, maxIntensity\n";

  size_t idx = 0;
  for (const auto& p : punctaList.data) {
    somaPunctaStream << idx++ << " " << p.x() << " " << p.y() << " " << p.z() << " " << p.volSize() << " "
                     << p.meanIntensity() << " " << p.maxIntensity() << "\n";
  }
  somaPunctaStream.close();

  std::map<ConstSwcTreeNode, size_t> nodeToBranchId;
  std::map<size_t, size_t> branchIdToParentBranchId;
  std::map<ConstSwcTreeNode, double> nodeDistToParent;
  std::map<ConstSwcTreeNode, double> nodeDistToBranchStart;
  std::map<ConstSwcTreeNode, double> nodeDistToSoma;
  std::map<ConstSwcTreeNode, int> nodeTopologyType;
  std::map<ConstSwcTreeNode, std::vector<const ZPunctum*>> nodeToPuncta;
  std::vector<Branch> branches;
  labelBranch(tree,
              nodeToBranchId,
              branchIdToParentBranchId,
              nodeDistToParent,
              nodeDistToBranchStart,
              nodeDistToSoma,
              nodeTopologyType,
              branches);
  punctaList = ZPuncta(m_input.punctaFilename);
  std::map<const ZPunctum*, ConstSwcTreeNode> punctumToNode;
  std::map<const ZPunctum*, double> punctumDistToBranchStart;
  std::map<const ZPunctum*, double> punctumDistToSoma;
  std::map<const ZPunctum*, double> punctumDistToSegmentStart;
  for (const auto& p : punctaList.data) {
    ConstSwcTreeNode tn = getNodeSegOfPunctum(tree, p, branches.size(), nodeToBranchId);
    punctumToNode[&p] = tn;
    if (ZSwc::isNull(tn)) {
      continue;
    }

    nodeToPuncta[tn].push_back(&p);

    size_t branchId = nodeToBranchId[tn];
    bool firstSeg = branchId != nodeToBranchId[ZSwc::parent(tn)];

    double frac;
    punctaFrustumConeDist(p, ZSwc::parent(tn), tn, &frac);
    if (frac < 0) {
      punctumDistToBranchStart[&p] = firstSeg ? 0 : nodeDistToBranchStart[ZSwc::parent(tn)];
      punctumDistToSoma[&p] = nodeDistToSoma[ZSwc::parent(tn)];
      punctumDistToSegmentStart[&p] = 0;
    } else if (frac > 1) {
      punctumDistToBranchStart[&p] = nodeDistToBranchStart[tn];
      punctumDistToSoma[&p] = nodeDistToSoma[tn];
      punctumDistToSegmentStart[&p] = nodeDistToParent[tn];
    } else {
      punctumDistToBranchStart[&p] =
        (firstSeg ? 0 : nodeDistToBranchStart[ZSwc::parent(tn)]) + frac * nodeDistToParent[tn];
      punctumDistToSoma[&p] = nodeDistToSoma[ZSwc::parent(tn)] + frac * nodeDistToParent[tn];
      punctumDistToSegmentStart[&p] = frac * nodeDistToParent[tn];
    }
  }

  std::ofstream branchStream = openOFStream(branchFilename);
  branchStream << "# branch id, type, x, y, z, radius, blueness, layer, topological type, "
                  "distToBranchStart, distToSoma\n";

  for (auto& branch : branches) {
    for (size_t j = 0; j < branch.nodes.size(); ++j) {
      const ConstSwcTreeNode& tn = branch.nodes[j];
      branchStream << branch.id << " " << tn->type << " " << tn->x << " " << tn->y << " " << tn->z << " " << tn->radius
                   << " " << nodeToBlueness.at(tn) << " " << nodeToLayer.at(tn) << " " << nodeTopologyType.at(tn) << " "
                   << nodeDistToBranchStart.at(tn) << " " << nodeDistToSoma.at(tn) << "\n";
    }
  }
  branchStream.close();

  std::ofstream punctaStream = openOFStream(punctaFilename);
  punctaStream << "# puncta id, x, y, z, branch id, offset, volsize, meanIntensity, maxIntensity, "
                  "distToBranchStart, distToSoma\n";

  idx = 0;
  for (const auto& p : punctaList.data) {
    if (ZSwc::isNull(punctumToNode[&p])) {
      continue;
    }
    size_t branchId = nodeToBranchId[punctumToNode[&p]];
    double offset = punctumDistToBranchStart[&p] / branches[branchId - 1].length;
    punctaStream << idx++ << " " << p.x() << " " << p.y() << " " << p.z() << " " << branchId << " " << offset << " "
                 << p.volSize() << " " << p.meanIntensity() << " " << p.maxIntensity() << " "
                 << punctumDistToBranchStart[&p] << " " << punctumDistToSoma[&p] << "\n";
  }
  punctaStream.close();

  // separated files

  // techinical branch
  QDir technicalBranchFolder = getSubDir("Technical_Branches");

  for (auto& branch : branches) {
    QString outSwcName = technicalBranchFolder.filePath(QString("branch_%1.swc").arg(branch.id, 4, 10, QChar('0')));
    QString outSwcTxtName = technicalBranchFolder.filePath(QString("branch_%1.txt").arg(branch.id, 4, 10, QChar('0')));
    QString outPunctaName = technicalBranchFolder.filePath(
      QString("branch_%1_%2").arg(branch.id, 4, 10, QChar('0')).arg(QFileInfo(m_input.punctaFilename).fileName()));
    QString outPunctaTxtName = technicalBranchFolder.filePath(
      QString("branch_%1_%2.txt").arg(branch.id, 4, 10, QChar('0')).arg(QFileInfo(m_input.punctaFilename).fileName()));

    std::ofstream outSwcStream = openOFStream(outSwcName);

    std::ofstream outSwcTxtStream = openOFStream(outSwcTxtName);
    outSwcTxtStream << "# Branch Part, Branch Type, x, y, z, radius, blueness, layer, topological type, "
                       "distToBranchStart, distToSoma\n";

    std::ofstream outPunctaTxtStream = openOFStream(outPunctaTxtName);
    outPunctaTxtStream << "# Branch location, x, y, z, offset, volsize, meanIntensity, maxIntensity, "
                          "distToBranchStart, distToSoma\n";

    ZPuncta tmpPunc;
    for (size_t j = 0; j < branch.nodes.size(); ++j) {
      const ConstSwcTreeNode& tn = branch.nodes[j];
      outSwcStream << tn->id << " " << tn->type << " " << tn->x << " " << tn->y << " " << tn->z << " " << tn->radius
                   << " " << ZSwc::parentID(tn) << "\n";
      if (j == 0) {
        outSwcTxtStream << 0 << " ";
      } else if (j == branch.nodes.size() - 1) {
        outSwcTxtStream << 2 << " ";
      } else {
        outSwcTxtStream << 1 << " ";
      }
      outSwcTxtStream << tn->type << " " << tn->x << " " << tn->y << " " << tn->z << " " << tn->radius << " "
                      << nodeToBlueness.at(tn) << " " << nodeToLayer.at(tn) << " " << nodeTopologyType.at(tn) << " "
                      << nodeDistToBranchStart.at(tn) << " " << nodeDistToSoma.at(tn) << "\n";

      if (j > 0) {
        std::vector<const ZPunctum*> puncta = nodeToPuncta[tn];
        for (auto punctum : puncta) {
          tmpPunc.data.push_back(*punctum);
          outPunctaTxtStream << j << " " << punctum->x() << " " << punctum->y() << " " << punctum->z() << " "
                             << (punctumDistToBranchStart[punctum] / branch.length) << " " << punctum->volSize() << " "
                             << punctum->meanIntensity() << " " << punctum->maxIntensity() << " "
                             << punctumDistToBranchStart[punctum] << " " << punctumDistToSoma[punctum] << "\n";
        }
      }
    }
    tmpPunc.save(outPunctaName);
    tmpPunc.clear();
  }

  // FC branches
  if (m_input.doPyramidalFunctionalSeparation) {
    QDir FCBasalBranchFolder = getSubDir("FC_Branches/Basal");
    QDir FCApicalBranchFolder = getSubDir("FC_Branches/Apical");

    int basalIndex = 0;
    int apicalIndex = 0;
    for (ZSwc::ConstLeafIterator tn = tree.beginLeaf(); tn != tree.endLeaf(); ++tn) {
      ConstSwcTreeNode tmptn = tn;

      QDir* currentBranchFolder = nullptr;
      int* currentBranchIndex = nullptr;
      if (tmptn->type == ZSwc::ApicalDendriteType) {
        currentBranchFolder = &FCApicalBranchFolder;
        currentBranchIndex = &apicalIndex;
      } else if (tmptn->type == ZSwc::BasalDendriteType) {
        currentBranchFolder = &FCBasalBranchFolder;
        currentBranchIndex = &basalIndex;
      }

      QString outSwcName =
        currentBranchFolder->filePath(QString("branch_%1.swc").arg(*currentBranchIndex, 4, 10, QChar('0')));
      QString outSwcTxtName =
        currentBranchFolder->filePath(QString("branch_%1.txt").arg(*currentBranchIndex, 4, 10, QChar('0')));
      QString outPunctaName = currentBranchFolder->filePath(QString("branch_%1_%2")
                                                              .arg(*currentBranchIndex, 4, 10, QChar('0'))
                                                              .arg(QFileInfo(m_input.punctaFilename).fileName()));
      QString outPunctaTxtName = currentBranchFolder->filePath(QString("branch_%1_%2.txt")
                                                                 .arg(*currentBranchIndex, 4, 10, QChar('0'))
                                                                 .arg(QFileInfo(m_input.punctaFilename).fileName()));
      (*currentBranchIndex) += 1;

      double length = nodeDistToSoma[tmptn];

      std::vector<size_t> branchIdList;
      branchIdList.push_back(nodeToBranchId[tmptn]);
      while (branchIdToParentBranchId[branchIdList[0]] != 0) {
        size_t parentBranchId = branchIdToParentBranchId[branchIdList[0]];
        branchIdList.insert(branchIdList.begin(), parentBranchId);
      }

      std::ofstream outSwcStream = openOFStream(outSwcName);

      std::ofstream outSwcTxtStream = openOFStream(outSwcTxtName);
      outSwcTxtStream << "# Branch Part, Branch Type, x, y, z, radius, blueness, layer, topological type, "
                         "distToBranchStart, distToSoma\n";

      std::ofstream outPunctaTxtStream = openOFStream(outPunctaTxtName);
      outPunctaTxtStream << "# Branch location, x, y, z, offset, volsize, meanIntensity, maxIntensity, "
                            "distToBranchStart, distToSoma\n";

      int nodeIdx = -1;
      ZPuncta tmpPunc;
      for (size_t i = 0; i < branchIdList.size(); ++i) {
        size_t branchId = branchIdList[i];
        Branch& branch = branches[branchId - 1];
        for (size_t j = 0; j < branch.nodes.size(); ++j) {
          if (i > 0 && j == 0) { // skip first one since it is already processed
            continue;
          }
          ++nodeIdx;

          const ConstSwcTreeNode& btn = branch.nodes[j];
          outSwcStream << btn->id << " " << btn->type << " " << btn->x << " " << btn->y << " " << btn->z << " "
                       << btn->radius << " " << ZSwc::parentID(btn) << "\n";
          if (i == 0 && j == 0) {
            outSwcTxtStream << 0 << " ";
          } else if (i == branchIdList.size() - 1 && j == branch.nodes.size() - 1) {
            outSwcTxtStream << 2 << " ";
          } else {
            outSwcTxtStream << 1 << " ";
          }
          outSwcTxtStream << btn->type << " " << btn->x << " " << btn->y << " " << btn->z << " " << btn->radius << " "
                          << nodeToBlueness.at(btn) << " " << nodeToLayer.at(btn) << " " << nodeTopologyType.at(btn)
                          << " " << nodeDistToBranchStart.at(btn) << " " << nodeDistToSoma.at(btn) << "\n";

          // don't need to skip i == 0 and j == 0 case because btn will be soma and there
          // should be no puncta for soma
          std::vector<const ZPunctum*> puncta = nodeToPuncta[btn];
          for (auto punctum : puncta) {
            tmpPunc.data.push_back(*punctum);
            outPunctaTxtStream << nodeIdx << " " << punctum->x() << " " << punctum->y() << " " << punctum->z() << " "
                               << (punctumDistToSoma[punctum] / length) << " " << punctum->volSize() << " "
                               << punctum->meanIntensity() << " " << punctum->maxIntensity() << " "
                               << punctumDistToSoma[punctum] << " " << punctumDistToSoma[punctum] << "\n";
          }
        }
      }
      tmpPunc.save(outPunctaName);
      tmpPunc.clear();
    }
  }

  // Subclass Branches
  if (m_input.doPyramidalSubclassSeparation) {
    QDir basalIntermediateBranchFolder = getSubDir("Subclass_Branches/BasalIntermediate");
    QDir basalTerminalBranchFolder = getSubDir("Subclass_Branches/BasalTerminal");
    QDir apicalObliqueIntermediateBranchFolder = getSubDir("Subclass_Branches/ApicalObliqueIntermediate");
    QDir apicalObliqueTerminalBranchFolder = getSubDir("Subclass_Branches/ApicalObliqueTerminal");
    QDir mainTrunkBranchFolder = getSubDir("Subclass_Branches/MainTrunk");
    QDir apicalTuftBranchFolder = getSubDir("Subclass_Branches/ApicalTuft");

    int basalIntermediateIndex = 0;
    int basalTerminalIndex = 0;
    int apicalObliqueIntermediateIndex = 0;
    int apicalObliqueTerminalIndex = 0;
    int mainTrunkIndex = 0;
    int apicalTuftIndex = 0;

    // get main trunk and tuft (MTT)
    std::map<ConstSwcTreeNode, size_t> nodeToMTTBranchId;
    std::vector<Branch> MTTBranches;
    size_t label = 0;
    for (ConstSwcTreeNode tn = tree.begin(); tn != tree.end(); ++tn) {
      size_t nodeSubclass = nodeToSubclass.at(tn);
      if (ZSwc::isRoot(tn) || (nodeSubclass != ZSwc::MainTrunkType && nodeSubclass != ZSwc::ApicalTuftType)) {
        nodeToMTTBranchId[tn] = 0;
        continue;
      }
      ConstSwcTreeNode parent = ZSwc::parent(tn);
      size_t parentNodeSubclass = nodeToSubclass.at(parent);
      bool parentIsMTTBranchNode = false;
      if (nodeToMTTBranchId[parent] != 0 && ZSwc::isBranchNode(parent)) {
        int count = 0;
        for (auto ctn = tree.beginChild(parent); ctn != tree.endChild(parent); ++ctn) {
          if (nodeToSubclass.at(ctn) == ZSwc::MainTrunkType || nodeToSubclass.at(ctn) == ZSwc::ApicalTuftType) {
            ++count;
          }
        }
        parentIsMTTBranchNode = count > 1;
      }

      if (nodeToMTTBranchId[parent] == 0 || parentIsMTTBranchNode || parentNodeSubclass != nodeSubclass) {
        // new branch
        nodeToMTTBranchId[tn] = ++label;
      } else {
        nodeToMTTBranchId[tn] = nodeToMTTBranchId[parent];
      }
    }

    MTTBranches.resize(label);
    for (ConstSwcTreeNode tn = tree.begin(); tn != tree.end(); ++tn) {
      size_t branchId = nodeToMTTBranchId[tn];
      if (branchId > 0) {
        Branch& branch = MTTBranches[branchId - 1];
        branch.id = branchId;
        ConstSwcTreeNode parent = ZSwc::parent(tn);
        if (nodeToMTTBranchId[parent] != branch.id) { // duplicate parent
          branch.nodes.push_back(parent);
        }
        branch.nodes.push_back(tn);
        branch.length += nodeDistToParent[tn];
      }
    }
    for (auto& branch : MTTBranches) {
      ConstSwcTreeNode tmptn = branch.nodes.back();
      size_t nodeSubclass = nodeToSubclass.at(tmptn);

      QDir* currentBranchFolder = nullptr;
      int* currentBranchIndex = nullptr;

      if (nodeSubclass == ZSwc::MainTrunkType) {
        currentBranchFolder = &mainTrunkBranchFolder;
        currentBranchIndex = &mainTrunkIndex;
      } else if (nodeSubclass == ZSwc::ApicalTuftType) {
        currentBranchFolder = &apicalTuftBranchFolder;
        currentBranchIndex = &apicalTuftIndex;
      } else {
        CHECK(false);
      }

      QString outSwcName =
        currentBranchFolder->filePath(QString("branch_%1.swc").arg(*currentBranchIndex, 4, 10, QChar('0')));
      QString outSwcTxtName =
        currentBranchFolder->filePath(QString("branch_%1.txt").arg(*currentBranchIndex, 4, 10, QChar('0')));
      QString outPunctaName = currentBranchFolder->filePath(QString("branch_%1_%2")
                                                              .arg(*currentBranchIndex, 4, 10, QChar('0'))
                                                              .arg(QFileInfo(m_input.punctaFilename).fileName()));
      QString outPunctaTxtName = currentBranchFolder->filePath(QString("branch_%1_%2.txt")
                                                                 .arg(*currentBranchIndex, 4, 10, QChar('0'))
                                                                 .arg(QFileInfo(m_input.punctaFilename).fileName()));
      (*currentBranchIndex) += 1;

      std::ofstream outSwcStream = openOFStream(outSwcName);

      std::ofstream outSwcTxtStream = openOFStream(outSwcTxtName);
      outSwcTxtStream << "# Branch Part, Branch Type, x, y, z, radius, blueness, layer, topological type, "
                         "distToBranchStart, distToSoma\n";

      std::ofstream outPunctaTxtStream = openOFStream(outPunctaTxtName);
      outPunctaTxtStream << "# Branch location, x, y, z, offset, volsize, meanIntensity, maxIntensity, "
                            "distToBranchStart, distToSoma\n";

      double segmentStartToBranchStartLength = 0.0;
      ZPuncta tmpPunc;
      for (size_t j = 0; j < branch.nodes.size(); ++j) {
        const ConstSwcTreeNode& tn = branch.nodes[j];
        outSwcStream << tn->id << " " << tn->type << " " << tn->x << " " << tn->y << " " << tn->z << " " << tn->radius
                     << " " << ZSwc::parentID(tn) << "\n";
        if (j == 0) {
          outSwcTxtStream << 0 << " ";
        } else if (j == branch.nodes.size() - 1) {
          outSwcTxtStream << 2 << " ";
        } else {
          outSwcTxtStream << 1 << " ";
        }
        outSwcTxtStream << tn->type << " " << tn->x << " " << tn->y << " " << tn->z << " " << tn->radius << " "
                        << nodeToBlueness.at(tn) << " " << nodeToLayer.at(tn) << " " << nodeTopologyType.at(tn) << " "
                        << nodeDistToBranchStart.at(tn) << " " << nodeDistToSoma.at(tn) << "\n";

        if (j > 0) {
          std::vector<const ZPunctum*> puncta = nodeToPuncta[tn];
          for (auto punctum : puncta) {
            tmpPunc.data.push_back(*punctum);
            double punctumDistToMTTBranchStart = segmentStartToBranchStartLength + punctumDistToSegmentStart[punctum];
            outPunctaTxtStream << j << " " << punctum->x() << " " << punctum->y() << " " << punctum->z() << " "
                               << (punctumDistToMTTBranchStart / branch.length) << " " << punctum->volSize() << " "
                               << punctum->meanIntensity() << " " << punctum->maxIntensity() << " "
                               << punctumDistToMTTBranchStart << " " << punctumDistToSoma[punctum] << "\n";
          }

          segmentStartToBranchStartLength += nodeDistToParent[tn];
        }
      }
      tmpPunc.save(outPunctaName);
      tmpPunc.clear();
    }

    // other not main trunk and not tuft branches
    for (auto& branch : branches) {
      ConstSwcTreeNode tmptn = branch.nodes.back();
      size_t nodeSubclass = nodeToSubclass.at(tmptn);
      size_t nodeType = tmptn->type;
      bool nodeIsLeaf = ZSwc::isLeaf(tmptn);

      QDir* currentBranchFolder = nullptr;
      int* currentBranchIndex = nullptr;

      if (nodeSubclass != ZSwc::MainTrunkType && nodeSubclass != ZSwc::ApicalTuftType) {
        if (nodeType == ZSwc::ApicalDendriteType) {
          currentBranchFolder =
            nodeIsLeaf ? &apicalObliqueTerminalBranchFolder : &apicalObliqueIntermediateBranchFolder;
          currentBranchIndex = nodeIsLeaf ? &apicalObliqueTerminalIndex : &apicalObliqueIntermediateIndex;
        } else if (nodeType == ZSwc::BasalDendriteType) {
          currentBranchFolder = nodeIsLeaf ? &basalTerminalBranchFolder : &basalIntermediateBranchFolder;
          currentBranchIndex = nodeIsLeaf ? &basalTerminalIndex : &basalIntermediateIndex;
        } else {
          CHECK(false);
        }

        QString outSwcName =
          currentBranchFolder->filePath(QString("branch_%1.swc").arg(*currentBranchIndex, 4, 10, QChar('0')));
        QString outSwcTxtName =
          currentBranchFolder->filePath(QString("branch_%1.txt").arg(*currentBranchIndex, 4, 10, QChar('0')));
        QString outPunctaName = currentBranchFolder->filePath(QString("branch_%1_%2")
                                                                .arg(*currentBranchIndex, 4, 10, QChar('0'))
                                                                .arg(QFileInfo(m_input.punctaFilename).fileName()));
        QString outPunctaTxtName = currentBranchFolder->filePath(QString("branch_%1_%2.txt")
                                                                   .arg(*currentBranchIndex, 4, 10, QChar('0'))
                                                                   .arg(QFileInfo(m_input.punctaFilename).fileName()));
        (*currentBranchIndex) += 1;

        std::ofstream outSwcStream = openOFStream(outSwcName);

        std::ofstream outSwcTxtStream = openOFStream(outSwcTxtName);
        outSwcTxtStream << "# Branch Part, Branch Type, x, y, z, radius, blueness, layer, topological type, "
                           "distToBranchStart, distToSoma\n";

        std::ofstream outPunctaTxtStream = openOFStream(outPunctaTxtName);
        outPunctaTxtStream << "# Branch location, x, y, z, offset, volsize, meanIntensity, maxIntensity, "
                              "distToBranchStart, distToSoma\n";

        ZPuncta tmpPunc;
        for (size_t j = 0; j < branch.nodes.size(); ++j) {
          const ConstSwcTreeNode& tn = branch.nodes[j];
          outSwcStream << tn->id << " " << tn->type << " " << tn->x << " " << tn->y << " " << tn->z << " " << tn->radius
                       << " " << ZSwc::parentID(tn) << "\n";
          if (j == 0) {
            outSwcTxtStream << 0 << " ";
          } else if (j == branch.nodes.size() - 1) {
            outSwcTxtStream << 2 << " ";
          } else {
            outSwcTxtStream << 1 << " ";
          }
          outSwcTxtStream << tn->type << " " << tn->x << " " << tn->y << " " << tn->z << " " << tn->radius << " "
                          << nodeToBlueness.at(tn) << " " << nodeToLayer.at(tn) << " " << nodeTopologyType.at(tn) << " "
                          << nodeDistToBranchStart.at(tn) << " " << nodeDistToSoma.at(tn) << "\n";

          if (j > 0) {
            std::vector<const ZPunctum*> puncta = nodeToPuncta[tn];
            for (auto punctum : puncta) {
              tmpPunc.data.push_back(*punctum);
              outPunctaTxtStream << j << " " << punctum->x() << " " << punctum->y() << " " << punctum->z() << " "
                                 << (punctumDistToBranchStart[punctum] / branch.length) << " " << punctum->volSize()
                                 << " " << punctum->meanIntensity() << " " << punctum->maxIntensity() << " "
                                 << punctumDistToBranchStart[punctum] << " " << punctumDistToSoma[punctum] << "\n";
            }
          }
        }
        tmpPunc.save(outPunctaName);
        tmpPunc.clear();
      }
    }
  }
}

} // namespace nim
