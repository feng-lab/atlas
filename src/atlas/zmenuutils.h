#pragma once

#include <QAction>
#include <QMenu>

namespace nim {

inline void appendClonedMenuActions(QMenu& dst, const QMenu& src)
{
  for (QAction* action : src.actions()) {
    if (action == nullptr) {
      continue;
    }
    if (action->isSeparator()) {
      dst.addSeparator();
      continue;
    }
    if (const QMenu* sub = action->menu()) {
      auto* newSub = dst.addMenu(sub->title());
      appendClonedMenuActions(*newSub, *sub);
      continue;
    }
    dst.addAction(action);
  }
}

} // namespace nim
