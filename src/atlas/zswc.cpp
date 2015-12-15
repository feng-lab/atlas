#include "zswc.h"

#include "zioutils.h"
#include <QFile>
#include <QTextStream>
#include <QStringList>
#include "zexception.h"
#include <map>
#include "QsLog.h"
#include "zstringutils.h"

namespace nim {

ZSwc::SwcTreeNode ZSwc::thickestNode()
{
  SwcTreeNode thickestNode;
  double maxRadius = -std::numeric_limits<double>::max();
  for (SwcTreeNode tn = begin(); tn != end(); ++tn) {
    if (tn->radius > maxRadius) {
      maxRadius = tn->radius;
      thickestNode = tn;
    }
  }
  return thickestNode;
}

ZSwc::SwcTreeNode ZSwc::thickestNode(const ZSwc::SwcTreeNode &subtree)
{
  assert(subtree.node && subtree.node != m_head && subtree.node != m_tail);
  SwcTreeNode thickestNode;
  double maxRadius = -std::numeric_limits<double>::max();
  for (SwcTreeNode tn = begin(subtree); tn != end(subtree); ++tn) {
    if (tn->radius > maxRadius) {
      maxRadius = tn->radius;
      thickestNode = tn;
    }
  }
  return thickestNode;
}

void ZSwc::labelSomaAndOthers(double radiusThre, int somaType, int otherType)
{
  for (RootIterator rt = beginRoot(); rt != endRoot(); ++rt) {
    SwcTreeNode rootNode  = thickestNode(rt);
    double maxRadius = rootNode->radius;

    if (rootNode != rt) {
      setAsRoot(rootNode);
      rt = rootNode;
    }

    for (BreadthFirstIterator it = beginBreadthFirst(rt); it != endBreadthFirst(rt); ++it) {
      if (maxRadius >= radiusThre && (rootNode == it || (parent(it)->type == somaType && it->radius * 3.0 >= maxRadius))) {
        it->type = somaType;
      } else {
        it->type = otherType;
      }
    }
  }
}

void ZSwc::resortPyramidal(int basalType, int apicalType, int somaType)
{
  if (empty())
    return;
  assert(numRoots() == 1);
  SwcTreeNode soma = begin();
  assert(thickestNode() == soma);  // soma must be correct
  std::vector<SwcTreeNode> somaChildren;
  SwcTreeNode it = begin();
  for (++it; it != end(); ++it) {
    if (it->type != somaType)
      it->type = basalType;
    if (parent(it)->type == somaType && it->type != somaType)
      somaChildren.push_back(it);
  }
  for (size_t i=0; i<somaChildren.size(); ++i) {
    if (somaChildren[i]->y > soma->y) { // apical
      for (SwcTreeNode tn = begin(somaChildren[i]); tn != end(somaChildren[i]); ++tn) {
        tn->type = apicalType;
      }
    }
  }
}

void ZSwc::resortID()
{
  size_t id = 1;
  for (BreadthFirstIterator it = beginBreadthFirst(); it != endBreadthFirst(); ++it) {
    it->id = id++;
    it->parentID = parentID(it);
  }
}

void ZSwc::load(const QString &filename)
{
  try {
  clear();

  QFile qFile(filename);
  if (!qFile.open(QIODevice::ReadOnly | QIODevice::Text))
    throw ZIOException("Can not read file.");

  std::map<int, SwcNode> nodeMap;

  QTextStream stream(&qFile);
  while (!stream.atEnd()) {
    QString line = stream.readLine().trimmed();
    if (stream.status() != QTextStream::Ok) {
      throw ZIOException("Error while reading file.");
    }
    removeComment(line, QString("#"), true);
    QStringList fieldList = line.split(" ", QString::SkipEmptyParts);
    if (fieldList.size() >= 7) {
      SwcNode node;
      bool ok;
      node.id = fieldList[0].toInt(&ok);
      if (!ok || node.id < 1) {
        throw ZIOException(QString("Wrong SWC format: %1.").arg(line));
      }
      node.type = fieldList[1].toInt(&ok);
      if (!ok) {
        throw ZIOException(QString("Wrong SWC format: %1.").arg(line));
      }
      node.x = fieldList[2].toDouble(&ok);
      if (!ok) {
        throw ZIOException(QString("Wrong SWC format: %1.").arg(line));
      }
      node.y = fieldList[3].toDouble(&ok);
      if (!ok) {
        throw ZIOException(QString("Wrong SWC format: %1.").arg(line));
      }
      node.z = fieldList[4].toDouble(&ok);
      if (!ok) {
        throw ZIOException(QString("Wrong SWC format: %1.").arg(line));
      }
      node.radius = fieldList[5].toDouble(&ok);
      if (!ok) {
        throw ZIOException(QString("Wrong SWC format: %1.").arg(line));
      }
      node.parentID = fieldList[6].toInt(&ok);
      if (!ok) {
        throw ZIOException(QString("Wrong SWC format: %1.").arg(line));
      }
      nodeMap[node.id] = node;
    } else if (!line.isEmpty()) {
      throw ZIOException(QString("Wrong SWC format: %1.").arg(line));
    }
  }

  std::map<int, Iterator> itMap;
  std::map<int, SwcNode>::iterator tmp;
  while (!nodeMap.empty()) {
    std::map<int, SwcNode>::iterator it = nodeMap.begin();
    while (it != nodeMap.end()) {
      int parentID = it->second.parentID;
      std::map<int, Iterator>::const_iterator nodeIt = itMap.find(parentID);
      if (nodeIt != itMap.end()) {
        itMap[it->first] = appendChild(nodeIt->second, it->second);
        tmp = it;
        ++it;
        nodeMap.erase(tmp);
      } else if (nodeMap.find(parentID) == nodeMap.end()) {
        itMap[it->first] = appendRoot(it->second);
        tmp = it;
        ++it;
        nodeMap.erase(tmp);
      } else {
        ++it;
      }
    }
  }
  }
  catch (const ZException & e) {
    throw ZIOException(QString("Can not load swc %1: %2").arg(filename).arg(e.what()));
  }
}

void ZSwc::save(const QString &filename) const
{
  try {
  QFile qFile(filename);
  if (!qFile.open(QIODevice::WriteOnly | QIODevice::Text))
    throw ZIOException("Can not open file.");

  QTextStream out(&qFile);
  out << "#id type x y z radius parentID\n";
  if (out.status() != QTextStream::Ok) {
    throw ZIOException("Error while writing file.");
  }
  for (ConstBreadthFirstIterator it = beginBreadthFirst(); it != endBreadthFirst(); ++it) {
    out << QString("%1 %2 %3 %4 %5 %6 %7\n").arg(it->id).arg(it->type).arg(it->x).arg(it->y).arg(it->z)
           .arg(it->radius).arg(parentID(it));
    if (out.status() != QTextStream::Ok) {
      throw ZIOException("Error while writing file.");
    }
  }
  }
  catch (const ZException & e) {
    throw ZIOException(QString("Can not save swc %1: %2").arg(filename).arg(e.what()));
  }
}

void ZSwc::addLine(const std::vector<glm::dvec3> &line, double radius)
{
  if (line.empty())
    return;
  SwcNode root(0, 0, line[0].x, line[0].y, line[0].z, radius, -1);
  Iterator it = appendRoot(root);
  for (size_t i=1; i<line.size(); ++i) {
    SwcNode node(0, 0, line[i].x, line[i].y, line[i].z, radius, -1);
    it = appendChild(it, node);
  }
}

} // namespace nim
