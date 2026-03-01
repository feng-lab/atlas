# neuTube → Atlas SWC Node Context Menu Parity

This document maps neuTube’s **SWC node** context menus (2D + 3D) to Atlas and tracks migration parity.

Scope:
- Right-click context menu shown when interacting with **SWC nodes**.
- neuTube reference: `~/code/neutu/neurolabi/gui` (not the `src/neurolabi` folder inside this repo).

Non-goals (for this doc):
- Non-node SWC menus (tree/forest menus).
- Non-SWC menus (image/process/body/puncta).

## neuTube 2D SWC node context menu (ZStackPresenter)

Source:
- Menu composition: `neurolabi/gui/mvc/zstackpresenter.cpp` (`ZStackPresenter::createSwcNodeContextMenu`)
- Presenter (view) actions: `neurolabi/gui/zmenufactory.cpp` (`ZMenuFactory::makeSwcNodeContextMenu(ZStackPresenter*, ...)`)
- Doc actions/submenus: `neurolabi/gui/zmenufactory.cpp` (`ZMenuFactory::makeSwcNodeContextMenu(ZStackDoc*, ...)`)
- Labels/shortcuts: `neurolabi/gui/zactionfactory.cpp`

Order (top → bottom):
1) Extend (Space)
2) Connect to (C)
3) Move to Current Plane (F)
4) Move Selected (Shift+Mouse) (V)
5) Estimate Radius
6) Delete (X)
7) Delete Unselected
8) Break (B)
9) Connect (C)
10) Merge
11) Insert (I)
12) Interpolate >
    - Position and Radius
    - Z
    - Position
    - Radius
13) Select >
    - Downstream
    - Upstream
    - Neighbors
    - Host branch
    - All connected nodes
    - All nodes (Ctrl+A)
14) Advanced Editing >
    - Remove turn
    - Resolve crossover
    - Join isolated branch
    - Join isolated brach (across trees)
    - Reset branch point
15) Change Property >
    - Translate
    - Change size
    - Set as a root
16) Information >
    - Summary
    - Path length
    - Scaled Path length
17) Separator
18) Add Neuron Node (G)
19) Locate node(s) in 3D

Notes:
- Items 1–5 are **interaction-mode entry** actions in neuTube (they switch the presenter into an SWC edit mode). They are
  not checkable in the menu; neuTube shows a status message and exits the mode on right-click.

### 2D view-action semantics (neuTube reference)

These are not just “commands” in neuTube: they enter an interactive mode managed by `ZInteractiveContext`
and then react to subsequent clicks/drags.

- **Extend** (`ACTION_EXTEND_SWC_NODE`, `ZStackPresenter::enterSwcExtendMode()`):
  - Enters `ZInteractiveContext::SWC_EDIT_EXTEND`.
  - Status tip (neuTube): “Left click to extend. Path calculation is off when 'CTRL' is pressed. Right click to exit extending mode.”
  - Next **left-click release**:
    - If `Ctrl` held: `OP_SWC_EXTEND` → `ZStackDoc::executeSwcNodeExtendCommand(center, radius)` (plain extend).
    - Else, if click hits an SWC node: selection changes (no extend).
    - Else: `OP_SWC_SMART_EXTEND` → `ZStackDoc::executeSwcNodeSmartExtendCommand(center, radius)` (path computation).
- **Connect to** (`ACTION_CONNECT_TO_SWC_NODE`, `ZStackPresenter::enterSwcConnectMode()`):
  - Enters `ZInteractiveContext::SWC_EDIT_CONNECT`.
  - Next left-click release: `OP_SWC_CONNECT_TO` → `ZStackDoc::executeConnectSwcNodeCommand(prevNode, targetNode)`,
    then exits SWC edit mode.
- **Move to Current Plane** (`ACTION_CHANGE_SWC_NODE_FOCUS`, `ZStackPresenter::changeSelectedSwcNodeFocus()`):
  - Immediate command: `ZStackDoc::executeSwcNodeChangeZCommand(currentSliceZ)`.
- **Move Selected (Shift+Mouse)** (`ACTION_MOVE_SWC_NODE`, `ZStackPresenter::enterSwcMoveMode()`):
  - Enters `ZInteractiveContext::SWC_EDIT_MOVE_NODE`.
  - User holds `Shift` and drags with left button: mapper emits `OP_MOVE_OBJECT` while in `INTERACT_SWC_MOVE_NODE`.
