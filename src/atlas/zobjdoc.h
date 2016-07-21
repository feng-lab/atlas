#ifndef ZOBJDOC_H
#define ZOBJDOC_H

#include "zdoc.h"
#include <QObject>
#include <QString>
#include <QMenu>
#include <QAction>
#include <QUndoStack>
#include "zsysteminfo.h"

namespace nim {

class ZObjDoc : public QObject
{
  Q_OBJECT
public:
  explicit ZObjDoc(ZDoc &doc);

  inline bool hasObj() const { return !objs().empty(); }
  inline bool hasObjWithID(size_t id) const { return m_doc.objsOfDoc(this).contains(id); }
  inline QList<size_t> objs() const { return m_doc.objsOfDoc(this); }
  inline QList<size_t> selectedObjs() const { return m_doc.selectedObjsOfDoc(this); }
  inline bool isObjSelected(size_t id) const { return m_doc.isObjSelected(id); }
  inline bool isObjVisible(size_t id) const { return m_doc.isObjVisible(id); }
  ZDoc& doc() { return m_doc; }

  // if not changed, do nothing and return true, otherwise save file to its path or use saveas
  virtual bool save(size_t id) = 0;
  virtual bool saveAs(size_t id) = 0;

  void showObjInGraphicalShell(size_t id);
  // popup a widget to let user choose one object, return 0 if user click cancel
  size_t chooseOneObjWithWidget(const QString &title = "", QWidget *parent = nullptr) const;

  virtual QString typeName() const = 0;
  virtual QString typePluralName() const = 0;
  virtual bool canReadFile(const QString &fileName) = 0;
  // return object id, if object already exist, return its old id, if can not load, return 0
  // return last id if more than one object loaded from file
  virtual size_t loadFile(const QString &fileName, QString &errorMsg) = 0;
  virtual size_t loadFile(const QJsonValue &jValue, QString &errorMsg) = 0;

  virtual QList<QAction*> loadFileActions() const = 0;
  virtual QMenu* processObjMenu() const { return nullptr; }

  // remove obj with id
  virtual void removeObj(size_t id) = 0;
  //
  virtual QString objName(size_t id) const = 0;
  QString objNameWithModifiedMarker(size_t id) const;
  QString objNameWithModifiedMarkerAndID(size_t id) const;
  virtual QString objPath(size_t id) const = 0;
  virtual bool objHasUnsavedChange(size_t id) const = 0;
  virtual QString objInfo(size_t id) const = 0;
  virtual QString objDetailedInfo(size_t id) const { static QString str; Q_UNUSED(id) return str; }
  virtual QString objTooltip(size_t id) const = 0;
  virtual QUndoStack* objUndoStack(size_t id) { Q_UNUSED(id) return nullptr; }
  virtual QJsonValue jsonValue(size_t id) const = 0;
  virtual bool isSameObj(const QJsonValue& v1, const QJsonValue& v2) const = 0;
  // make alias of obj (many id point to same actual object)
  virtual size_t makeAlias(size_t id) = 0;
  // has other id point to same object
  virtual bool isAlias(size_t id) const = 0;

  virtual QWidget* createObjEditWidget(size_t /*id*/) { return nullptr; }

  std::map<size_t,size_t> read(const QList<QPair<QString,QJsonValue>> &docKeyValueList, QString &err);
  void write(QJsonObject &json) const;

  // show/hide obj with id
  void setObjVisible(size_t id, bool v) { emit objVisibleChanged(id, v); }
  //
  void sendObjSelectionChangedFromDocSignal(const QList<size_t> &selected, const QList<size_t> &deselected)
  { emit selectionChangedFromDoc(selected, deselected); }

  QString lastOpenedObjPath();
  void setLastOpenedObjPath(const QString &path);

signals:
  void objAdded(size_t id, ZObjDoc* doc);
  void objAboutToBeRemoved(size_t id, ZObjDoc* doc);
  void objRemoved(size_t id, ZObjDoc* doc);
  void allObjsRemoved(ZObjDoc* doc);
  void objVisibleChanged(size_t id, bool v);
  void selectionChangedFromDoc(const QList<size_t> &selected, const QList<size_t> &deselected);

protected:
  QString strippedName(const QString &fullFileName);

protected:
  ZDoc& m_doc;
};

} // namespace nim

#endif // ZOBJDOC_H
