#pragma once

#include "zglobal.h"
#include "zdoc.h"
#include "zjson.h"
#include "zsysteminfo.h"
#include <QObject>
#include <QString>
#include <QMenu>
#include <QAction>
#include <QUndoStack>
#include <memory>

namespace nim {

class ZObjDoc : public QObject
{
Q_OBJECT
public:
  explicit ZObjDoc(ZDoc& doc);

  [[nodiscard]] inline bool hasObj() const
  { return !objs().empty(); }

  [[nodiscard]] inline bool hasObjWithID(size_t id) const
  { return contains(m_doc.objsOfDoc(this), id); }

  [[nodiscard]] inline std::vector<size_t> objs() const
  { return m_doc.objsOfDoc(this); }

  [[nodiscard]] inline std::vector<size_t> selectedObjs() const
  { return m_doc.selectedObjsOfDoc(this); }

  [[nodiscard]] inline bool isObjSelected(size_t id) const
  { return m_doc.isObjSelected(id); }

  [[nodiscard]] inline bool isObjVisible(size_t id) const
  { return m_doc.isObjVisible(id); }

  ZDoc& doc()
  { return m_doc; }

  // if not changed, do nothing and return true, otherwise save file to its path or use saveas
  virtual bool save(size_t id) = 0;

  virtual bool saveAs(size_t id) = 0;

  void showObjInGraphicalShell(size_t id) const;

  // popup a widget to let user choose one object, return 0 if user click cancel
  size_t chooseOneObjWithWidget(const QString& title = "", QWidget* parent = nullptr) const;

  [[nodiscard]] virtual QString typeName() const = 0;

  [[nodiscard]] virtual QString typePluralName() const = 0;

  [[nodiscard]] virtual bool canReadFile(const QString& fileName) const = 0;

  // return object id, if object already exist, return its old id, if can not load, return 0
  // return last id if more than one object loaded from file
  virtual size_t loadFile(const QString& fileName, QString& errorMsg) = 0;

  virtual size_t loadFile(const json::value& jValue, QString& errorMsg) = 0;

  [[nodiscard]] virtual std::vector<QAction*> loadFileActions() const = 0;

  [[nodiscard]] virtual QMenu* processObjMenu() const
  { return nullptr; }

  // remove obj with id
  virtual void removeObj(size_t id) = 0;

  //
  [[nodiscard]] virtual QString objName(size_t id) const = 0;

  [[nodiscard]] QString objNameWithModifiedMarker(size_t id) const;

  [[nodiscard]] QString objNameWithModifiedMarkerAndID(size_t id) const;

  [[nodiscard]] virtual QString objPath(size_t id) const = 0;

  [[nodiscard]] virtual bool objHasUnsavedChange(size_t id) const = 0;

  [[nodiscard]] virtual QString objInfo(size_t id) const = 0;

  [[nodiscard]] virtual QString objDetailedInfo(size_t id) const
  {
    Q_UNUSED(id)
    static QString str;
    return str;
  }

  [[nodiscard]] virtual QString objTooltip(size_t id) const = 0;

  inline QUndoStack* objUndoStack(size_t id)
  {
    return const_cast<QUndoStack*>(const_cast<const ZObjDoc*>(this)->objUndoStack(id));
  }

  [[nodiscard]] virtual const QUndoStack* objUndoStack(size_t id) const
  {
    Q_UNUSED(id)
    return nullptr;
  }

  [[nodiscard]] virtual json::value jsonValue(size_t id) const = 0;

  [[nodiscard]] virtual bool isSameObj(const json::value& v1, const json::value& v2) const = 0;

  // make alias of obj (many id point to same actual object)
  virtual size_t makeAlias(size_t id) = 0;

  // has other id point to same object
  [[nodiscard]] virtual bool isAlias(size_t id) const = 0;

  virtual QWidget* createObjEditWidget(size_t /*id*/)
  { return nullptr; }

  std::map<size_t, size_t> read(const std::vector<std::pair<QString, json::value>>& docKeyValueList, QString& err);

  void write(json::object& json) const;

//  // show/hide obj with id
//  void setObjVisible(size_t id, bool v)
//  { emit objVisibleChanged(id, v); }
//
//  // lock/unlock obj from editing with id
//  void setObjLocked(size_t id, bool v)
//  { emit objLockedChanged(id, v); }

  //
  void sendObjSelectionChangedFromDocSignal(const std::vector<size_t>& selected, const std::vector<size_t>& deselected)
  { emit selectionChangedFromDoc(selected, deselected); }

  [[nodiscard]] QString lastOpenedObjPath() const;

  void setLastOpenedObjPath(const QString& path) const;

signals:

  void objAdded(size_t id, ZObjDoc* doc);

  void objAboutToBeRemoved(size_t id, ZObjDoc* doc);

  void objRemoved(size_t id, ZObjDoc* doc);

  void objVisibleChanged(size_t id, bool v);

  void selectionChangedFromDoc(const std::vector<size_t>& selected, const std::vector<size_t>& deselected);

protected:
  static QString strippedName(const QString& fullFileName);

protected:
  ZDoc& m_doc;
};

} // namespace nim

