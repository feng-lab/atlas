#include "zobjpack.h"

#include "zlog.h"
#include "zobjdoc.h"

namespace nim {

ZObjPack::ZObjPack(size_t id, ZObjDoc* objDoc, QObject* parent)
  : QObject(parent)
  , m_id(id)
  , m_objDoc(objDoc)
{}

void ZObjPack::setVisible(bool v)
{
  if (m_show != v) {
    m_show = v;
    Q_EMIT visibleStateChanged(m_show);
    if (m_objDoc) {
      Q_EMIT m_objDoc->objVisibleChanged(m_id, m_show);
    }
  }
}

void ZObjPack::setLocked(bool v)
{
  if (m_locked != v) {
    m_locked = v;
    Q_EMIT lockedStateChanged(m_locked);
  }
}

int ZObjPack::row() const
{
  if (parent) {
    for (size_t i = 0; i < parent->children.size(); ++i) {
      if (parent->children[i].get() == this) {
        return i;
      }
    }
  }
  return 0;
}

} // namespace nim
