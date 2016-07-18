#include "zgenerateanalysistextfile.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include "zimg.h"
#include "zimggraph.h"
#include "zimgautothreshold.h"
#include "zglmutils.h"
#include "zioutils.h"
#include "include/reader.h"

namespace nim {

ZGenerateAnalysisTextFile::ZGenerateAnalysisTextFile()
{
}

void ZGenerateAnalysisTextFile::generate(const ZAnalysisTextFileInput input)
{
  m_input = input;
  generate();
}

void ZGenerateAnalysisTextFile::generate(const QString& imgFilename, const QString& swcFilename, const QString& punctaFilename)
{
  m_input.imgFilename = imgFilename;
  m_input.swcFilename = swcFilename;
  m_input.punctaFilename = punctaFilename;
  generate();
}

void ZGenerateAnalysisTextFile::generate(const QString &worklistFile)
{
  checkFileExist(worklistFile);

  QStringList header;
  header << "# imageName" << "swcName" << "punctaName" << "voxelSizeXInUm" << "voxelSizeYInUm"
         << "voxelSizeZInUm" << "dendriteChannel" << "axonChannel(can be empty)"
         << "maxDistToBranch"  << "bluenessExtend" << "outputFolder(can be empty)"
         << "doPyramidalFunctionalSeparation(yes or no)" << "doPyramidalSubclassSeparation(yes or no)"
         << "somaPunctaName";

  QList<QStringList> allLines = QtCSV::Reader::readToList(worklistFile);
  if (allLines.empty()) {
    throw ZImgException(QString("Can not parse file (%1) or file is empty").arg(worklistFile));
  }

  for (const auto& list: allLines) {
    if (list.empty() || list.at(0).startsWith("#")) {
      continue;
    }
    if (list.size() != header.size()) {
      throw ZImgException(QString("Wrong number of items in line (%1), expected format: <%2>").arg(list.join(',')).arg(header.join(',')));
    }
    bool ok = false;
    m_input.imgFilename = list[0];
    m_input.swcFilename = list[1];
    m_input.punctaFilename = list[2];
    if (!list[3].isEmpty()) {
      m_input.voxelSizeX = list[3].toDouble(&ok);
      if (!ok) throw ZImgException(QString("Can not parse line (%1) with format <%2>").arg(list.join(',')).arg(header.join(',')));
    }
    if (!list[4].isEmpty()) {
      m_input.voxelSizeY = list[4].toDouble(&ok);
      if (!ok) throw ZImgException(QString("Can not parse line (%1) with format <%2>").arg(list.join(',')).arg(header.join(',')));
    }
    if (!list[5].isEmpty()) {
      m_input.voxelSizeZ = list[5].toDouble(&ok);
      if (!ok) throw ZImgException(QString("Can not parse line (%1) with format <%2>").arg(list.join(',')).arg(header.join(',')));
    }
    m_input.dendriteChannel = list[6].toInt(&ok);
    if (!ok) throw ZImgException(QString("Can not parse line (%1) with format <%2>").arg(list.join(',')).arg(header.join(',')));
    if (!list[7].isEmpty()) {
      m_input.axonChannel = list[7].toInt(&ok);
      if (!ok) throw ZImgException(QString("Can not parse line (%1) with format <%2>").arg(list.join(',')).arg(header.join(',')));
    }
    m_input.maxDistToBranch = list[8].toDouble(&ok);
    if (!ok) throw ZImgException(QString("Can not parse line (%1) with format <%2>").arg(list.join(',')).arg(header.join(',')));
    m_input.bluenessExtend = list[9].toDouble(&ok);
    if (!ok) throw ZImgException(QString("Can not parse line (%1) with format <%2>").arg(list.join(',')).arg(header.join(',')));
    m_input.outputFolder = list[10];
    if (list[11].compare("yes", Qt::CaseInsensitive) == 0) {
      m_input.doPyramidalFunctionalSeparation = true;
    } else if (list[11].compare("no", Qt::CaseInsensitive) == 0) {
      m_input.doPyramidalFunctionalSeparation = false;
    } else {
      throw ZImgException(QString("Can not parse line (%1) with format <%2>").arg(list.join(',')).arg(header.join(',')));
    }
    if (list[12].compare("yes", Qt::CaseInsensitive) == 0) {
      m_input.doPyramidalSubclassSeparation = true;
    } else if (list[12].compare("no", Qt::CaseInsensitive) == 0) {
      m_input.doPyramidalSubclassSeparation = false;
    } else {
      throw ZImgException(QString("Can not parse line (%1) with format <%2>").arg(list.join(',')).arg(header.join(',')));
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

  ZImgInfo info = ZImg::readImgInfo(m_input.imgFilename).at(0);
  if (info.voxelSizeUnit != VoxelSizeUnit::none) {
    if (m_input.voxelSizeX != -1) {
      m_input.voxelSizeX = info.voxelSizeXInUm();
    } else {
      if (m_input.voxelSizeX != info.voxelSizeXInUm()) {
        throw ZImgException(QString("voxel size x %1 doesn't match voxel size x from img %2")
                            .arg(m_input.voxelSizeX).arg(info.voxelSizeXInUm()));
      }
    }
    if (m_input.voxelSizeY != -1) {
      m_input.voxelSizeY = info.voxelSizeYInUm();
    } else {
      if (m_input.voxelSizeY != info.voxelSizeYInUm()) {
        throw ZImgException(QString("voxel size y %1 doesn't match voxel size y from img %2")
                            .arg(m_input.voxelSizeY).arg(info.voxelSizeYInUm()));
      }
    }
    if (m_input.voxelSizeZ != -1) {
      m_input.voxelSizeZ = info.voxelSizeZInUm();
    } else {
      if (m_input.voxelSizeZ != info.voxelSizeZInUm()) {
        throw ZImgException(QString("voxel size z %1 doesn't match voxel size z from img %2")
                            .arg(m_input.voxelSizeZ).arg(info.voxelSizeZInUm()));
      }
    }
  }
  if (m_input.voxelSizeX == -1 || m_input.voxelSizeY == -1 || m_input.voxelSizeZ == -1) {
    throw ZImgException(QString("need valid voxel size information: %1, %2, %3")
                        .arg(m_input.voxelSizeX).arg(m_input.voxelSizeY).arg(m_input.voxelSizeZ));
  }

  if (m_input.dendriteChannel < 0 || m_input.dendriteChannel >= static_cast<int>(info.numChannels)) {
    throw ZImgException(QString("invalid dendrite channel %1 of file %2")
                        .arg(m_input.dendriteChannel).arg(m_input.imgFilename));
  }
  if (m_input.axonChannel >= 0 && m_input.axonChannel >= static_cast<int>(info.numChannels)) {
    throw ZImgException(QString("invalid axon channel %1 of file %2")
                        .arg(m_input.axonChannel).arg(m_input.imgFilename));
  }
  if (m_input.dendriteChannel == m_input.axonChannel) {
    throw ZImgException(QString("dendrite channel and axon channel are both %1")
                        .arg(m_input.dendriteChannel));
  }

  ZSwc tree(m_input.swcFilename);
  if (tree.numRoots() != 1) {
    throw ZImgException(QString("wrong swc file %1 with %2 roots.")
                        .arg(m_input.swcFilename).arg(tree.numRoots()));
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
    throw ZImgException(QString("output folder %1 does not exist and can not be created")
                        .arg(m_input.outputFolder));
  }

  m_processedSwcFilename = outputDir.filePath(swcFileInfo.fileName());
  if (m_input.doPyramidalFunctionalSeparation || m_input.doPyramidalSubclassSeparation) {
    if (inputSwcIsPyramidal()) { // already pyramidal, don't need process
      tree.setAsRoot(tree.thickestNode());
      tree.resortID();
      tree.save(m_processedSwcFilename);
      //QFile::copy(m_input.swcFilename, m_processedSwcFilename);
    } else { // make it pyramidal
      throw ZImgException(QString("input SWC %1 is not pyramidal SWC")
                          .arg(m_input.swcFilename));
      //      // mark soma from swc nodes
      //      tree.labelSomaAndOthers(3.0 / m_input.voxelSizeX);  // soma radius at least 3um
      //      tree.resortPyramidal();
      //      m_processedSwcFilename = outputDir.filePath(swcFileInfo.completeBaseName()) + "Py.swc";
      //      tree.resortID();
      //      tree.save(m_processedSwcFilename);
    }
  } else {
    // mark soma from swc nodes
    tree.labelSomaAndOthers(0); //3.0 / m_input.voxelSizeX);  // soma radius at least 3um
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
      throw ZImgException(QString("wrong layer swc file %1 with %2 roots.")
                          .arg(m_layerSwcFilename).arg(layerTree.numRoots()));
    }
    // mark soma from swc nodes
    layerTree.setAsRoot(layerTree.thickestNode());
    layerTree.resortID();
    layerTree.save(m_layerSwcFilename);
    getLayerFeature(tree, layerTree, nodeToLayer);
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
        throw ZImgException(QString("wrong subclass swc file %1 with %2 roots.")
                            .arg(m_subclassSwcFilename).arg(subclassTree.numRoots()));
      }
      subclassTree.setAsRoot(subclassTree.thickestNode());
      subclassTree.resortID();
      subclassTree.save(m_subclassSwcFilename);
      getSubclassFeature(tree, subclassTree, nodeToSubclass);
    } else {
      throw ZImgException(QString("Can not find subclass SWC %1.").arg(m_subclassSwcFilename));
    }
  }

