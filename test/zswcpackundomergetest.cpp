#include <gtest/gtest.h>

#include <QApplication>

#include <memory>
#include <set>

#include "zdoc.h"
#include "zswcdoc.h"
#include "zswcpack.h"

namespace {

class ScopedQtWidgetsApplication
{
public:
  ScopedQtWidgetsApplication()
  {
    if (QApplication::instance() != nullptr) {
      return;
    }

#if defined(__linux__)
    static int argc = 3;
    static char arg0[] = "zswcpackundomergetest";
    static char arg1[] = "-platform";
    static char arg2[] = "offscreen";
    static char* argv[] = {arg0, arg1, arg2, nullptr};
    _app = std::make_unique<QApplication>(argc, argv);
#else
    static int argc = 1;
    static char arg0[] = "zswcpackundomergetest";
    static char* argv[] = {arg0, nullptr};
    _app = std::make_unique<QApplication>(argc, argv);
#endif
  }

private:
  std::unique_ptr<QApplication> _app;
};

class TestableSwcPack : public nim::ZSwcPack
{
public:
  using nim::ZSwcPack::ZSwcPack;

  nim::ZSwc& swcMutable()
  {
    return m_swc;
  }
};

[[nodiscard]] nim::ZSwc makeSimpleTwoNodeSwc()
{
  nim::ZSwc swc;

  nim::SwcNode root;
  root.id = 1;
  root.type = 0;
  root.x = 0.0;
  root.y = 0.0;
  root.z = 0.0;
  root.radius = 1.0;
  root.parentID = -1;
  swc.appendRoot(root);

  nim::SwcNode child;
  child.id = 2;
  child.type = 0;
  child.x = 1.0;
  child.y = 0.0;
  child.z = 0.0;
  child.radius = 1.0;
  child.parentID = 1;
  const auto rootIt = swc.begin();
  swc.appendChild(rootIt, child);

  return swc;
}

[[nodiscard]] nim::ZSwc::SwcTreeNode findNodeById(nim::ZSwc& swc, int64_t id)
{
  for (auto it = swc.begin(); it != swc.end(); ++it) {
    if (it->id == id) {
      return it;
    }
  }
  return nim::ZSwc::SwcTreeNode{};
}

} // namespace

TEST(ZSwcPackUndoMerge, MoveSelectedNodes_MergesWithSameSelection)
{
  ScopedQtWidgetsApplication qtApp;

  nim::ZDoc doc;
  nim::ZSwcDoc swcDoc(doc);

  TestableSwcPack pack(makeSimpleTwoNodeSwc(), QString(), /*id=*/1, swcDoc);

  const int64_t rootId = 1;
  const int64_t childId = 2;
  ASSERT_FALSE(nim::ZSwc::isNull(findNodeById(pack.swcMutable(), rootId)));
  ASSERT_FALSE(nim::ZSwc::isNull(findNodeById(pack.swcMutable(), childId)));

  {
    std::set<nim::ZSwc::SwcTreeNode> sel;
    sel.insert(findNodeById(pack.swcMutable(), childId));
    pack.setSelectedNodes(sel);
  }

  pack.translateSelectedNodesLegacyLike(10.0, 0.0, 0.0);
  ASSERT_EQ(pack.undoStack()->count(), 1);
  EXPECT_DOUBLE_EQ(findNodeById(pack.swcMutable(), childId)->x, 11.0);

  pack.translateSelectedNodesLegacyLike(5.0, 0.0, 0.0);
  EXPECT_EQ(pack.undoStack()->count(), 1) << "Move commands should merge when selection is unchanged.";
  EXPECT_DOUBLE_EQ(findNodeById(pack.swcMutable(), childId)->x, 16.0);

  {
    std::set<nim::ZSwc::SwcTreeNode> sel;
    sel.insert(findNodeById(pack.swcMutable(), rootId));
    pack.setSelectedNodes(sel);
  }

  pack.translateSelectedNodesLegacyLike(1.0, 0.0, 0.0);
  EXPECT_EQ(pack.undoStack()->count(), 2) << "Selection change should prevent move-command merge.";
  EXPECT_DOUBLE_EQ(findNodeById(pack.swcMutable(), rootId)->x, 1.0);

  pack.undoStack()->undo();
  EXPECT_DOUBLE_EQ(findNodeById(pack.swcMutable(), rootId)->x, 0.0);
  EXPECT_DOUBLE_EQ(findNodeById(pack.swcMutable(), childId)->x, 16.0);

  pack.undoStack()->undo();
  EXPECT_DOUBLE_EQ(findNodeById(pack.swcMutable(), rootId)->x, 0.0);
  EXPECT_DOUBLE_EQ(findNodeById(pack.swcMutable(), childId)->x, 1.0);

  pack.undoStack()->redo();
  pack.undoStack()->redo();
  EXPECT_DOUBLE_EQ(findNodeById(pack.swcMutable(), rootId)->x, 1.0);
  EXPECT_DOUBLE_EQ(findNodeById(pack.swcMutable(), childId)->x, 16.0);
}

TEST(ZSwcPackContextMenu, DocActionsMatchNeuTubeOrderAndLabels)
{
  ScopedQtWidgetsApplication qtApp;

  nim::ZDoc doc;
  nim::ZSwcDoc swcDoc(doc);
  TestableSwcPack pack(makeSimpleTwoNodeSwc(), QString(), /*id=*/1, swcDoc);

  QMenu& menu = pack.contextMenu();
  const auto actions = menu.actions();

  std::vector<QString> topLevel;
  topLevel.reserve(actions.size());
  for (QAction* act : actions) {
    if (act == nullptr) {
      continue;
    }
    if (act->isSeparator()) {
      continue;
    }
    if (act->menu() != nullptr) {
      topLevel.push_back(act->menu()->title());
    } else {
      topLevel.push_back(act->text());
    }
  }

  const std::vector<QString> expectedTopLevel = {
    QStringLiteral("Delete"),
    QStringLiteral("Delete Unselected"),
    QStringLiteral("Break"),
    QStringLiteral("Connect"),
    QStringLiteral("Merge"),
    QStringLiteral("Insert"),
    QStringLiteral("Interpolate"),
    QStringLiteral("Select"),
    QStringLiteral("Advanced Editing"),
    QStringLiteral("Change Property"),
    QStringLiteral("Information"),
  };
  EXPECT_EQ(topLevel, expectedTopLevel);

  auto* interp = actions[6]->menu();
  ASSERT_NE(interp, nullptr);
  EXPECT_EQ(interp->title(), QStringLiteral("Interpolate"));
  ASSERT_EQ(interp->actions().size(), 4);
  EXPECT_EQ(interp->actions().at(0)->text(), QStringLiteral("Position and Radius"));
  EXPECT_EQ(interp->actions().at(1)->text(), QStringLiteral("Z"));
  EXPECT_EQ(interp->actions().at(2)->text(), QStringLiteral("Position"));
  EXPECT_EQ(interp->actions().at(3)->text(), QStringLiteral("Radius"));
}
