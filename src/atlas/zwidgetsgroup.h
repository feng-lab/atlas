#pragma once

#include <QObject>
#include <QLayout>
#include <memory>

class QLabel;

namespace nim {

class ZParameter;

class ZWidgetsGroup : public QObject
{
  Q_OBJECT

public:
  enum class Type
  {
    Group,
    Widget,
    Parameter
  };

  explicit ZWidgetsGroup(QWidget& widget, int visibleLevel);

  explicit ZWidgetsGroup(QString groupName, int visibleLevel);

  explicit ZWidgetsGroup(ZParameter& parameter, int visibleLevel);

  bool isGroup() const
  {
    return m_type == Type::Group;
  }

  QString getGroupName() const
  {
    return m_groupName;
  }

  void setGroupName(const QString& name)
  {
    m_groupName = name;
  }

  std::vector<ZParameter*> getParameterList();

  void addChild(QWidget& widget, int visibleLevel);

  void addChild(ZParameter& parameter, int visibleLevel);

  void addChild(const std::shared_ptr<ZWidgetsGroup>& child, bool atEnd = true);

  void removeAllChildren();

  void removeChild(const QWidget& widget);

  void removeChild(const ZParameter& para);

  void removeChild(const std::shared_ptr<ZWidgetsGroup>& child);

  void protectWidgetChildren();

  int visibleLevel() const
  {
    return m_visibleLevel;
  }

  void setVisibleLevel(int v)
  {
    m_visibleLevel = v;
  }

  void setBasicAdvancedCutoff(int v)
  {
    m_cutOffbetweenBasicAndAdvancedLevel = v;
  }

  int basicAdvancedCutoff() const
  {
    return m_cutOffbetweenBasicAndAdvancedLevel;
  }

  void setVisible(bool visible)
  {
    m_isVisible = visible;
  }

  bool isVisible()
  {
    return m_isVisible;
  }

  QWidget* createWidget(bool createBasic = true, bool scroll = true, QLabel* label = nullptr, bool noStretch = false);

  QLayout* createLayout(bool createBasic = true);

  bool operator<(const ZWidgetsGroup& other) const
  {
    return m_visibleLevel < other.m_visibleLevel;
  }

  void emitRequestAdvancedWidgetSignal();

  void emitWidgetsGroupChangedSignal();

Q_SIGNALS:
  void requestAdvancedWidget(const QString& name);

  void widgetsGroupChanged();

private:
  void sortChildGroups();

private:
  Type m_type;
  QString m_groupName;
  QWidget* m_widget = nullptr;
  ZParameter* m_parameter = nullptr;
  int m_visibleLevel;
  bool m_isSorted = false;
  int m_cutOffbetweenBasicAndAdvancedLevel = 1;
  std::vector<std::shared_ptr<ZWidgetsGroup>> m_childGroups;
  bool m_isVisible = true;
};

} // namespace nim