  mergeSoma(tree, nodeToBlueness, nodeToLayer);
  removeSmallLeafBranch(tree, 6, 5);

  generateAnalysisFiles(tree, nodeToBlueness, nodeToLayer, nodeToSubclass);
}

void ZGenerateAnalysisTextFile::checkFileExist(const QString &filename) const
{
  if (!QFile::exists(filename)) {
    throw ZImgException(QString("file %1 doesn't exist").arg(filename));
  }
}

void ZGenerateAnalysisTextFile::getAxonFeature(const ZSwc &tree, std::map<ConstSwcTreeNode, double> &nodeToBlueness) const
{
  if (m_input.axonChannel < 0)
    return;
  ZImg axonImg(m_input.imgFilename, ZImgRegion(0,-1,0,-1,0,-1,m_input.axonChannel,m_input.axonChannel+1,0,1));
  for (ConstSwcTreeNode tn = tree.begin(); tn != tree.end(); ++tn) {
    if (ZSwc::isRoot(tn)) {
      double maxr = tn->radius + m_input.bluenessExtend / m_input.voxelSizeX;
      double zscale = m_input.voxelSizeZ / m_input.voxelSizeX;
      int left = std::floor(tn->x - maxr);
      int right = std::ceil(tn->x + maxr);
      int up = std::floor(tn->y - maxr);
      int down = std::ceil(tn->y + maxr);
      int zup = std::floor(tn->z - maxr / zscale);
      int zdown = std::ceil(tn->z + maxr / zscale);

      left = std::max(0, left);
      right = std::min(right, static_cast<int>(axonImg.width())-1);
      up = std::max(0, up);
      down = std::min(down, static_cast<int>(axonImg.height())-1);
      zup = std::max(0, zup);
      zdown = std::min(zdown, static_cast<int>(axonImg.depth())-1);

      int count = 0;
      double intensity = 0.0;
      for (int z = zup; z <= zdown; z++) {
        for (int y = up; y <= down; y++) {
          for (int x = left; x <= right; x++) {
            if ((x == roundTo<int>(tn->x) && y == roundTo<int>(tn->y) && z == roundTo<int>(tn->z)) ||
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
        LWARN() << "node" << tn->x << tn->y << tn->z << tn->radius;
        LWARN() << "img info" << axonImg.info().toQString();
        throw ZImgException("Swc root don't overlap with img?");
      }

      nodeToBlueness[tn] = intensity;
    } else {
      ConstSwcTreeNode parent = ZSwc::parent(tn);
      double maxr = std::max(tn->radius + m_input.bluenessExtend / m_input.voxelSizeX,
                             parent->radius + m_input.bluenessExtend / m_input.voxelSizeX);
      double zscale = m_input.voxelSizeZ / m_input.voxelSizeX;
      int left = std::floor(std::min(tn->x - maxr, parent->x - maxr));
      int right = std::ceil(std::max(tn->x + maxr, parent->x + maxr));
      int up = std::floor(std::min(tn->y - maxr, parent->y - maxr));
      int down = std::ceil(std::max(tn->y + maxr, parent->y + maxr));
      int zup = std::floor(std::min(tn->z - maxr / zscale, parent->z - maxr / zscale));
      int zdown = std::ceil(std::max(tn->z + maxr / zscale, parent->z + maxr / zscale));

      left = std::max(0, left);
      right = std::min(right, static_cast<int>(axonImg.width())-1);
      up = std::max(0, up);
      down = std::min(down, static_cast<int>(axonImg.height())-1);
      zup = std::max(0, zup);
      zdown = std::min(zdown, static_cast<int>(axonImg.depth())-1);

      int count = 0;
      double intensity = 0.0;
      for (int z = zup; z <= zdown; z++) {
        for (int y = up; y <= down; y++) {
          for (int x = left; x <= right; x++) {
            if ((x == roundTo<int>(tn->x) && y == roundTo<int>(tn->y) && z == roundTo<int>(tn->z)) ||
                (x == roundTo<int>(parent->x) && y == roundTo<int>(parent->y) && z == roundTo<int>(parent->z)) ||
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
        LWARN() << "node" << tn->x << tn->y << tn->z << tn->radius;
        LWARN() << "parent node" << parent->x << parent->y << parent->z << parent->radius;
        LWARN() << "img info" << axonImg.info().toQString();
        throw ZImgException("Swc seg don't overlap with img?");
      }

      nodeToBlueness[tn] = intensity;
    }
  }
}

void ZGenerateAnalysisTextFile::getLayerFeature(const ZSwc &tree, const ZSwc &layerTree, std::map<ConstSwcTreeNode, size_t> &nodeToLayer) const
{
  ConstSwcTreeNode tn = tree.begin();
  ConstSwcTreeNode layerTn = layerTree.begin();
  while (tn != tree.end()) {
    if (glm::length(glm::dvec3(tn->x - layerTn->x, tn->y - layerTn->y, tn->z - layerTn->z)) > 1.) {
      LWARN() << "node" << tn->x << tn->y << tn->z << tn->radius;
      LWARN() << "layer node" << layerTn->x << layerTn->y << layerTn->z << layerTn->radius;
      throw ZImgException("wrong layer node match");
    }
    nodeToLayer[tn] = layerTn->type;
    ++tn;
    ++layerTn;
  }
}

void ZGenerateAnalysisTextFile::getSubclassFeature(const ZSwc &tree, const ZSwc &subclassTree, std::map<ConstSwcTreeNode, size_t> &nodeToSubclass) const
{
  ConstSwcTreeNode tn = tree.begin();
  ConstSwcTreeNode layerTn = subclassTree.begin();
  while (tn != tree.end()) {
    if (glm::length(glm::dvec3(tn->x - layerTn->x, tn->y - layerTn->y, tn->z - layerTn->z)) > 1.) {
      LWARN() << "node" << tn->x << tn->y << tn->z << tn->radius;
      LWARN() << "subclass node" << layerTn->x << layerTn->y << layerTn->z << layerTn->radius;
      throw ZImgException("wrong subclass node match");
    }
    nodeToSubclass[tn] = layerTn->type;
    ++tn;
    ++layerTn;
  }
}

void ZGenerateAnalysisTextFile::writeFeatureSwc(const ZSwc &treeIn, const std::map<ConstSwcTreeNode, double> &nodeToFeature, const QString &outSwcName) const
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

double ZGenerateAnalysisTextFile::pointFrustumConeDist(double x, double y, double z,
                                                       const ConstSwcTreeNode &start, const ConstSwcTreeNode &end, double *fracOut) const
{
  double dist;
  glm::dvec3 pt(x,y,z);
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
  if (fracOut) *fracOut = frac;
  if (frac < 0)
    dist = std::sqrt(normbp) - start->radius * m_input.voxelSizeX;
  else if (frac > 1)
    dist = std::sqrt(normtp) - end->radius * m_input.voxelSizeX;
  else {
    double radius = m_input.voxelSizeX * ((1-frac)*start->radius + frac*end->radius);
    dist = std::sqrt(normbp - dotbptb * dotbptb / normtb) - radius;
  }
  return dist;
}

double ZGenerateAnalysisTextFile::pointSphereDist(double x, double y, double z, const ZGenerateAnalysisTextFile::ConstSwcTreeNode &tn) const
{
  glm::dvec3 pt(x,y,z);
  glm::dvec3 center(tn->x, tn->y, tn->z);
  glm::dvec3 res(m_input.voxelSizeX, m_input.voxelSizeY, m_input.voxelSizeZ);
  return glm::distance(pt*res, center*res);
}

double ZGenerateAnalysisTextFile::punctaFrustumConeDist(const ZPunctum &punctum,
                                                        const ConstSwcTreeNode &start, const ConstSwcTreeNode &end, double *frac) const
{
  return pointFrustumConeDist(punctum.x(), punctum.y(), punctum.z(), start, end, frac) - punctum.radius() * m_input.voxelSizeX;
}

double ZGenerateAnalysisTextFile::treeNodeDist(const ConstSwcTreeNode &tn, const ConstSwcTreeNode &ptn) const
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

void ZGenerateAnalysisTextFile::mergeSoma(ZSwc &tree,
                                          std::map<ConstSwcTreeNode, double> &nodeToBlueness,
                                          std::map<ConstSwcTreeNode, size_t> &nodeToLayer) const
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
  for (std::map<size_t, size_t>::const_iterator it = layerCount.begin();
       it != layerCount.end(); ++it) {
    if (it->second > bestCount) {
      nodeToLayer[somaNode] = it->first;
      bestCount = it->second;
    }
  }

  // get blueness of soma node
  if (m_input.axonChannel >= 0) {
    ZImg axonImg(m_input.imgFilename, ZImgRegion(0,-1,0,-1,0,-1,m_input.axonChannel,m_input.axonChannel+1,0,1));
    double maxr = somaNode->radius + m_input.bluenessExtend / m_input.voxelSizeX;
    double zscale = m_input.voxelSizeZ / m_input.voxelSizeX;
    int left = std::floor(somaNode->x - maxr);
    int right = std::ceil(somaNode->x + maxr);
    int up = std::floor(somaNode->y - maxr);
    int down = std::ceil(somaNode->y + maxr);
    int zup = std::floor(somaNode->z - maxr / zscale);
    int zdown = std::ceil(somaNode->z + maxr / zscale);

    left = std::max(0, left);
    right = std::min(right, static_cast<int>(axonImg.width())-1);
    up = std::max(0, up);
    down = std::min(down, static_cast<int>(axonImg.height())-1);
    zup = std::max(0, zup);
    zdown = std::min(zdown, static_cast<int>(axonImg.depth())-1);

    int count = 0;
    double intensity = 0.0;
    glm::dvec3 center(somaNode->x, somaNode->y, somaNode->z);
    glm::dvec3 res(m_input.voxelSizeX, m_input.voxelSizeY, m_input.voxelSizeZ);
    center *= res;
    for (int z = zup; z <= zdown; z++) {
      for (int y = up; y <= down; y++) {
        for (int x = left; x <= right; x++) {
          glm::dvec3 pt(x,y,z);
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

void ZGenerateAnalysisTextFile::removeSmallLeafBranch(ZSwc &tree, int numNodeThre, double lengthThre) const
{
  ZSwc::LeafIterator tn = tree.beginLeaf();
  while (tn != tree.endLeaf()) {
    ZSwc::LeafIterator tnnext = tn;
    ++tnnext;
    double branchLength = 0;
    int nNodes = 0;
    SwcTreeNode nodeBeforeBranchNode = tn;
    for (ZSwc::AncestorIterator tmptn = tree.beginAncestor(tn, true); tmptn != tree.endAncestor(tn) && !ZSwc::isBranchNode(tmptn); ++tmptn) {
      nNodes++;
      ZSwc::AncestorIterator parent = ZSwc::parent(tmptn);
      glm::dvec3 bot(tmptn->x, tmptn->y, tmptn->z);
      glm::dvec3 top(parent->x, parent->y, parent->z);
      glm::dvec3 res(m_input.voxelSizeX, m_input.voxelSizeY, m_input.voxelSizeZ);
      bot *= res;
      top *= res;
      branchLength += glm::length(bot - top);
      if(ZSwc::isBranchNode(parent)){
        nodeBeforeBranchNode = tmptn;
      }
    }
    if (nNodes < numNodeThre && branchLength < lengthThre) {
      tree.eraseSubtree(nodeBeforeBranchNode);
    }
    tn = tnnext;
  }
}

size_t ZGenerateAnalysisTextFile::labelBranch(const ZSwc &tree,
                                              std::map<ConstSwcTreeNode, size_t> &nodeToBranchId,
                                              std::map<size_t, size_t> &branchIdToParentBranchId,
                                              std::map<ConstSwcTreeNode, double> &nodeDistToParent,
                                              std::map<ConstSwcTreeNode, double> &nodeDistToBranchStart,
                                              std::map<ConstSwcTreeNode, double> &nodeDistToSoma,
                                              std::map<ConstSwcTreeNode, int> &nodeTopologyType,
                                              std::vector<Branch> &branches) const
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
    if (ZSwc::isRoot(tn))  {
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
      Branch &branch = branches[branchId-1];
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

ZGenerateAnalysisTextFile::ConstSwcTreeNode ZGenerateAnalysisTextFile::getNodeSegOfPunctum(const ZSwc &tree, const ZPunctum &punctum, size_t numBranches,
                                                                                           const std::map<ConstSwcTreeNode, size_t> &nodeToBranchId) const
{
  std::vector<double> distToBranch(numBranches, std::numeric_limits<double>::max());
  std::vector<ConstSwcTreeNode> nearestNodeOfBranch(numBranches);
  for (ConstSwcTreeNode tn = tree.begin(); tn != tree.end(); ++tn) {
    size_t branchId = nodeToBranchId.at(tn);
    if (branchId > 0) {
      double dist = punctaFrustumConeDist(punctum, ZSwc::parent(tn), tn);
      if (dist < distToBranch[branchId-1]) {
        distToBranch[branchId-1] = dist;
        nearestNodeOfBranch[branchId-1] = tn;
      }
    }
  }
  double distToTree = distToBranch[0];
  ConstSwcTreeNode res = nearestNodeOfBranch[0];
  std::vector<ConstSwcTreeNode> nodesWithinRange;
  for (size_t i=0; i<numBranches; ++i) {
    if (i > 0 && distToBranch[i] < distToTree) {
      distToTree = distToBranch[i];
      res = nearestNodeOfBranch[i];
    }
    if (distToBranch[i] <= m_input.maxDistToBranch) {
      nodesWithinRange.push_back(nearestNodeOfBranch[i]);
    }
  }

  if (distToTree - punctum.radius() * m_input.voxelSizeX <= 0.0) { //puncta inside branch, no more check
    return res;
  } else if (nodesWithinRange.size() == 0) { // zero branch in range
    //throw ZImgException("Need check");
    LWARN() << "Check Punctum from (no branch)" << m_input.punctaFilename << ":"
            << punctum.x() << punctum.y() << punctum.z() << punctum.radius()
            << punctum.maxIntensity() << punctum.meanIntensity();
    return ConstSwcTreeNode();
  } else if (nodesWithinRange.size() == 1) {
    return res;
  }

  // more than one neighbour branches
  res = intensityWeightedNearestNode(punctum.x(), punctum.y(), punctum.z(), nodesWithinRange);

//  if (nearestNode(punctum.x(), punctum.y(), punctum.z(), nodesWithinRange) != res) {
//    LWARN() << "Punctum assign example from" << m_input.punctaFilename << ":"
//            << punctum.x() << punctum.y() << punctum.z() << punctum.radius()
//            << punctum.maxIntensity() << punctum.meanIntensity();
//  }
  return res;
}

ZGenerateAnalysisTextFile::ConstSwcTreeNode ZGenerateAnalysisTextFile::intensityWeightedNearestNode(double x, double y, double z,
                                                                                                    const std::vector<ConstSwcTreeNode> &nodes) const
{
  //first crop out the region
  int left = roundTo<int>(x);
  int right = roundTo<int>(x);
  int up = roundTo<int>(y);
  int down = roundTo<int>(y);
  int zup = roundTo<int>(z);
  int zdown = roundTo<int>(z);
  for (size_t i=0; i<nodes.size(); ++i) {
    ConstSwcTreeNode parent = ZSwc::parent(nodes[i]);
    left = std::min(std::min(left, roundTo<int>(nodes[i]->x)), roundTo<int>(parent->x));
    right = std::max(std::max(right, roundTo<int>(nodes[i]->x)), roundTo<int>(parent->x));
    up = std::min(std::min(up, roundTo<int>(nodes[i]->y)), roundTo<int>(parent->y));
    down = std::max(std::max(down, roundTo<int>(nodes[i]->y)), roundTo<int>(parent->y));
    zup = std::min(std::min(zup, roundTo<int>(nodes[i]->z)), roundTo<int>(parent->z));
    zdown = std::max(std::max(zdown, roundTo<int>(nodes[i]->z)), roundTo<int>(parent->z));
  }
  ZImgInfo imgInfo = ZImg::readImgInfo(m_input.imgFilename).at(0);
  left = std::max(0, left);
  right = std::min(right, static_cast<int>(imgInfo.width)-1);
  up = std::max(0, up);
  down = std::min(down, static_cast<int>(imgInfo.height)-1);
  zup = std::max(0, zup);
  zdown = std::min(zdown, static_cast<int>(imgInfo.depth)-1);
  ZImgRegion rgn(left, right+1, up, down+1, zup, zdown+1, m_input.dendriteChannel, m_input.dendriteChannel+1, 0, 1);
  ZImg img(m_input.imgFilename, rgn);
  img.infoRef().voxelSizeUnit = VoxelSizeUnit::um;
  img.infoRef().voxelSizeX = m_input.voxelSizeX;
  img.infoRef().voxelSizeY = m_input.voxelSizeY;
  img.infoRef().voxelSizeZ = m_input.voxelSizeZ;

  ZImgGraph imgGraph(img);
  imgGraph.setConnectivity(26);
  ZImgAutoThreshold<> imgAutoThre;
  double cent1 = 0;
  double cent2 = 0;
  double thre1 = imgAutoThre.centroidThre<double>(cent1, cent2, img);
  double scale = cent2 - cent1;
  if (scale < 1.0)
    scale = 1.0;
  scale /= 9.2;
  imgGraph.build(ZImgGraph::EdgeWeight2(thre1, scale));

  ZVoxelCoordinate startCoord(roundTo<int>(x)-left, roundTo<int>(y)-up, roundTo<int>(z)-zup);
  std::vector<double> dist = imgGraph.shortestPaths(startCoord);

  std::vector<double> nodeMinDists(nodes.size(), std::numeric_limits<double>::max());
  for (size_t v=0; v<dist.size(); ++v) {
    ZVoxelCoordinate coord = img.indexToCoord(v);
    int nodeIdx = -1;
    for (int i=nodes.size()-1; i>=0; --i) {
      if (pointFrustumConeDist(coord.x+left, coord.y+up, coord.z+zup, nodes[i], ZSwc::parent(nodes[i])) <= 0.0) {
        nodeIdx = i;
        break;
      }
    }
    if (nodeIdx > -1)
      nodeMinDists[nodeIdx] = std::min(nodeMinDists[nodeIdx], dist[v]);
  }

  size_t minIndex = std::min_element(nodeMinDists.begin(), nodeMinDists.end()) - nodeMinDists.begin();
  //LINFO() << " min dist" << nodeMinDists[minIndex];
  //nodeMinDists[minIndex] = std::numeric_limits<double>::max();
  //LINFO() << " second min dist" << *std::min_element(nodeMinDists.begin(), nodeMinDists.end());

  return nodes[minIndex];
}

ZGenerateAnalysisTextFile::ConstSwcTreeNode ZGenerateAnalysisTextFile::nearestNode(double x, double y, double z, const std::vector<ConstSwcTreeNode> &nodes) const
{  
  double dist = std::numeric_limits<double>::max();
  ConstSwcTreeNode res;
  for (size_t i=0; i<nodes.size(); ++i) {
    const ConstSwcTreeNode &node = nodes[i];
    double nodeDist = pointFrustumConeDist(x,y,z,node,ZSwc::parent(node));
    if (nodeDist < dist) {
      dist = nodeDist;
      res = node;
    }
  }
  assert(!ZSwc::isNull(res));
  return res;
}

QDir ZGenerateAnalysisTextFile::getSubDir(const QString &subFoldername) const
{
  QDir outputDir(m_input.outputFolder);
  QFileInfo folderInfo(outputDir.filePath(subFoldername));
  if (!folderInfo.exists() || !folderInfo.isDir()) {
    QFile::remove(folderInfo.absoluteFilePath());
    outputDir.mkpath(subFoldername);
  }
  QDir res = QDir(folderInfo.absoluteFilePath());
  if (!res.exists()) {
    throw ZImgException(QString("Can not create %1 for writing").arg(folderInfo.absoluteFilePath()));
  }
  return res;
}

void ZGenerateAnalysisTextFile::generateAnalysisFiles(const ZSwc &tree,
                                                      const std::map<ConstSwcTreeNode, double> &nodeToBlueness,
                                                      const std::map<ConstSwcTreeNode, size_t> &nodeToLayer,
                                                      const std::map<ConstSwcTreeNode, size_t> &nodeToSubclass) const
{
  QDir outputDir(m_input.outputFolder);
  QFileInfo swcFileInfo(m_processedSwcFilename);
  QFileInfo punctaFileInfo(m_input.punctaFilename);
  QFileInfo somaPunctaFileInfo(m_input.somaPunctaFilename);
  QString branchFilename = outputDir.filePath(swcFileInfo.fileName()) + "_branch.txt";
  QString punctaFilename = outputDir.filePath(punctaFileInfo.fileName()) + "_puncta.txt";
  QString somaPunctaFilename = outputDir.filePath(somaPunctaFileInfo.fileName()) + "_puncta.txt";

  ZPuncta punctaList(m_input.somaPunctaFilename);
  std::ofstream somaPunctaStream;
  openFileStream(somaPunctaStream, somaPunctaFilename, std::ios_base::out | std::ios_base::trunc);
  somaPunctaStream << "# puncta id, x, y, z, volsize, meanIntensity, maxIntensity" << std::endl;

  size_t idx = 0;
  for (ZPuncta::const_iterator it = punctaList.begin(); it != punctaList.end(); ++it) {
    somaPunctaStream << idx++ << " " << it->x() << " " << it->y() << " "
                     << it->z() << " "
                     << it->volSize() << " " << it->meanIntensity() << " "
                     << it->maxIntensity() << std::endl;
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
  labelBranch(tree, nodeToBranchId, branchIdToParentBranchId,
              nodeDistToParent, nodeDistToBranchStart, nodeDistToSoma, nodeTopologyType, branches);
  punctaList = ZPuncta(m_input.punctaFilename);
  std::map<const ZPunctum*, ConstSwcTreeNode> punctumToNode;
  std::map<const ZPunctum*, double> punctumDistToBranchStart;
  std::map<const ZPunctum*, double> punctumDistToSoma;
  std::map<const ZPunctum*, double> punctumDistToSegmentStart;
  for (ZPuncta::const_iterator it = punctaList.begin(); it != punctaList.end(); ++it) {
    ConstSwcTreeNode tn = getNodeSegOfPunctum(tree, *it, branches.size(), nodeToBranchId);
    punctumToNode[&(*it)] = tn;
    if (ZSwc::isNull(tn))
      continue;

    nodeToPuncta[tn].push_back(&(*it));

    size_t branchId = nodeToBranchId[tn];
    bool firstSeg = branchId != nodeToBranchId[ZSwc::parent(tn)];

    double frac;
    punctaFrustumConeDist(*it, ZSwc::parent(tn), tn, &frac);
    if (frac < 0) {
      punctumDistToBranchStart[&(*it)] = firstSeg ? 0 : nodeDistToBranchStart[ZSwc::parent(tn)];
      punctumDistToSoma[&(*it)] = nodeDistToSoma[ZSwc::parent(tn)];
      punctumDistToSegmentStart[&(*it)] = 0;
    } else if (frac > 1) {
      punctumDistToBranchStart[&(*it)] = nodeDistToBranchStart[tn];
      punctumDistToSoma[&(*it)] = nodeDistToSoma[tn];
      punctumDistToSegmentStart[&(*it)] = nodeDistToParent[tn];
    } else {
      punctumDistToBranchStart[&(*it)] = (firstSeg ? 0 : nodeDistToBranchStart[ZSwc::parent(tn)]) + frac * nodeDistToParent[tn];
      punctumDistToSoma[&(*it)] = nodeDistToSoma[ZSwc::parent(tn)] + frac * nodeDistToParent[tn];
      punctumDistToSegmentStart[&(*it)] = frac * nodeDistToParent[tn];
    }
  }

  std::ofstream branchStream;
  openFileStream(branchStream, branchFilename, std::ios_base::out | std::ios_base::trunc);
  branchStream << "# branch id, type, x, y, z, radius, blueness, layer, topological type, distToBranchStart, distToSoma" << std::endl;

  for (size_t i=0; i<branches.size(); ++i) {
    Branch &branch = branches[i];
    for (size_t j=0; j<branch.nodes.size(); ++j) {
      const ConstSwcTreeNode &tn = branch.nodes[j];
      branchStream << branch.id << " " << tn->type << " "
                   << tn->x << " " << tn->y << " "
                   << tn->z << " " << tn->radius << " "
                   << nodeToBlueness.at(tn) << " "
                   << nodeToLayer.at(tn) << " "
                   << nodeTopologyType.at(tn) << " "
                   << nodeDistToBranchStart.at(tn) << " "
                   << nodeDistToSoma.at(tn) << std::endl;
    }
  }
  branchStream.close();

  std::ofstream punctaStream;
  openFileStream(punctaStream, punctaFilename, std::ios_base::out | std::ios_base::trunc);
  punctaStream << "# puncta id, x, y, z, branch id, offset, volsize, meanIntensity, maxIntensity, distToBranchStart, distToSoma" << std::endl;

  idx = 0;
  for (ZPuncta::const_iterator it = punctaList.begin(); it != punctaList.end(); ++it) {
    if (ZSwc::isNull(punctumToNode[&(*it)]))
      continue;
    size_t branchId = nodeToBranchId[punctumToNode[&(*it)]];
    double offset = punctumDistToBranchStart[&(*it)] / branches[branchId - 1].length;
    punctaStream << idx++ << " " << it->x() << " " << it->y() << " "
                 << it->z() << " " << branchId << " " << offset << " "
                 << it->volSize() << " " << it->meanIntensity() << " "
                 << it->maxIntensity() << " "
                 << punctumDistToBranchStart[&(*it)] << " "
                 << punctumDistToSoma[&(*it)] << std::endl;
  }
  punctaStream.close();

  // separated files

  // techinical branch
  QDir technicalBranchFolder = getSubDir("Technical_Branches");

  for (size_t i=0; i<branches.size(); ++i) {
    Branch &branch = branches[i];
    QString outSwcName = technicalBranchFolder.filePath(QString("branch_%1.swc")
                                                        .arg(branch.id, 4, 10, QChar('0')));
    QString outSwcTxtName = technicalBranchFolder.filePath(QString("branch_%1.txt")
                                                           .arg(branch.id, 4, 10, QChar('0')));
    QString outPunctaName = technicalBranchFolder.filePath(QString("branch_%1_%2")
                                                           .arg(branch.id, 4, 10, QChar('0'))
                                                           .arg(QFileInfo(m_input.punctaFilename).fileName()));
    QString outPunctaTxtName = technicalBranchFolder.filePath(QString("branch_%1_%2.txt")
                                                              .arg(branch.id, 4, 10, QChar('0'))
                                                              .arg(QFileInfo(m_input.punctaFilename).fileName()));

    std::ofstream outSwcStream;
    openFileStream(outSwcStream, outSwcName, std::ios_base::out | std::ios_base::trunc);

    std::ofstream outSwcTxtStream;
    openFileStream(outSwcTxtStream, outSwcTxtName, std::ios_base::out | std::ios_base::trunc);
    outSwcTxtStream << "# Branch Part, Branch Type, x, y, z, radius, blueness, layer, topological type, distToBranchStart, distToSoma" << std::endl;

    std::ofstream outPunctaTxtStream;
    openFileStream(outPunctaTxtStream, outPunctaTxtName, std::ios_base::out | std::ios_base::trunc);
    outPunctaTxtStream << "# Branch location, x, y, z, offset, volsize, meanIntensity, maxIntensity, distToBranchStart, distToSoma" << std::endl;

    ZPuncta tmpPunc;
    for (size_t j=0; j<branch.nodes.size(); ++j) {
      const ConstSwcTreeNode &tn = branch.nodes[j];
      outSwcStream << tn->id << " " << tn->type << " " << tn->x << " "
                   << tn->y << " " << tn->z << " " << tn->radius << " "
                   << ZSwc::parentID(tn) << std::endl;
      if (j == 0) {
        outSwcTxtStream << 0 << " ";
      } else if (j == branch.nodes.size() - 1) {
        outSwcTxtStream << 2 << " ";
      } else {
        outSwcTxtStream << 1 << " ";
      }
      outSwcTxtStream << tn->type << " " << tn->x << " "
                      << tn->y << " "
                      << tn->z << " " << tn->radius << " "
                      << nodeToBlueness.at(tn) << " "
                      << nodeToLayer.at(tn) << " "
                      << nodeTopologyType.at(tn) << " "
                      << nodeDistToBranchStart.at(tn) << " "
                      << nodeDistToSoma.at(tn) << std::endl;

      if (j > 0) {
        std::vector<const ZPunctum*> puncta = nodeToPuncta[tn];
        for (size_t k=0; k<puncta.size(); ++k) {
          const ZPunctum *punctum = puncta[k];
          tmpPunc.push_back(*punctum);
          outPunctaTxtStream << j << " " << punctum->x() << " " << punctum->y() << " "
                             << punctum->z() << " "
                             << (punctumDistToBranchStart[punctum] / branch.length) << " "
                             << punctum->volSize() << " " << punctum->meanIntensity() << " "
                             << punctum->maxIntensity() << " "
                             << punctumDistToBranchStart[punctum] << " "
                             << punctumDistToSoma[punctum] << std::endl;
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

      QDir *currentBranchFolder = nullptr;
      int *currentBranchIndex = nullptr;
      if (tmptn->type == ZSwc::ApicalDendriteType) {
        currentBranchFolder = &FCApicalBranchFolder;
        currentBranchIndex = &apicalIndex;
      } else if (tmptn->type == ZSwc::BasalDendriteType) {
        currentBranchFolder = &FCBasalBranchFolder;
        currentBranchIndex = &basalIndex;
      }

      QString outSwcName = currentBranchFolder->filePath(QString("branch_%1.swc")
                                                         .arg(*currentBranchIndex, 4, 10, QChar('0')));
      QString outSwcTxtName = currentBranchFolder->filePath(QString("branch_%1.txt")
                                                            .arg(*currentBranchIndex, 4, 10, QChar('0')));
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

      std::ofstream outSwcStream;
      openFileStream(outSwcStream, outSwcName, std::ios_base::out | std::ios_base::trunc);

      std::ofstream outSwcTxtStream;
      openFileStream(outSwcTxtStream, outSwcTxtName, std::ios_base::out | std::ios_base::trunc);
      outSwcTxtStream << "# Branch Part, Branch Type, x, y, z, radius, blueness, layer, topological type, distToBranchStart, distToSoma" << std::endl;

      std::ofstream outPunctaTxtStream;
      openFileStream(outPunctaTxtStream, outPunctaTxtName, std::ios_base::out | std::ios_base::trunc);
      outPunctaTxtStream << "# Branch location, x, y, z, offset, volsize, meanIntensity, maxIntensity, distToBranchStart, distToSoma" << std::endl;

      int nodeIdx = -1;
      ZPuncta tmpPunc;
      for (size_t i=0; i<branchIdList.size(); ++i) {
        size_t branchId = branchIdList[i];
        Branch &branch = branches[branchId-1];
        for (size_t j=0; j<branch.nodes.size(); ++j) {
          if (i > 0 && j == 0) { // skip first one since it is already processed
            continue;
          }
          ++nodeIdx;

          const ConstSwcTreeNode &btn = branch.nodes[j];
          outSwcStream << btn->id << " " << btn->type << " " << btn->x << " "
                       << btn->y << " " << btn->z << " " << btn->radius << " "
                       << ZSwc::parentID(btn) << std::endl;
          if (i == 0 && j == 0) {
            outSwcTxtStream << 0 << " ";
          } else if (i == branchIdList.size() - 1 && j == branch.nodes.size() - 1) {
            outSwcTxtStream << 2 << " ";
          } else {
            outSwcTxtStream << 1 << " ";
          }
          outSwcTxtStream << btn->type << " " << btn->x << " "
                          << btn->y << " "
                          << btn->z << " " << btn->radius << " "
                          << nodeToBlueness.at(btn) << " "
                          << nodeToLayer.at(btn) << " "
                          << nodeTopologyType.at(btn) << " "
                          << nodeDistToBranchStart.at(btn) << " "
                          << nodeDistToSoma.at(btn) << std::endl;

          // don't need to skip i == 0 and j == 0 case because btn will be soma and there
          // should be no puncta for soma
          std::vector<const ZPunctum*> puncta = nodeToPuncta[btn];
          for (size_t k=0; k<puncta.size(); ++k) {
            const ZPunctum *punctum = puncta[k];
            tmpPunc.push_back(*punctum);
            outPunctaTxtStream << nodeIdx << " " << punctum->x() << " " << punctum->y() << " "
                               << punctum->z() << " "
                               << (punctumDistToSoma[punctum] / length) << " "
                               << punctum->volSize() << " " << punctum->meanIntensity() << " "
                               << punctum->maxIntensity() << " "
                               << punctumDistToSoma[punctum] << " "
                               << punctumDistToSoma[punctum] << std::endl;
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
      if (ZSwc::isRoot(tn) || (nodeSubclass != ZSwc::MainTrunkType && nodeSubclass != ZSwc::ApicalTuftType))  {
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
        Branch &branch = MTTBranches[branchId-1];
        branch.id = branchId;
        ConstSwcTreeNode parent = ZSwc::parent(tn);
        if (nodeToMTTBranchId[parent] != branch.id) { // duplicate parent
          branch.nodes.push_back(parent);
        }
        branch.nodes.push_back(tn);
        branch.length += nodeDistToParent[tn];
      }
    }
    for (size_t i=0; i<MTTBranches.size(); ++i) {
      Branch &branch = MTTBranches[i];

      ConstSwcTreeNode tmptn = branch.nodes[branch.nodes.size()-1];
      size_t nodeSubclass = nodeToSubclass.at(tmptn);

      QDir *currentBranchFolder = nullptr;
      int *currentBranchIndex = nullptr;

      if (nodeSubclass == ZSwc::MainTrunkType) {
        currentBranchFolder = &mainTrunkBranchFolder;
        currentBranchIndex = &mainTrunkIndex;
      } else if (nodeSubclass == ZSwc::ApicalTuftType) {
        currentBranchFolder = &apicalTuftBranchFolder;
        currentBranchIndex = &apicalTuftIndex;
      } else {
        assert(false);
      }

      QString outSwcName = currentBranchFolder->filePath(QString("branch_%1.swc")
                                                         .arg(*currentBranchIndex, 4, 10, QChar('0')));
      QString outSwcTxtName = currentBranchFolder->filePath(QString("branch_%1.txt")
                                                            .arg(*currentBranchIndex, 4, 10, QChar('0')));
      QString outPunctaName = currentBranchFolder->filePath(QString("branch_%1_%2")
                                                            .arg(*currentBranchIndex, 4, 10, QChar('0'))
                                                            .arg(QFileInfo(m_input.punctaFilename).fileName()));
      QString outPunctaTxtName = currentBranchFolder->filePath(QString("branch_%1_%2.txt")
                                                               .arg(*currentBranchIndex, 4, 10, QChar('0'))
                                                               .arg(QFileInfo(m_input.punctaFilename).fileName()));
      (*currentBranchIndex) += 1;

      std::ofstream outSwcStream;
      openFileStream(outSwcStream, outSwcName, std::ios_base::out | std::ios_base::trunc);

      std::ofstream outSwcTxtStream;
      openFileStream(outSwcTxtStream, outSwcTxtName, std::ios_base::out | std::ios_base::trunc);
      outSwcTxtStream << "# Branch Part, Branch Type, x, y, z, radius, blueness, layer, topological type, distToBranchStart, distToSoma" << std::endl;

      std::ofstream outPunctaTxtStream;
      openFileStream(outPunctaTxtStream, outPunctaTxtName, std::ios_base::out | std::ios_base::trunc);
      outPunctaTxtStream << "# Branch location, x, y, z, offset, volsize, meanIntensity, maxIntensity, distToBranchStart, distToSoma" << std::endl;

      double segmentStartToBranchStartLength = 0.0;
      ZPuncta tmpPunc;
      for (size_t j=0; j<branch.nodes.size(); ++j) {
        const ConstSwcTreeNode &tn = branch.nodes[j];
        outSwcStream << tn->id << " " << tn->type << " " << tn->x << " "
                     << tn->y << " " << tn->z << " " << tn->radius << " "
                     << ZSwc::parentID(tn) << std::endl;
        if (j == 0) {
          outSwcTxtStream << 0 << " ";
        } else if (j == branch.nodes.size() - 1) {
          outSwcTxtStream << 2 << " ";
        } else {
          outSwcTxtStream << 1 << " ";
        }
        outSwcTxtStream << tn->type << " " << tn->x << " "
                        << tn->y << " "
                        << tn->z << " " << tn->radius << " "
                        << nodeToBlueness.at(tn) << " "
                        << nodeToLayer.at(tn) << " "
                        << nodeTopologyType.at(tn) << " "
                        << nodeDistToBranchStart.at(tn) << " "
                        << nodeDistToSoma.at(tn) << std::endl;

        if (j > 0) {
          std::vector<const ZPunctum*> puncta = nodeToPuncta[tn];
          for (size_t k=0; k<puncta.size(); ++k) {
            const ZPunctum *punctum = puncta[k];
            tmpPunc.push_back(*punctum);
            double punctumDistToMTTBranchStart = segmentStartToBranchStartLength + punctumDistToSegmentStart[punctum];
            outPunctaTxtStream << j << " " << punctum->x() << " " << punctum->y() << " "
                               << punctum->z() << " "
                               << (punctumDistToMTTBranchStart / branch.length) << " "
                               << punctum->volSize() << " " << punctum->meanIntensity() << " "
                               << punctum->maxIntensity() << " "
                               << punctumDistToMTTBranchStart << " "
                               << punctumDistToSoma[punctum] << std::endl;
          }

          segmentStartToBranchStartLength += nodeDistToParent[tn];
        }
      }
      tmpPunc.save(outPunctaName);
      tmpPunc.clear();
    }

    // other not main trunk and not tuft branches
    for (size_t i=0; i<branches.size(); ++i) {
      Branch &branch = branches[i];

      ConstSwcTreeNode tmptn = branch.nodes[branch.nodes.size()-1];
      size_t nodeSubclass = nodeToSubclass.at(tmptn);
      size_t nodeType = tmptn->type;
      bool nodeIsLeaf = ZSwc::isLeaf(tmptn);

      QDir *currentBranchFolder = nullptr;
      int *currentBranchIndex = nullptr;

      if (nodeSubclass != ZSwc::MainTrunkType && nodeSubclass != ZSwc::ApicalTuftType) {
        if (nodeType == ZSwc::ApicalDendriteType) {
          currentBranchFolder = nodeIsLeaf ? &apicalObliqueTerminalBranchFolder : &apicalObliqueIntermediateBranchFolder;
          currentBranchIndex = nodeIsLeaf ? &apicalObliqueTerminalIndex : &apicalObliqueIntermediateIndex;
        } else if (nodeType == ZSwc::BasalDendriteType) {
          currentBranchFolder = nodeIsLeaf ? &basalTerminalBranchFolder : &basalIntermediateBranchFolder;
          currentBranchIndex = nodeIsLeaf ? &basalTerminalIndex : &basalIntermediateIndex;
        } else {
          assert(false);
        }

        QString outSwcName = currentBranchFolder->filePath(QString("branch_%1.swc")
                                                           .arg(*currentBranchIndex, 4, 10, QChar('0')));
        QString outSwcTxtName = currentBranchFolder->filePath(QString("branch_%1.txt")
                                                              .arg(*currentBranchIndex, 4, 10, QChar('0')));
        QString outPunctaName = currentBranchFolder->filePath(QString("branch_%1_%2")
                                                              .arg(*currentBranchIndex, 4, 10, QChar('0'))
                                                              .arg(QFileInfo(m_input.punctaFilename).fileName()));
        QString outPunctaTxtName = currentBranchFolder->filePath(QString("branch_%1_%2.txt")
                                                                 .arg(*currentBranchIndex, 4, 10, QChar('0'))
                                                                 .arg(QFileInfo(m_input.punctaFilename).fileName()));
        (*currentBranchIndex) += 1;

        std::ofstream outSwcStream;
        openFileStream(outSwcStream, outSwcName, std::ios_base::out | std::ios_base::trunc);

        std::ofstream outSwcTxtStream;
        openFileStream(outSwcTxtStream, outSwcTxtName, std::ios_base::out | std::ios_base::trunc);
        outSwcTxtStream << "# Branch Part, Branch Type, x, y, z, radius, blueness, layer, topological type, distToBranchStart, distToSoma" << std::endl;

        std::ofstream outPunctaTxtStream;
        openFileStream(outPunctaTxtStream, outPunctaTxtName, std::ios_base::out | std::ios_base::trunc);
        outPunctaTxtStream << "# Branch location, x, y, z, offset, volsize, meanIntensity, maxIntensity, distToBranchStart, distToSoma" << std::endl;

        ZPuncta tmpPunc;
        for (size_t j=0; j<branch.nodes.size(); ++j) {
          const ConstSwcTreeNode &tn = branch.nodes[j];
          outSwcStream << tn->id << " " << tn->type << " " << tn->x << " "
                       << tn->y << " " << tn->z << " " << tn->radius << " "
                       << ZSwc::parentID(tn) << std::endl;
          if (j == 0) {
            outSwcTxtStream << 0 << " ";
          } else if (j == branch.nodes.size() - 1) {
            outSwcTxtStream << 2 << " ";
          } else {
            outSwcTxtStream << 1 << " ";
          }
          outSwcTxtStream << tn->type << " " << tn->x << " "
                          << tn->y << " "
                          << tn->z << " " << tn->radius << " "
                          << nodeToBlueness.at(tn) << " "
                          << nodeToLayer.at(tn) << " "
                          << nodeTopologyType.at(tn) << " "
                          << nodeDistToBranchStart.at(tn) << " "
                          << nodeDistToSoma.at(tn) << std::endl;

          if (j > 0) {
            std::vector<const ZPunctum*> puncta = nodeToPuncta[tn];
            for (size_t k=0; k<puncta.size(); ++k) {
              const ZPunctum *punctum = puncta[k];
              tmpPunc.push_back(*punctum);
              outPunctaTxtStream << j << " " << punctum->x() << " " << punctum->y() << " "
                                 << punctum->z() << " "
                                 << (punctumDistToBranchStart[punctum] / branch.length) << " "
                                 << punctum->volSize() << " " << punctum->meanIntensity() << " "
                                 << punctum->maxIntensity() << " "
                                 << punctumDistToBranchStart[punctum] << " "
                                 << punctumDistToSoma[punctum] << std::endl;
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
