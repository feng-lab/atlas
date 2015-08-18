#include "zactiongroup.h"

#include <cassert>

namespace nim {

ZActionGroup::ZActionGroup(QObject *parent)
  : QObject(parent)
  , m_enabled(true)
  , m_visible(true)
{
}

QAction *ZActionGroup::addAction(QAction *action)
{
  if (!m_actions.contains(action)) {
    m_actions.append(action);
    connect(action, SIGNAL(toggled(bool)), this, SLOT(actionToggled(bool)));
    connect(action, SIGNAL(triggered()), this, SLOT(actionTriggered()));
    connect(action, SIGNAL(changed()), this, SLOT(actionChanged()));
    connect(action, SIGNAL(hovered()), this, SLOT(actionHovered()));
  }
  action->setEnabled(m_enabled);
  action->setVisible(m_visible);
  if (action->isChecked())
    m_current = action;
  return action;
}

QAction *ZActionGroup::addAction(const QString &text)
{
  return new QAction(text, this);
}

QAction *ZActionGroup::addAction(const QIcon &icon, const QString &text)
{
  return new QAction(icon, text, this);
}

void ZActionGroup::removeAction(QAction *action)
{
  if (m_actions.removeAll(action)) {
    if (action == m_current)
      m_current = 0;
    disconnect(action, SIGNAL(triggered()), this, SLOT(actionTriggered()));
    disconnect(action, SIGNAL(changed()), this, SLOT(actionChanged()));
    disconnect(action, SIGNAL(hovered()), this, SLOT(actionHovered()));
  }
}

void ZActionGroup::setEnabled(bool v)
{
  if (m_enabled == v)
    return;
  m_enabled = v;
  for (int i=0; i<m_actions.size(); ++i)
    m_actions[i]->setEnabled(v);
}

void ZActionGroup::setVisible(bool v)
{
  if (m_visible == v)
    return;
  m_visible = v;
  for (int i=0; i<m_actions.size(); ++i)
    m_actions[i]->setVisible(v);
}

void ZActionGroup::actionChanged()
{
  QAction *action = qobject_cast<QAction*>(sender());
  assert(action);
  if (action->isChecked()) {
    if (action != m_current) {
      if (m_current)
        m_current->setChecked(false);
      m_current = action;
    }
  } else if (action == m_current) {
    m_current = 0;
  }
}

void ZActionGroup::actionToggled(bool checked)
{
  QAction *action = qobject_cast<QAction*>(sender());
  assert(action);
  emit toggled(action, checked);
}

void ZActionGroup::actionTriggered()
{
  QAction *action = qobject_cast<QAction*>(sender());
  assert(action);
  emit triggered(action);
}

void ZActionGroup::actionHovered()
{
  QAction *action = qobject_cast<QAction*>(sender());
  assert(action);
  emit hovered(action);
}

} // namespace nim
