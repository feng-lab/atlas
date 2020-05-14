#pragma once

#include <QObject>
#include <vector>
#include <memory>

namespace nim {

class ZObjDoc;

class ZObjPack : public QObject
{
  Q_OBJECT
public:
  ZObjPack(size_t id, ZObjDoc* objDoc, QObject* parent = nullptr);

  void setVisible(bool v);

  void setLocked(bool v);

  [[nodiscard]] bool isLocked() const
  { return m_locked; }

signals:

  void visibleStateChanged(bool v);

  void lockedStateChanged(bool v);

protected:
  size_t m_id = 0;
  ZObjDoc* m_objDoc = nullptr;
  bool m_locked = false;
  bool m_show = true;

private:
  friend class ZObjModel;

  [[nodiscard]] int row() const;

  ZObjPack* parent = nullptr;
  std::vector<std::shared_ptr<ZObjPack>> children;
};

} // namespace nim


