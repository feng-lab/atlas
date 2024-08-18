#include "zswc.h"
#include "zexception.h"
#include "zioutils.h"
#include "zlog.h"
#include "zstringutils.h"
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>
#include <map>

namespace nim {

ZSwc::SwcTreeNode ZSwc::thickestNode()
{
  SwcTreeNode thickestNode;
  double maxRadius = std::numeric_limits<double>::lowest();
  for (SwcTreeNode tn = begin(); tn != end(); ++tn) {
    if (tn->radius > maxRadius) {
      maxRadius = tn->radius;
      thickestNode = tn;
    }
  }
  return thickestNode;
}

ZSwc::SwcTreeNode ZSwc::thickestNode(const ZSwc::SwcTreeNode& subtree)
{
  CHECK(isValid(subtree));
  SwcTreeNode thickestNode;
  double maxRadius = std::numeric_limits<double>::lowest();
  for (SwcTreeNode tn = begin(subtree); tn != end(subtree); ++tn) {
    if (tn->radius > maxRadius) {
      maxRadius = tn->radius;
      thickestNode = tn;
    }
  }
  return thickestNode;
}

void ZSwc::labelSomaAndOthers(double radiusThre, int64_t somaType, int64_t otherType)
{
  for (RootIterator rt = beginRoot(); rt != endRoot(); ++rt) {
    SwcTreeNode rootNode = thickestNode(rt);
    double maxRadius = rootNode->radius;

    if (rootNode != rt) {
      setAsRoot(rootNode);
      rt = rootNode;
    }

    for (BreadthFirstIterator it = beginBreadthFirst(rt); it != endBreadthFirst(rt); ++it) {
      if (maxRadius >= radiusThre &&
          (rootNode == it || (parent(it)->type == somaType && it->radius * 3.0 >= maxRadius))) {
        it->type = somaType;
      } else {
        it->type = otherType;
      }
    }
  }
}

void ZSwc::resortPyramidal(int64_t basalType, int64_t apicalType, int64_t somaType)
{
  if (empty()) {
    return;
  }
  CHECK(numRoots() == 1);
  SwcTreeNode soma = begin();
  CHECK(thickestNode() == soma); // soma must be correct
  std::vector<SwcTreeNode> somaChildren;
  SwcTreeNode it = begin();
  for (++it; it != end(); ++it) {
    if (it->type != somaType) {
      it->type = basalType;
    }
    if (parent(it)->type == somaType && it->type != somaType) {
      somaChildren.push_back(it);
    }
  }
  for (auto& sch : somaChildren) {
    if (sch->y > soma->y) { // apical
      for (SwcTreeNode tn = begin(sch); tn != end(sch); ++tn) {
        tn->type = apicalType;
      }
    }
  }
}

void ZSwc::resortID()
{
  int64_t id = 1;
  for (BreadthFirstIterator it = beginBreadthFirst(); it != endBreadthFirst(); ++it) {
    it->id = id++;
    it->parentID = parentID(it);
  }
}

void ZSwc::load(const QString& filename)
{
  try {
    clear();

    std::map<int64_t, SwcNode> nodeMap;
    int version = 1;

    if (version == 1) {
      std::ifstream file = openIFStream(filename, std::ios_base::in);

      if (!file.is_open()) {
        throw ZException("Can not read file.", ZException::Option::CheckErrno);
      }

      std::string line;
      while (std::getline(file, line)) {
        if (!file) {
          throw ZException("Error while reading file.", ZException::Option::CheckErrno);
        }
        removeComment(line, "#", true);
        std::vector<std::string_view> fieldList =
          absl::StrSplit(line, absl::ByAnyChar(" \t\n\r\v\f"), absl::SkipEmpty());
        if (fieldList.size() >= 7) {
          SwcNode node;
          stringToValue(fieldList[0], node.id);
          stringToValue(fieldList[1], node.type);
          stringToValue(fieldList[2], node.x);
          stringToValue(fieldList[3], node.y);
          stringToValue(fieldList[4], node.z);
          stringToValue(fieldList[5], node.radius);
          stringToValue(fieldList[6], node.parentID);
          nodeMap[node.id] = node;
        } else if (!line.empty()) {
          throw ZException(fmt::format("Wrong SWC format: {}.", line));
        }
      }
    } else {
      QFile qFile(filename);
      if (!qFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        throw ZException("Can not read file.", ZException::Option::CheckErrno);
      }

      QTextStream stream(&qFile);
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
      stream.setCodec("UTF-8");
#endif
      for (QString line = stream.readLine(); !line.isNull(); line = stream.readLine()) {
        line = line.trimmed();
        if (stream.status() != QTextStream::Ok) {
          throw ZException("Error while reading file.", ZException::Option::CheckErrno);
        }
        removeComment(line, QString("#"), true);
        static QRegularExpression rx("\\s+");
        QStringList fieldList = line.split(rx, Qt::SkipEmptyParts);
        if (fieldList.size() >= 7) {
          SwcNode node;
          bool ok;
          node.id = fieldList[0].toInt(&ok);
          if (!ok || node.id < 0) {
            throw ZException(fmt::format("Wrong SWC format: {}.", line));
          }
          node.type = fieldList[1].toInt(&ok);
          if (!ok) {
            throw ZException(fmt::format("Wrong SWC format: {}.", line));
          }
          node.x = fieldList[2].toDouble(&ok);
          if (!ok) {
            throw ZException(fmt::format("Wrong SWC format: {}.", line));
          }
          node.y = fieldList[3].toDouble(&ok);
          if (!ok) {
            throw ZException(fmt::format("Wrong SWC format: {}.", line));
          }
          node.z = fieldList[4].toDouble(&ok);
          if (!ok) {
            throw ZException(fmt::format("Wrong SWC format: {}.", line));
          }
          node.radius = fieldList[5].toDouble(&ok);
          if (!ok) {
            throw ZException(fmt::format("Wrong SWC format: {}.", line));
          }
          node.parentID = fieldList[6].toInt(&ok);
          if (!ok) {
            throw ZException(fmt::format("Wrong SWC format: {}.", line));
          }
          nodeMap[node.id] = node;
        } else if (!line.isEmpty()) {
          throw ZException(fmt::format("Wrong SWC format: {}.", line));
        }
      }
    }

    std::map<int64_t, Iterator> itMap;
    while (!nodeMap.empty()) {
      auto it = nodeMap.begin();
      while (it != nodeMap.end()) {
        auto parentID = it->second.parentID;
        if (auto nodeIt = itMap.find(parentID); nodeIt != itMap.end()) {
          itMap[it->first] = appendChild(nodeIt->second, it->second);
          it = nodeMap.erase(it);
        } else if (!nodeMap.contains(parentID)) {
          itMap[it->first] = appendRoot(it->second);
          it = nodeMap.erase(it);
        } else {
          ++it;
        }
      }
    }
  }
  catch (const ZException& e) {
    throw ZException(fmt::format("Can not load swc {}: {}", filename, e.what()));
  }
}

void ZSwc::save(const QString& filename) const
{
  try {
    auto of = openOFStream(filename, std::ios_base::out);

    of << "#id type x y z radius parentID\n";
    if (!of) {
      throw ZException("Error while writing file header", ZException::Option::CheckErrno);
    }
    for (ConstBreadthFirstIterator it = beginBreadthFirst(); it != endBreadthFirst(); ++it) {
      of << fmt::format("{} {} {} {} {} {} {}\n", it->id, it->type, it->x, it->y, it->z, it->radius, parentID(it));
      if (!of) {
        throw ZException("Error while writing file", ZException::Option::CheckErrno);
      }
    }
  }
  catch (const ZException& e) {
    throw ZException(fmt::format("Can not save swc {}: {}", filename, e.what()));
  }
}

void ZSwc::addLine(const std::vector<glm::dvec3>& line, double radius)
{
  if (line.empty()) {
    return;
  }
  SwcNode root(0, 0, line[0].x, line[0].y, line[0].z, radius, -1);
  Iterator it = appendRoot(root);
  for (size_t i = 1; i < line.size(); ++i) {
    SwcNode node(0, 0, line[i].x, line[i].y, line[i].z, radius, -1);
    it = appendChild(it, node);
  }
}

} // namespace nim
