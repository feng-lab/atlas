#ifndef ZWIDGETSGROUP_H
#define ZWIDGETSGROUP_H

#include <QObject>
#include <QLayout>

namespace nim {

class ZParameter;

class ZWidgetsGroup : public QObject
{
  Q_OBJECT
public:
  enum class Type {
    Group, Widget, Parameter
  };

  explicit ZWidgetsGroup(QWidget* widget, ZWidgetsGroup* parentGroup, int visibleLevel, QObject *parent = 0);
  explicit ZWidgetsGroup(const QString &groupName, ZWidgetsGroup* parentGroup, int visibleLevel, QObject *parent = 0);
  explicit ZWidgetsGroup(ZParameter *parameter, ZWidgetsGroup* parentGroup, int visibleLevel, QObject *parent = 0);
  virtual ~ZWidgetsGroup();

  inline bool isGroup() const {return m_type == Type::Group;}
  inline QString getGroupName() const {return m_groupName;}
  inline void setGroupName(const QString &name) { m_groupName = name; }
  const QList<ZWidgetsGroup*>& getChildGroups();

  inline void setUseToolBoxStyle(bool v) { m_useToolBoxStyle = v; }

  QList<ZParameter*> getParameterList();

  void addChildGroup(ZWidgetsGroup *child, bool atEnd = true);
  void deleteAllChildGroups();
  void deleteChildParamter(const ZParameter *para);

  void mergeGroup(ZWidgetsGroup* other, bool atEnd = true);

  inline int getVisibleLevel() const {return m_visibleLevel;}
  inline void setVisibleLevel(int v) {m_visibleLevel=v;}
  inline void setBasicAdvancedCutoff(int v) {m_cutOffbetweenBasicAndAdvancedLevel=v;}
  inline int getBasicAdvancedCutoff() const {return m_cutOffbetweenBasicAndAdvancedLevel;}
  void setVisible(bool visible);
  inline bool isVisible() { return m_isVisible; }

  QWidget *createWidget(bool createBasic = true, bool scroll = true, const QString &label = QString());
  QLayout *createLayout(bool createBasic = true);

  bool operator<(const ZWidgetsGroup& other) const;

protected:
  void removeChildGroup(ZWidgetsGroup* child);
  
signals:
  void requestAdvancedWidget(const QString &name);
  void widgetsGroupChanged();
  
public slots:
  void emitRequestAdvancedWidgetSignal();
  void emitWidgetsGroupChangedSignal();

private:
  void sortChildGroups();
  
private:
  Type m_type;
  QString m_groupName;
  QWidget *m_widget;
  ZParameter *m_parameter;
  ZWidgetsGroup* m_parent;
  int m_visibleLevel;
  bool m_isSorted;
  int m_cutOffbetweenBasicAndAdvancedLevel;
  QList<ZWidgetsGroup*> m_childGroups;
  bool m_isVisible;

  bool m_useToolBoxStyle;
};

} // namespace nim

#endif // ZWIDGETSGROUP_H
