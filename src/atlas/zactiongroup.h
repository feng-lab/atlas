#ifndef ZACTIONGROUP_H
#define ZACTIONGROUP_H

#include <QObject>
#include <QAction>
#include <QPointer>

namespace nim {

// similar to exclusive QActionGroup, but allow uncheck
class ZActionGroup : public QObject
{
  Q_OBJECT
public:
  explicit ZActionGroup(QObject *parent = nullptr);

  QAction *addAction(QAction* action);
  QAction *addAction(const QString &text);
  QAction *addAction(const QIcon &icon, const QString &text);
  void removeAction(QAction *action);
  QList<QAction*> actions() const { return m_actions; }

  // can be nullptr
  QAction *checkedAction() const { return m_current; }
  bool isEnabled() const { return m_enabled; }
  bool isVisible() const { return m_visible; }

public slots:
  void setEnabled(bool v);
  void setVisible(bool v);

signals:
  void toggled(QAction *, bool);
  void triggered(QAction *);
  void hovered(QAction *);

private slots:
  void actionChanged();
  void actionToggled(bool checked);
  void actionTriggered();
  void actionHovered();

private:
  QList<QAction*> m_actions;
  QPointer<QAction> m_current;
  bool m_enabled;
  bool m_visible;
};

} // namespace nim

#endif // ZACTIONGROUP_H