- **Estimate Radius** (`ACTION_ESTIMATE_SWC_NODE_RADIUS`, `ZStackPresenter::estimateSelectedSwcRadius()`):
  - Immediate command: `ZStackDoc::executeSwcNodeEstimateRadiusCommand()`.

### Atlas implementation pointers (current)

These pointers are here so reviewers can quickly find the current Atlas ports and compare with the neuTube reference
above:

- 2D node context menu composition: `src/atlas/zswcfilter.cpp` (`popupSwcNodeContextMenu`).
- 2D SWC edit modes (Extend/Connect-to/Add-node/Move-selected): `src/atlas/zgraphicsscene.cpp`.
- 2D SWC pack edit helpers (undoable): `src/atlas/zswcpack.cpp`.
- Shared doc-level SWC submenus (Delete/Break/Connect/Merge/Insert/Interpolate/Select/Advanced/Change Property/Information):
  `src/atlas/zswcpack.cpp` (`createContextMenu`).
- 3D node context menu composition (UI thread): `src/atlas/z3dcanvas.cpp` (`Z3DCanvas::showSwcNodeContextMenu`).
- 3D SWC interaction modes (render thread): `src/atlas/z3dswcfilter.cpp` (`selectSwc`, `contextMenuEvent`).
- 3D view→UI forwarding: `src/atlas/z3dswcview.cpp`, `src/atlas/z3drenderingengine.cpp`.

## neuTube 3D SWC node context menu (Z3DWindow)

Source:
- Menu composition: `neurolabi/gui/z3dwindow.cpp` (`Z3DWindow::createContextMenu`)
- Doc actions/submenus: `neurolabi/gui/zstackdocmenufactory.cpp` (`ZStackDocMenuFactory::makeSwcNodeContextMenu`)
- Labels/shortcuts: `neurolabi/gui/zactionfactory.cpp`

Order (top → bottom):
1) Extend (Space) (toggle)
2) Connect to (C)
3) Move Selected (Shift+Mouse) (V) (toggle)
4) Doc actions/submenus (same as 2D doc section, items 6–16 above)
5) Separator
6) Locate node(s) in 2D
7) Change type
8) Add neuron node (toggle)

Notes:
- Items 1, 3, and 8 are **toggle** actions that represent active interaction modes.
- `Tree` action is added in code but hidden by `customizeContextMenu()` in the default app; it does not appear in typical UX.
- 3D menu does **not** include “Move to Current Plane” or “Estimate Radius” (2D-only).
- neuTube 3D builds the doc submenu title as **"Intepolate"** (typo) in `ZStackDocMenuFactory`; 2D uses **"Interpolate"**.

### 3D view-action semantics (neuTube reference)

- **Extend** (toggle, `Z3DWindow::toogleSmartExtendSelectedSwcNodeMode(bool)`):
  - Sets `Z3DSwcFilter::EInteractionMode` to `SmartExtendSwcNode` when stack data exists, otherwise `PlainExtendSwcNode`.
  - Sets canvas `ZInteractiveContext::SWC_EDIT_SMART_EXTEND`.
  - Mutually exclusive with “Add neuron node” mode (turns it off when enabled).
  - Actual execution is driven by a **volume click** callback (`Z3DWindow::pointInVolumeLeftClicked(...)`):
    - Preconditions (legacy): `hasVolume()`, `channelNumber() == 1`, extend toggle checked, and exactly one SWC node selected.
    - If `Ctrl` held: `ZStackDoc::executeSwcNodeExtendCommand(clickPos)`.
    - Else: `ZStackDoc::executeSwcNodeSmartExtendCommand(clickPos)`.
- **Connect to** (`Z3DWindow::startConnectingSwcNode()`):
  - Sets `Z3DSwcFilter::EInteractionMode::ConnectSwcNode`, `SWC_EDIT_CONNECT`.
  - Next click on target node triggers `Z3DWindow::connectSwcTreeNode(tn)` which runs
    `ZStackDoc::executeConnectSwcNodeCommand(closestSelectedNode, tn)` and exits mode.
