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

  explicit ZWidgetsGroup(QWidget &widget, int visibleLevel);
  explicit ZWidgetsGroup(const QString &groupName, int visibleLevel);
  explicit ZWidgetsGroup(ZParameter &parameter, int visibleLevel);

  inline bool isGroup() const {return m_type == Type::Group;}
  inline QString getGroupName() const {return m_groupName;}
  inline void setGroupName(const QString &name) { m_groupName = name; }

  inline void setUseToolBoxStyle(bool v) { m_useToolBoxStyle = v; }

  std::vector<ZParameter*> getParameterList();

  void addChild(QWidget &widget, int visibleLevel);
  void addChild(ZParameter &parameter, int visibleLevel);
  void addChild(std::shared_ptr<ZWidgetsGroup> child, bool atEnd = true);
  void removeAllChildren();
  void removeChild(const ZParameter &para);
  void removeChild(const std::shared_ptr<ZWidgetsGroup> &child);

  inline int visibleLevel() const {return m_visibleLevel;}
  inline void setVisibleLevel(int v) {m_visibleLevel=v;}
  inline void setBasicAdvancedCutoff(int v) {m_cutOffbetweenBasicAndAdvancedLevel=v;}
  inline int basicAdvancedCutoff() const {return m_cutOffbetweenBasicAndAdvancedLevel;}
  void setVisible(bool visible) { m_isVisible = visible; }
  inline bool isVisible() { return m_isVisible; }

  QWidget *createWidget(bool createBasic = true, bool scroll = true, const QString &label = QString());
  QLayout *createLayout(bool createBasic = true);

  bool operator<(const ZWidgetsGroup& other) const;

protected:
  
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
  int m_visibleLevel;
  bool m_isSorted;
  int m_cutOffbetweenBasicAndAdvancedLevel;
  std::vector<std::shared_ptr<ZWidgetsGroup>> m_childGroups;
  bool m_isVisible;

  bool m_useToolBoxStyle;
};

} // namespace nim

#endif // ZWIDGETSGROUP_H