- **Move Selected** (toggle, `Z3DWindow::toogleMoveSelectedObjectsMode(bool)`):
  - Delegates to the interaction handler (`setMoveObjects(checked)`); used with `Shift+Mouse`.
  - The interaction handler emits `objectsMoved(dx, dy, dz)` continuously while dragging; neuTube pushes
    `ZStackDocCommand::ObjectEdit::MoveSelected` and relies on `mergeWith()` to coalesce the drag into a single undo item.
- **Add neuron node** (toggle, `Z3DWindow::toogleAddSwcNodeMode(bool)`):
  - Sets `Z3DSwcFilter::EInteractionMode::AddSwcNode`, `SWC_EDIT_ADD_NODE`.
  - Mutually exclusive with “Extend” (turns it off when enabled).
  - On background click (hit nothing) with node picking enabled, neuTube picks a nearby SWC node to define a depth,
    projects that node position onto the click ray, and adds a new SWC node at the projected position with the
    picked node's radius (`Z3DSwcFilter::selectSwc` emits `addNewSwcTreeNode(x, y, z, r)`).

### 3D "Change type" dialog (neuTube reference)

Source:
- Dialog: `neurolabi/gui/dialogs/swctypedialog.*` (`SwcTypeDialog`)
- 3D invocation + apply logic: `neurolabi/gui/z3dwindow.cpp` (`Z3DWindow::changeSelectedSwcNodeType`)

UI elements:
- Title: "Change Swc Type"
- Type: `QSpinBox` (`0..65535`)
- Picking modes (radio buttons):
  - Individual (default)
  - Downstream
  - Connection
  - Main trunk
  - Trunk level
  - Branch level
  - Traffic
  - Longest leaf
  - Furthest leaf
  - Root
  - Subtree

Visibility rules:
- When invoked with `ZSwcTree::SWC_NODE`:
  - **Shown**: Individual, Downstream, Connection, Branch level, Longest leaf, Furthest leaf
  - **Hidden**: Main trunk, Traffic, Trunk level, Root, Subtree
- When invoked with `ZSwcTree::WHOLE_TREE`:
  - **Shown**: Individual, Main trunk, Traffic, Trunk level, Root, Subtree
  - **Hidden**: Connection, Downstream

Apply semantics used by the 3D SWC-node context menu:
- `INDIVIDUAL`: set selected node(s) type to `dlg.type()`
- `DOWNSTREAM`: set downstream type to `dlg.type()`
- `CONNECTION`: set upstream type to `dlg.type()` until common ancestor
- `LONGEST_LEAF`: set path type from selected node to furthest node (geodesic)
- Other dialog modes may be visible in the dialog but are not handled in `Z3DWindow::changeSelectedSwcNodeType()`.

## Atlas parity checklist

Legend:
- ✅ implemented (behavior + UI parity)
- 🟡 implemented but not parity
- ❌ missing

### Shared doc-level SWC actions
- Delete: ✅ (ported legacy semantics: children become new roots)
- Delete Unselected: ✅ (ported legacy semantics: children become new roots)
- Break: ✅ (ported legacy semantics: selected-child links detach to master-root/forest root)
- Connect: ✅ (ported legacy ZSwcConnector MST + minDist semantics)
- Merge: ✅ (ported legacy MergeSwcNode semantics)
- Insert: ✅ (ported legacy insert-between-adjacent-selected semantics)
- Interpolate submenu: ✅ (ported legacy plane-path interpolation semantics)
- Select submenu: ✅ (ported legacy downstream/upstream/branch/connected semantics)
- Advanced Editing submenu:
  - Remove turn: ✅
  - Resolve crossover: ✅ (ported legacy matching/rewire)
  - Join isolated branch: ✅
  - Join isolated brach (across trees): 🟡 (logic parity; Atlas does not auto-remove empty SWC objects)
  - Reset branch point: ✅
- Change Property submenu:
  - Translate: ✅ (ZSwcSkeletonTransformDialog)
  - Change size: ✅ (ZSwcSizeDialog; single dialog)
  - Set as a root: ✅
- Information submenu:
  - Summary: ✅ (ZInformationDialog)
  - Path length: ✅ (ZInformationDialog)
  - Scaled Path length: ✅ (ZResolutionDialog + ZInformationDialog)

### 2D-only SWC actions
- Extend: ✅ (plain extend + smart extend/path computation)
- Connect to: ✅
- Move to Current Plane: ✅
- Move Selected (Shift+Mouse): ✅
- Estimate Radius: ✅ (signal-fit legacy-like)
- Add Neuron Node: ✅
- Locate node(s) in 3D: ✅

### 3D-only SWC actions
- Extend (toggle): ✅ (smart vs plain matches stack-data presence)
- Move Selected (toggle): ✅ (continuous move merges undo like neuTube)
- Locate node(s) in 2D: ✅
- Change type (SwcTypeDialog): ✅ (apply semantics match neuTube 3D)
- Add neuron node (toggle): ✅ (depth from nearby node, project onto click ray)

## Atlas regression tests

- `test/zswcpackundomergetest.cpp`:
  - Verifies neuTube-like undo merge behavior for repeated move-selected edits.
  - Verifies doc-level SWC node context menu action ordering/labels (Delete/Break/Connect/…).

## Atlas 3D implementation note (threading)

In Atlas, `Z3DRenderingEngine` runs on a dedicated rendering thread (see `src/atlas/z3dmainwindow.cpp`), so any 3D
context menus and dialogs must be created on the UI thread. The current 2D SWC node menu is already UI-thread
safe, but the 3D SWC node menu needs to be implemented via a signal/request to `Z3DCanvas` (similar to how the
3D seed-trace menu is shown) to avoid creating `QMenu`/`QDialog` on the rendering thread.

Status:
- Atlas now shows the 3D SWC node context menu via `Z3DCanvas::showSwcNodeContextMenu()` (UI thread).

## Migration plan (step-by-step)

This is the concrete plan/checklist used to reach parity. Items are marked complete as of this change.

1. [x] Locate neuTube 2D SWC node menu composition (`ZStackPresenter::createSwcNodeContextMenu`).
2. [x] Extract the ordered action list and shortcuts (via `ZActionFactory`).
3. [x] Locate neuTube doc-level submenu construction (`ZMenuFactory::makeSwcNodeContextMenu(ZStackDoc*, ...)`).
4. [x] Locate neuTube 3D SWC node menu composition (`Z3DWindow::createContextMenu`).
5. [x] Locate neuTube 3D doc-level SWC menu factory (`ZStackDocMenuFactory::makeSwcNodeContextMenu`).
6. [x] Write this parity mapping doc (2D + 3D menus, order, semantics, sources).
7. [x] Implement Atlas 2D node context menu composition (`ZSwcFilter::popupSwcNodeContextMenu`).
8. [x] Implement Atlas 2D interactive SWC edit modes (Extend/Connect-to/Move-selected/Add-node) with neuTube semantics.
9. [x] Implement/port shared doc-level SWC actions: Delete / Delete Unselected / Break / Connect / Merge / Insert.
10. [x] Implement/port Interpolate submenu actions (position/radius/Z).
11. [x] Implement/port Select submenu actions (downstream/upstream/branch/connected/all).
12. [x] Implement/port Advanced Editing: Remove turn.
13. [x] Implement/port Advanced Editing: Resolve crossover.
14. [x] Implement/port Advanced Editing: Join isolated branch.
15. [x] Implement/port Advanced Editing: Join isolated brach (across trees).
16. [x] Implement/port Advanced Editing: Reset branch point.
17. [x] Implement/port Change Property: Translate (single dialog).
18. [x] Implement/port Change Property: Change size (single dialog).
19. [x] Implement/port Change Property: Set as a root.
20. [x] Implement/port Information: Summary / Path length.
21. [x] Implement/port Information: Scaled path length (resolution prompt + report).
22. [x] Implement Atlas 3D node context menu on UI thread (`Z3DCanvas::showSwcNodeContextMenu`).
23. [x] Implement Atlas 3D interaction-mode toggles (Extend / Move-selected / Add-node) matching neuTube behavior.
24. [x] Implement 2D↔3D locate actions (Locate node(s) in 3D / Locate node(s) in 2D).
25. [x] Add regression tests (undo-merge + menu ordering/labels) and wire them into CTest.
26. [x] Build and run `ctest --output-on-failure`.
