Atlas User Manual
=================

> 📸 **Screenshot to add:** Application splash or title screen that introduces Atlas. Include version number in the window title.

## Table of Contents
- [1. Introduction](#1-introduction)
  - [1.1 What Atlas Does](#11-what-atlas-does)
  - [1.2 System Requirements](#12-system-requirements)
  - [1.3 Supported Data Types](#13-supported-data-types)
  - [1.4 How This Guide Is Organized](#14-how-this-guide-is-organized)
- [2. Getting Atlas Ready](#2-getting-atlas-ready)
  - [2.1 Building or Installing](#21-building-or-installing)
  - [2.2 First Launch Checklist](#22-first-launch-checklist)
  - [2.3 Understanding the Atlas File Layout](#23-understanding-the-atlas-file-layout)
- [3. Guided Tour of the Interface](#3-guided-tour-of-the-interface)
  - [3.1 2D Main Window at a Glance](#31-2d-main-window-at-a-glance)
  - [3.2 Menus in Detail](#32-menus-in-detail)
  - [3.3 Toolbars and Quick Controls](#33-toolbars-and-quick-controls)
  - [3.4 Dock Widgets](#34-dock-widgets)
  - [3.5 Status Bar and Notifications](#35-status-bar-and-notifications)
  - [3.6 Customizing Your Workspace](#36-customizing-your-workspace)
- [4. Working with Data Objects](#4-working-with-data-objects)
  - [4.1 The Object Lifecycle](#41-the-object-lifecycle)
  - [4.2 Images](#42-images)
  - [4.3 Region of Interest (ROI) Masks](#43-region-of-interest-roi-masks)
  - [4.4 Region Annotations](#44-region-annotations)
  - [4.5 Puncta Sets](#45-puncta-sets)
  - [4.6 SWC Trees](#46-swc-trees)
  - [4.7 Meshes](#47-meshes)
  - [4.8 SVG Overlays](#48-svg-overlays)
  - [4.9 2D Animations](#49-2d-animations)
  - [4.10 3D Animations](#410-3d-animations)
  - [4.11 Alias Objects](#411-alias-objects)
- [5. 2D Workspace Skills](#5-2d-workspace-skills)
  - [5.1 Navigation Fundamentals](#51-navigation-fundamentals)
  - [5.2 View Styles and Slicing](#52-view-styles-and-slicing)
  - [5.3 ROI Authoring Tools](#53-roi-authoring-tools)
  - [5.4 Selection, Copy, and Paste](#54-selection-copy-and-paste)
  - [5.5 The Edit and Output Dock](#55-the-edit-and-output-dock)
  - [5.6 Logging View Changes](#56-logging-view-changes)
- [6. 3D Workspace Skills](#6-3d-workspace-skills)
  - [6.1 Opening and Reusing the 3D Window](#61-opening-and-reusing-the-3d-window)
  - [6.2 Camera Navigation](#62-camera-navigation)
  - [6.3 Object View Settings in 3D](#63-object-view-settings-in-3d)
  - [6.4 Global View Settings in 3D](#64-global-view-settings-in-3d)
  - [6.5 Background, Axis, and Help Panels](#65-background-axis-and-help-panels)
  - [6.6 The Progress Toolbar and Rendering Queue](#66-the-progress-toolbar-and-rendering-queue)
- [7. Scene Management](#7-scene-management)
  - [7.1 Saving Your Workspace](#71-saving-your-workspace)
  - [7.2 Loading Scenes Step by Step](#72-loading-scenes-step-by-step)
  - [7.3 Troubleshooting Scene Loads](#73-troubleshooting-scene-loads)
- [8. Capture and Export](#8-capture-and-export)
  - [8.1 2D Screenshots](#81-2d-screenshots)
  - [8.2 3D Screenshots](#82-3d-screenshots)
  - [8.3 3D Animation Export in the GUI](#83-3d-animation-export-in-the-gui)
  - [8.4 Headless 3D Animation Export](#84-headless-3d-animation-export)
- [9. Workflow Recipes](#9-workflow-recipes)
  - [9.1 Explore a New Dataset](#91-explore-a-new-dataset)
  - [9.2 Create and Refine ROIs with a Mask Image](#92-create-and-refine-rois-with-a-mask-image)
  - [9.3 Build a 3D Animation for Presentation](#93-build-a-3d-animation-for-presentation)
  - [9.4 Generate High-Resolution Stereo Captures](#94-generate-high-resolution-stereo-captures)
  - [9.5 Batch Export Animations via CLI](#95-batch-export-animations-via-cli)
- [10. Configuration, Logs, and Maintenance](#10-configuration-logs-and-maintenance)
  - [10.1 Configuration Files and Flags](#101-configuration-files-and-flags)
  - [10.2 Log Files and Diagnostics](#102-log-files-and-diagnostics)
  - [10.3 Custom Commands](#103-custom-commands)
  - [10.4 Updating and Multiple Instances](#104-updating-and-multiple-instances)
- [11. Troubleshooting and FAQ](#11-troubleshooting-and-faq)
  - [11.1 Common Errors and Fixes](#111-common-errors-and-fixes)
  - [11.2 Performance Tuning](#112-performance-tuning)
  - [11.3 Rendering Quality Tips](#113-rendering-quality-tips)
- [12. Reference Appendix](#12-reference-appendix)
  - [12.1 Keyboard and Mouse Shortcuts](#121-keyboard-and-mouse-shortcuts)
  - [12.2 Command-Line Flags](#122-command-line-flags)
  - [12.3 File Format Support at a Glance](#123-file-format-support-at-a-glance)
  - [12.4 Glossary](#124-glossary)

---

## 1. Introduction

### 1.1 What Atlas Does
Atlas is a multi-modal visualization and analysis environment designed for large 2D and 3D datasets. The application combines:

- High-performance volume rendering for large image stacks.
- Interactive editing and inspection tools for meshes, neuronal skeletons (SWC), puncta, ROI masks, and SVG overlays.
- Dedicated 2D and 3D workspaces that share the same underlying documents (`ZDoc` and friends) so state stays synchronized.
- Scene persistence (`*.scene`) and animation timelines (`*.animation3d` and 2D counterparts) for reproducible visual storytelling.
- GPU-accelerated rendering (OpenGL or Vulkan backends) with CLI automation for headless exports.

Atlas is crafted to handle entire imaging pipelines, from loading raw data to producing publishable figures and videos.

### 1.2 System Requirements

- **Hardware**: A modern CPU and GPU with drivers that support OpenGL 4.5+. Vulkan support provides additional acceleration when enabled. For large volumes, favor GPUs with ≥8 GB VRAM.
- **RAM**: At least 16 GB is recommended for large multi-channel stacks.
- **Storage**: Scenes reference the original data rather than copying it, but high-resolution exports can consume significant disk space. Reserve space for temporary frames.
- **Operating systems**: macOS, Windows, and Linux builds are supported. Some features (desktop entry creation, EGL flags) are platform-specific.

### 1.3 Supported Data Types

Atlas organizes data into documents. Each document type contributes load actions, save routines, and editing widgets.

| Document | Typical Extensions | Notes |
| --- | --- | --- |
| `ZImgDoc` (Images) | `.tif`, `.tiff`, `.ome.tif`, `.mhd`, `.raw`, `.nii`, `.hdr`, `.png`, `.jpg`, `.bmp`, `.exr`, `.lsm`, `.v3draw` | Multi-channel, multi-timepoint volumes supported. Sequences can be imported as stacks. |
| `ZROIDoc` (ROI Masks) | `.roi`, `.mask`, `.nii`, `.mhd`, `.nrrd` | Accepts atlas-generated ROI files or converts mask images into editable ROIs. |
| `ZRegionAnnotationDoc` | `.annotation`, `.json`, label images | Handles labeled regions; can import/export label images. |
| `ZPunctaDoc` | `.apo`, `.csv`, `.json` | Stores point clouds like synaptic puncta with undo support. |
| `ZSwcDoc` | `.swc`, `.eswc`, `.json` | Manages neuronal tree structures with per-node attributes. |
| `ZMeshDoc` | `.obj`, `.ply`, `.stl`, `.off`, `.vtk`, `.gii` | Calculates surface/volume metrics on load. |
| `ZSvgDoc` | `.svg` | Overlays vector graphics (regions, labels). |
| `Z2DAnimationDoc` | `.animation2d` | Timeline for 2D view parameters. |
| `Z3DAnimationDoc` | `.animation3d` | Timeline for 3D parameters and camera paths. |

> 📸 **Screenshot to add:** A collage showing different supported object types loaded into the Objects Manager.
<p align="center">
  <img src="./images/zuhe.png" alt="label_region" width="800">
</p>

### 1.4 How This Guide Is Organized

Sections 2–6 walk through setup and the interface. Sections 7–10 cover everyday tasks and advanced workflows. Sections 11 and 12 provide troubleshooting and reference material. Each hands-on section provides step-by-step instructions, and you will find screenshot placeholders wherever a visual aid is helpful. Replace those placeholders with actual screen captures when documenting your deployment.

---

## 2. Getting Atlas Ready

### 2.1 Building or Installing

1. **Clone the repository** (or obtain binaries if your organization provides them).
2. **Follow `readme.md`** for platform-specific build instructions. Typical steps include configuring CMake with either Qt’s qmake-compatible toolchain or Ninja, then compiling the `atlas` target.
3. **Install runtime dependencies** such as Qt libraries and OpenGL/Vulkan drivers. Verify GPU driver updates before launching.
4. **Run the application** through the provided launcher or by executing the compiled binary:
   ```bash
   ./build/atlas
   ```
5. **Optional: create a desktop shortcut** using the Help menu once Atlas is running (Linux only).

### 2.2 First Launch Checklist

Perform the following steps the first time you open Atlas:

1. **Start Atlas**. The 2D main window appears.
2. **Confirm GPU initialization**. In the console/log window you should see messages from `Z3DRenderingEngine` indicating that the OpenGL or Vulkan context initialized. When Vulkan is enabled, Atlas performs asynchronous end‑of‑frame readback (offscreen) with a default one‑frame latency for UI display.
3. **Open the 3D window** via **View → Open 3D Window** once to confirm that the rendering engine loads correctly. Close it again if you prefer to start in 2D.
4. **Open the Help dock** and skim navigation shortcuts.
5. **Generate configuration file** if you require custom flags: **Help → Generate Config File** copies the default `settings_flagfile.txt` into your config directory. You can edit the new `user_settings_flagfile.txt` afterwards.
6. **Set your default working folders** by loading an image to seed the recent-files list and default directory history.

### 2.3 Understanding the Atlas File Layout

Atlas keeps runtime files in a few key locations:

- **Installation directory**: contains the executable, Qt frameworks, and resources.
- **Log directory** (`ZSystemInfo::logDir()`): runtime logs, including 3D engine diagnostics.
- **Config directory** (`ZSystemInfo::configDir()`): user settings, generated flag files, animation defaults.
- **Scene files** (`*.scene`): stored wherever you save them; contain serialized document state and view settings.
- **Animation files** (`*.animation3d`, `*.animation2d`): saved alongside your data or in project folders.

> 📸 **Screenshot to add:** Finder/Explorer window showing config and log directories after first launch.

---

## 3. Guided Tour of the Interface

### 3.1 2D Main Window at a Glance

> 📸 **Screenshot to add:** The main window with each labeled region (Menus, Toolbars, Objects Manager, 2D View, Docks).
<p align="center">
  <img src="./images/labeled_region.png" alt="label_region" width="800">
</p>

Key regions:

1. **Menu bar** (top): global commands grouped by theme (File, Edit, View, Animation, Window, Help).
2. **Toolbars** (top rows): quick access to open, save, zoom, view mode toggles, ROI tools, help).
3. **Objects Manager** (right dock): tree of all loaded objects with visibility and lock controls.
4. **Central 2D View**: renders images and overlays; responds to navigation and editing gestures.
5. **Dock widgets** (right, bottom, and floating): view settings, detailed metadata, capture panels, edit widgets, help text.
6. **Status bar** (bottom): short messages like “Ready” or “scene saved as ...”.

### 3.2 Menus in Detail

Below is an expanded description of each menu. Items contributed by individual documents (for example, `ZImgDoc`) appear dynamically.

#### File

1. **Open...** – prompts for `.scene` files; loads entire workspaces.
2. **Save** – saves all modified objects back to their original source files via the owning document (`ZObjDoc::save`).
3. **Save As...** – saves selected objects to new filenames.
4. **Load Scene... / Save Scene...** – persistent workspace import/export.
5. **Document-specific actions** – one cluster per document, including `Load Image...`, `Import Sequence Images...`, `Load ROI...`, `Load Mesh...`, etc., followed by `Remove All <Type>` items.
6. **Recent files** – up to nine entries for quick reopening.
7. **Close** – closes the 2D window (prompts to save unsaved objects).
8. **Exit** – quits Atlas entirely.

#### Edit

1. **Undo / Redo** – connected to the active object’s undo stack (from `QUndoGroup`).
2. **Copy / Paste** – operate on 2D selections, ROI shapes, or annotation data depending on current tool.

#### View

1. **Zoom In / Zoom Out** – navigational shortcuts.
2. **Fit Into Window** – resizes the viewport to fit all visible data.
3. **Normal View / Maximum Z Projection / Montage View** – toggles among slice views.
4. **Open 3D Window** – launches or raises the synced 3D window.
5. **Screenshot** – opens the Capture dock.

#### Animation

1. **Make 2D Animation** – seeds a 2D timeline from the current state.
2. **Change Animation Settings...** – adjusts global animation defaults.

#### Window

- Toggle all dock widgets. If a dock is closed, use this menu to bring it back.

#### Help

1. **About Atlas / About Qt** – application and Qt version info.
2. **Check for Updates** – launches Qt’s MaintenanceTool.
3. **Help** – raises the Help dock.
4. **Create Desktop Entry** (Linux) – writes a `.desktop` file under `~/.local/share/applications`.
5. **Open Log Folder / Open Config Folder** – opens respective directories in the OS file browser.
6. **Generate Config File** – copies `settings_flagfile.txt` from resources to the config directory.
7. **Run Custom Command** – invokes `ZCustomCommand` (see section 10.3).

### 3.3 Toolbars and Quick Controls

Toolbars mirror frequently used menu actions:

- **File Toolbar** – Open, Save.
- **Edit Toolbar** – Undo, Redo.
- **View Toolbar** – Zoom controls, scale widget (progressively updated by `ZView`), view style toggles, 3D window shortcut, screenshot trigger.
- **Drag Mode Toolbar** – toggles between Scroll Hand Drag and Rubber Band Drag; integrates ROI sculpting actions (spline, polygon, rectangle, ellipse, cut).
- **ROI Toolbar** – drop-down ROI tool selector plus ROI mode switch (RegionAnnotation vs ROI).
- **Help Toolbar** – quick access to the Help dock.

Toolbars can be rearranged or floated like standard Qt toolbars. Right-click a toolbar area to toggle visibility.

### 3.4 Dock Widgets

Dock widgets provide specialized interfaces. You can anchor them to any side, tabify related docks, or float them.

- **Objects Manager** (`ZObjWidget`) – lists objects with eye icons (visibility) and lock icons. Supports multi-selection, context menu operations, and Delete/Backspace removal.
- **Object View Setting** (`ZViewSettingWidget`) – per-object controls like channel visibility, transfer functions, transforms, bounding box styling.
- **Global View Setting** – controls for global plane cuts, camera defaults, fog, transparency mode, lighting and stereo parameters.
- **Object Detailed Info** (`ZObjDetailedInfoWidget`) – read-only metadata such as voxel sizes, mesh statistics, ROI metrics.
- **Capture** – wraps `ZTakeScreenShotWidget` for 2D output.
- **Help** – static reminder of navigation shortcuts; can float for quick reference.
- **Edit and Output** (`ZObjEditWidget`) – hosts a tab per active editor (ROI editor, animation timeline, puncta detection). Also contains a persistent “Log Output” tab for system logs.

> 📸 **Screenshot to add:** Objects Manager context menu showing Show/Hide, Lock/Unlock, Save, Save As, Make Alias.
<p align="center">
  <img src="./images/Objects_Manager _context_menu.png" alt="Objects_Manager _context_menu" width="200">
</p>

### 3.5 Status Bar and Notifications

- Displays the latest action result (“Ready”, “scene saved as ...”).
- Long-running operations (e.g., 3D export) also update the 3D window’s progress bar.
- Log output is available in the Edit and Output dock’s log tab for detailed investigation.

### 3.6 Customizing Your Workspace

1. Resize docks and arrange toolbars to fit your workflow. Atlas remembers window size (via `QSettings`).
2. Tabify related docks (e.g., Object View Setting and Global View Setting) to save space.
3. Float the Help dock or Capture dock onto a secondary monitor if desired.
4. If you have multiple monitors, drag the 3D window to a dedicated display and Atlas will remember the position (3D window geometry is saved separately).

---

## 4. Working with Data Objects

### 4.1 The Object Lifecycle

1. **Load** – Each document exposes `loadFile` actions that return a unique object ID. Atlas records the source path and adds the object to the Objects Manager.
2. **Inspect** – Select the object to switch the Object View Setting and Detailed Info docks to the relevant controls.
3. **Edit** – For editable objects, double-click or use context actions to open editors in the Edit and Output dock.
4. **Save** – Use context menu → Save, or global Save (Ctrl/Cmd+S) to persist modifications via the owning document.
5. **Remove** – Delete keys or context menu → Remove. Scene saves do not include removed objects.
6. **Alias** – Create aliases for alternative views without duplicating data.

Tip: Objects with IDs <100 (background, axis, lighting) belong to the 3D environment and remain hidden in most lists. User objects start at ID 100.

### 4.2 Images

Steps to load and manage images via `ZImgDoc`:

1. **Load**
   1. Choose **File → Load Image...**.
   2. Select one or more image files. Atlas supports multi-selection.
   3. Confirm. Each image becomes a new object in the manager.
2. **Import sequences** – use **File → Import Sequence Images...** to select an ordered set of images. Atlas stacks the frames into a volume.
3. **View settings** – with the image selected, the Object View Setting dock exposes channel toggles, color maps, and transfer functions. Modify per alias if needed.
4. **Full resolution rendering** – in 3D, enable Full Resolution in Object View Setting when you require high-quality output. Monitor GPU memory usage and progress logs.
5. **Save** – `ZImgDoc` saves back to original paths when possible. If the format does not support writing (or the image was imported as a sequence), use **Save As...** to choose a new format.
6. **Advanced processing** – Access via the document menu or object context menu:
   - **Stitch Images...** – run the image stitching dialog for tiled data.
   - **Align Sections...** – align serial sections.
   - **Correct Chromatic Shift...** – adjust channel misalignment.

> 📸 **Screenshot to add:** Object View Setting dock for an image showing channel controls.
<p align="center">
  <img src="./images/channel_control.png" alt="channel_control" width="400">
</p>

### 4.3 Region of Interest (ROI) Masks

`ZROIDoc` handles ROI packs.

1. **Load ROI files** – **File → Load ROI...** and select `.roi` or compatible files.
2. **Import mask image** – **File → Import Mask Image...** converts a mask image into atlas-editable ROI(s).
3. **Edit** – double-click the ROI object to open the ROI editor tabs. Use spline/polygon/rectangle tools in the ROI toolbar.
4. **Convert to mask** – **File → To Mask Image...** exports the ROI back into a mask image after edits.
5. **Undo support** – ROI edits push onto a per-object undo stack, so Undo/Redo apply locally.
6. **Save** – Save writes to the original ROI file; Save As lets you export to a new destination.

### 4.4 Region Annotations

`ZRegionAnnotationDoc` manages labeled annotations.

1. **Load** – **File → Load RegionAnnotation...**.
2. **Import label images** – use **Import Label Image...** to convert a label volume into Atlas annotations.
3. **Edit** – open the annotation editor in the Edit and Output dock. Modify labels, merge/split as needed.
4. **Export** – **Export Label Image...** writes label data to disk.
5. **Alias** – create aliases to compare different styling or visibility combinations.

### 4.5 Puncta Sets

`ZPunctaDoc` stores point-based annotations (e.g., synapses).

1. **Load** – **File → Load Puncta...**; supports typical `.apo` formats.
2. **Detect** – choose **Detect Puncta...** to launch automatic detection (when available) and feed results into the document.
3. **Analysis export** – **Generate Analysis Text Files...** outputs CSV summaries of the puncta set.
4. **Edit** – double-click to open the puncta editor; add/remove points, adjust thresholds. Undo/Redo tied to the puncta pack.
5. **Save** – Save writes in the native format when possible; Save As provides new formats.

### 4.6 SWC Trees

`ZSwcDoc` manages neuronal skeletons.

1. **Load** – **File → Load Swc...**. Atlas prevents duplicate loads by checking canonical paths.
2. **Edit** – open the SWC editor to adjust node positions, prune branches, annotate attributes.
3. **View settings** – adjust line thickness, color schemes in Object View Setting.
4. **Save** – Save writes to the source path if the format supports writing; otherwise Save As prompts for a new file.

### 4.7 Meshes

`ZMeshDoc` handles triangle meshes.

1. **Load** – **File → Load Mesh...**.
2. **Inspect** – Object Detailed Info shows metrics (bounding box, surface area, volume, curvature statistics) computed lazily by `MeshPack`.
3. **Aliases** – create multiple aliases to compare shading or transformations without duplicating geometry.
4. **Save** – Save As writes to canonical formats; Save uses the original format if writeable.

### 4.8 SVG Overlays

`ZSvgDoc` imports vector overlays.

1. **Load** – **File → Load Svg...**.
2. **View** – overlays appear in both 2D and 3D (if applicable) for labeling or outlining features.
3. **Aliases** – maintain variants with different styling by making aliases.
4. **Save** – Save writes to the original SVG path.

### 4.9 2D Animations

`Z2DAnimationDoc` stores viewport animations.

1. **Create** – **Animation → Make 2D Animation**; name the animation.
2. **Edit** – open the animation tab in Edit and Output. Set keyframes, timing, interpolation.
3. **Bind view** – the animation references the current 2D view; ensure the animation is visible to preview.
4. **Save** – Save or Save As to store `.animation2d` files.

### 4.10 3D Animations

`Z3DAnimationDoc` governs 3D timelines.

1. **Load** – **File → Load 3D Animations...**.
2. **Create** – in the 3D window use **Animation → Make 3D Animation**.
3. **Bind view** – the 3D window emits `viewReady` so the animation doc can bind to the engine. Logs show “3D animation parameters bound”.
4. **Edit** – use Edit and Output dock to tweak camera paths, object transforms, opacity over time.
5. **Export** – see section 8 for GUI and CLI exports.

### 4.11 Alias Objects

1. Select one or more objects in the Objects Manager.
2. Right-click and choose **Make Alias**. Atlas creates a new object ID referencing the same underlying data.
3. Configure separate view settings or animations for the alias.
4. Use Full Resolution rendering or unique color maps per alias to stage complex scenes.

---

## 5. 2D Workspace Skills

### 5.1 Navigation Fundamentals

1. **Zoom** – use mouse wheel, `Ctrl/Cmd` + `+`/`-`, or View toolbar buttons.
2. **Pan** – enable Scroll Hand Drag mode or press Spacebar while dragging.
3. **Rubber band selection** – choose Rubber Band Drag and draw selection boxes.
4. **Fit to window** – `F` (shortcut of Fit Into Window) to reframe the dataset.

### 5.2 View Styles and Slicing

1. **Normal View** – displays the current slice (controlled by the slice spin box below the view).
2. **Maximum Z Projection** – integrates all slices along Z. Range display updates to `[min, max+1]` as implemented in `ZView::currentSliceRange`.
3. **Montage View** – arranges slices into a grid; adjust columns via the “Montage Columns” parameter. Atlas computes rows based on dataset depth.
4. **Time navigation** – for time-series data, adjust the time spin box.
5. **Viewport parameter** – read `ZView::viewportPara` to track the exact bounding box displayed.

### 5.3 ROI Authoring Tools

1. **Activate ROI mode** – choose ROI or RegionAnnotation from the ROI Mode drop-down.
2. **Select a drawing tool** – spline, polygon, rectangle, ellipse, or Cut (splits existing ROIs).
3. **Draw** – click to place control points. For splines, double-click to finish; for polygons, close the shape.
4. **Edit** – use context handles in the Edit and Output dock or switch to selection mode to move points.
5. **Undo/Redo** – `Ctrl/Cmd+Z` / `Ctrl/Cmd+Shift+Z` operate on the ROI’s undo stack.
6. **Convert to masks** – use the ROI document actions described in section 4.3.

### 5.4 Selection, Copy, and Paste

- `Ctrl/Cmd+C` (`ZView::copy`) copies the current ROI/selection.
- `Ctrl/Cmd+V` (`ZView::paste`) pastes into the current document (ROI or RegionAnnotation as dictated by ROI mode).
- Use the context menu in Objects Manager to copy file paths or show items in the OS file browser.

### 5.5 The Edit and Output Dock

1. **Access** – double-click an object in Objects Manager or choose **Open Edit Widget** from context.
2. **Tabs** – each object with an editor gets its own tab labeled `Edit <Name [ID]>`. Titles update automatically when objects are renamed or modified.
3. **Log Output** – pinned first tab collects log lines (via `ZLogWidget`).
4. **Closing tabs** – click the close button. The log tab cannot be closed (
`ZObjEditWidget` hides the button for tab index 0).

### 5.6 Logging View Changes

- Watch the status bar for quick updates.
- Open the log tab to see details (e.g., ROI operations, animation binding, 3D engine events). Each log entry helps correlate actions with underlying system behavior.

---

## 6. 3D Workspace Skills

### 6.1 Opening and Reusing the 3D Window

1. In the 2D window, choose **View → Open 3D Window** or press the toolbar button.
2. Atlas creates `Z3DMainWindow`, shares the `ZDoc`, and wires up signals so selection state remains synchronized.
3. Closing the 3D window releases GPU resources but keeps objects intact. Reopen at any time.
4. If a scene file contains 3D state, Atlas automatically opens the 3D window during load (see section 7.2).

### 6.2 Camera Navigation

- **Rotate** – left-click drag or `Ctrl/Cmd` + arrow keys.
- **Pan** – `Shift` + drag or `Shift` + arrow keys.
- **Zoom/Dolly** – mouse wheel, `Ctrl/Cmd +`/`-`.
- **Roll** – `Alt` + drag or `Alt` + left/right arrows.
- **Reset Camera** – toolbar button or **View → Reset Camera** fits all visible objects.
- **Context menu** – right-click for quick options.

### 6.3 Object View Settings in 3D

1. Select an object in Objects Manager.
2. In the Object View Setting dock at right, adjust parameters such as visibility, transform (translation, rotation, scale), bounding box style, transfer functions, slice toggles, and per-object clipping.
3. Use the Global/Per-object tabs to manage render passes.
4. Changes immediately affect the 3D canvas; for heavy operations (full-resolution volume streaming) watch the progress toolbar.

### 6.4 Global View Settings in 3D

1. **Open the Global View Setting dock**.
2. **Camera** – set projection, focal distance, clip planes, stereo eye parameters.
3. **Lighting** – toggle global lighting, adjust intensities.
4. **Fog and transparency** – choose a transparency method (Blend No Depth Mask, Blend Delayed, Dual Depth Peeling, Weighted Average, Weighted Blended). Use Weighted methods for large translucent scenes.
5. **Global cuts** – X/Y/Z plane sliders clip data globally; oblique cuts reveal interior structures.
   - Global Cut Mode (per axis) determines how the two endpoints are recalculated when dataset bounds change:
     - Absolute: hold values in world units; clamp to the new range.
       - newLower = clamp(oldLower, min, max); newUpper = clamp(oldUpper, min, max)
     - Track Edges: pin each endpoint independently to the moving min/max using “Pin Lower/Pin Upper”. If a toggle is OFF, that endpoint holds its absolute value (clamped).
       - newLower = (PinLower ? min : clamp(oldLower, min, max))
       - newUpper = (PinUpper ? max : clamp(oldUpper, min, max))
     - Normalized [0..1]: store fractional endpoints f0,f1; recompute by linear interpolation on each bounds change.
       - newLower = min + (max−min)·f0; newUpper = min + (max−min)·f1
   - Defaults: Track Edges with both toggles ON (shows full range and follows edges).
   - Tips: Track Edges to keep “show all” or pin sides while the other stays fixed; Absolute to lock a fixed window; Normalized to keep a proportional window (e.g., 0.1→0.9).

### 6.5 Background, Axis, and Help Panels

- **Background dock** – select gradient colors, environment maps, or background images.
- **Axis dock** – expose axis gizmo parameters.
- **Help dock** – duplicates navigation shortcuts for quick reference.

### 6.6 The Progress Toolbar and Rendering Queue

1. Long-running processes (full-res rendering, animation export) update the progress bar.
2. **Cancel Rendering** – stop the current job without exiting the application.
3. Check logs for “waiting for 3D scene apply to finish” messages when using blocking options.

> 📸 **Screenshot to add:** 3D window with Progress toolbar visible during a render.
<p align="center">
  <img src="./images/Progress_toolbar.png" alt="Progress_toolbar" width="800">
</p>

---

## 7. Scene Management

### 7.1 Saving Your Workspace

1. Ensure all objects are saved (use **File → Save** first; unsaved objects trigger prompts).
2. Choose **File → Save Scene...**.
3. Select a destination `.scene` file. Atlas records the directory to seed next time.
4. Atlas serializes:
   - Document state (`ZDoc::write`).
   - Per-object 2D view settings.
   - Per-object 3D view settings (queried from the 3D engine via `Z3DRenderingEngine::write`).
   - Global view settings for both 2D and 3D scenes.
5. Confirmation appears in the status bar (“scene saved as ...”).

### 7.2 Loading Scenes Step by Step

1. **Drag and drop** a `.scene` file into the 2D window or choose **File → Load Scene...**.
2. Atlas parses the JSON, restores documents, and updates the Objects Manager.
3. If 3D state is present and the 3D window is not open, Atlas launches it and waits for `renderingEngineInitialized`.
4. View settings are applied. Logs may contain “waiting for 3d window initialization” until the engine is ready.
5. If `--atlas_block_scene_3d_apply` is active, Atlas blocks until “3D scene parameters applied” appears in the log.

### 7.3 Troubleshooting Scene Loads

- **Missing files** – Atlas reports missing resources in a message box and continues loading remaining data. Use the Object Detailed Info dock to inspect paths.
- **Errors** – messages such as `Can not load scene <file>: <error>` appear when JSON parsing or object restoration fails. Check logs for stack traces.
- **Partial 3D apply** – enable `--atlas_block_scene_3d_apply` for deterministic apply when scripts depend on fully realized scenes.

---

## 8. Capture and Export

### 8.1 2D Screenshots

1. Open the Capture dock (View toolbar → Screenshot).
2. Choose **Capture Single Image** or **Capture Rotating Image sequence**.
3. Set filename handling:
   - Automatic numbering: choose folder and prefix.
   - Manual naming: enable “Use Manual Name” to invoke file dialog each time.
4. Decide on resolution:
   - Use window size or specify custom width/height.
5. Click **Capture**.
6. 2D captures emit the signal `take2DScreenShot` or `takeFixedSize2DScreenShot`, writing PNG images.

> 📸 **Screenshot to add:** 2D Capture dock with annotations of important controls.
<p align="center">
  <img src="./images/2DCapture.png" alt="2DCapture" width="400">
</p>

### 8.2 3D Screenshots

1. In the 3D window, open the Capture dock.
2. Choose mono or stereo (Half / Full side-by-side) output.
3. Set window or custom size. For large outputs enable tiling (tile size and border).
4. Optionally configure rotation sequences (axis, direction, duration, frame rate) for dynamic captures.
5. Click **Capture**. The engine renders the frame(s) and stores them in the target folder.
6. Monitor the Progress toolbar; cancel if necessary.
<p align="center">
  <img src="./images/3DCapture.png" alt="3DCapture" width="400">
</p>

### 8.3 3D Animation Export in the GUI

1. Prepare a 3D animation (section 4.10).
2. In the 3D Capture dock, switch to animation export mode.
3. Configure parameters:
   - Output filename (video file).
   - Frame rate, start frame, end frame.
   - Output size, stereo mode, tiling.
   - Optional flags (overwrite existing output, limit memory usage).
4. Start export. Atlas renders frames, optionally encodes a video (ffmpeg integration), and reports progress.
5. Completion is indicated by progress bar reaching 100% and logs noting success.
<p align="center">
  <img src="./images/animationexport.png" alt="animationexport" width="800">
</p>

### 8.4 Headless 3D Animation Export

For automation or cluster rendering:

1. Prepare a `.animation3d` file and ensure referenced data is accessible.
2. Run Atlas with CLI flags:
   ```bash
   ./atlas \
     --run_export_3d_animation \
     --filename path/to/animation.animation3d \
     --output_filename path/to/output.mp4 \
     --output_fps 30 \
     --output_start_frame 0 \
     --output_end_frame 450 \
     --output_width 1920 \
     --output_height 1080
   ```
3. Optional flags:
   - `--overwrite`
   - `--output_image_folder_name frames`
   - `--skip_video_compression`
   - `--output_image_name_prefix frame_`
   - `--output_image_name_field_width 5`
   - `--output_tile_size 2048`
   - `--output_tile_border 32`
   - `--limit_memory_usage_in_gb_to 12`
   - On Linux: `--use_gpu_devices 0,1 --__use_EGL`
4. Monitor CLI logs for status messages (`3D animation parameters bound`, progress updates, errors).

---

## 9. Workflow Recipes

### 9.1 Explore a New Dataset

1. **Create a new scene**: launch Atlas (main window empty).
2. **Load images**: drag a folder of `.tif` files onto the window. Atlas loads recognized formats, warns about unsupported ones.
3. **Inspect slices**: set view mode to Maximum Z Projection, then back to Normal to inspect key slices.
4. **Adjust channels**: in Object View Setting, hide channels to isolate structures.
5. **Open 3D view**: **View → Open 3D Window**.
6. **Enable full resolution** for the key object and note GPU memory in logs.
7. **Save scene**: **File → Save Scene...** (`dataset_exploration.scene`).
8. **Capture overview**: take a 3D screenshot for documentation.

### 9.2 Create and Refine ROIs with a Mask Image

1. **Load volume**: follow steps in section 9.1.
2. **Import mask**: **File → Import Mask Image...**, select a binary mask.
3. **Switch to ROI mode**: ROI toolbar drop-down → ROI.
4. **Inspect imported ROI**: select the ROI object to view overlays.
5. **Refine with spline tool**:
   1. Choose Spline tool.
   2. Draw adjustments around edges.
   3. Use Cut tool to split the ROI if necessary.
6. **Check in 3D**: open the 3D window, ensure ROI alias is visible (e.g., as a surface overlay).
7. **Export mask**: **File → To Mask Image...** and choose output filename.
8. **Save ROI object**: Save or Save As to persist edits.

### 9.3 Build a 3D Animation for Presentation

1. **Prepare scene**: load objects, arrange view, save base scene.
2. **Create animation**: in 3D window **Animation → Make 3D Animation**, name it “Presentation”.
3. **Set keyframes**: in Edit and Output dock, add keyframes for camera positions, object visibility, channel changes.
4. **Preview**: play the animation in the 3D window, adjust timing as needed.
5. **Export**: follow section 8.3 to produce a video (e.g., 1920×1080 @ 30 fps).
6. **Document**: capture still frames for slides.

### 9.4 Generate High-Resolution Stereo Captures

1. **Open animation or static scene** in 3D window.
2. **Capture dock**: enable “Capture Stereo Image”.
3. **Set custom resolution**: choose e.g., 4096×4096.
4. **Enable tiling**: set tile size to 2048, border 32.
5. **Capture**: start capture and monitor progress.
6. **Post-process**: combine left/right images as needed or keep the stereo pair (Half or Full side-by-side).

### 9.5 Batch Export Animations via CLI

1. **Prepare a script** that iterates over animation files.
2. **Call Atlas** with CLI flags for each file. Example (bash):
   ```bash
   for anim in animations/*.animation3d; do
     base=$(basename "$anim" .animation3d)
     ./atlas \
       --run_export_3d_animation \
       --filename "$anim" \
       --output_filename "renders/${base}.mp4" \
       --output_fps 24 \
       --output_start_frame 0 \
       --output_end_frame 600 \
       --output_width 3840 \
       --output_height 2160 \
       --limit_memory_usage_in_gb_to 10
   done
   ```
3. **Check logs** after each run for errors.
4. **Review outputs**: verify video files (or frame folders if using image sequence mode).

---

## 10. Configuration, Logs, and Maintenance

### 10.1 Configure via Flag File

Atlas supports a gflags-based configuration file that lets you tweak performance, memory, and debugging behavior without recompiling.

- Generate file: use **Help → Generate Config File**. This copies the template into your config directory as `user_settings_flagfile.txt`.
- Open location: use **Help → Open Config Folder** to open the directory in your file browser.
- Edit format: open `user_settings_flagfile.txt` in a text editor. Use one flag per line with `--name=value`. Lines starting with `#` are comments; blank lines are allowed.
- Apply changes: save the file and restart Atlas. Flags are read at startup. If a value is invalid or misspelled, it will not be applied—check the startup logs for any parse errors.

Examples

```text
# Increase cache memory usage (50% of RAM)
--atlas_image_cache_memory_proportion=0.5

# Enable OpenGL debugging (slower; use when diagnosing issues)
--atlas_debug_opengl=true

# Vulkan validation/diagnostics
--atlas_debug_vulkan=true

# Raise ray-march rounds for volume rendering
--atlas_volume_rendering_maximum_round=200

# Increase log verbosity
--v=1
```

Tips

- Start conservatively when raising limits (e.g., cache sizes or volume rounds). If performance regresses, reduce the values.
- Prefer the flagfile for persistent tweaks. Use command-line flags to temporarily override settings (see section 12.2).

### 10.2 Log Files and Diagnostics

1. **Open log folder**: **Help → Open Log Folder**. Each run generates timestamped logs.
2. **Increase verbosity**: launch Atlas with `--v=1` or set environment variable `GLOG_v=1` for detailed logging.
3. **Debug builds** (compiled with `ATLAS_DEBUG_VERSION`) provide additional lines (parameter-change reasons, port propagation, image filter cut checks, camera near/far plane changes).

### 10.3 Custom Commands

1. Configure the custom command (see `ZCustomCommand` configuration) to point to scripts or executables.
2. Run **Help → Run Custom Command** to execute the script within Atlas. Useful for data preprocessing or invoking external pipelines.
3. Monitor log output for command status.

### 10.4 Updating and Multiple Instances

- **Updates**: **Help → Check for Updates** launches Qt MaintenanceTool if present.
- **Multiple instances (macOS)**: from the dock icon, choose **Open Additional Instance of Atlas**.
- **Desktop entry (Linux)**: create a launcher for easy access.

---

## 11. Troubleshooting and FAQ

### 11.1 Common Errors and Fixes

| Symptom | Likely Cause | Resolution |
| --- | --- | --- |
| “Can not read file ...” | Unsupported format or missing file | Validate path, convert format, or ensure file exists. |
| Scene load waits indefinitely | 3D window not ready | Watch logs; ensure 3D window is visible. Use `--atlas_block_scene_3d_apply` only when necessary. |
| 3D window fails to open | GPU initialization error | Update GPU drivers, verify OpenGL/Vulkan support, check logs for `Z3DRenderingEngine` errors. |
| Exported animations missing frames | Out-of-disk space or canceled job | Increase disk space, rerun export, check progress log. |
| Full-resolution render never starts | Insufficient GPU memory or aggressive cuts | Reduce `atlas_image_block_size`, limit visible region, disable other full-res objects. |

### 11.2 Performance Tuning

1. Use aliases to isolate high-quality rendering to specific objects.
2. Apply global cuts to remove out-of-view data.
3. Adjust sampling rates and switch to MIP or Local MIP when real-time performance is needed.
4. In headless mode, use `--limit_memory_usage_in_gb_to` to cap GPU memory usage.
5. Disable unnecessary transparency techniques when not needed.

### 11.3 Rendering Quality Tips

1. Use Dual Depth Peeling for complex translucent scenes; switch to Weighted Blended for faster previews.
2. Increase sampling rate for smoother DVR at the expense of performance.
3. Use tiled exports for extremely high resolutions to avoid GPU texture limits.
4. Enable stereo captures cautiously—eye separation settings live in Global View Setting.

---

## 12. Reference Appendix

### 12.1 Keyboard and Mouse Shortcuts

| Context | Action | Shortcut |
| --- | --- | --- |
| 2D | Zoom in | `Ctrl/Cmd` + `=` or `+` |
| 2D | Zoom out | `Ctrl/Cmd` + `-` |
| 2D | Fit into window | `F` (via menu) |
| 2D | Pan | Scroll Hand Drag or Space + drag |
| 2D | Rubber band select | Rubber Band Drag |
| 2D | Copy | `Ctrl/Cmd+C` |
| 2D | Paste | `Ctrl/Cmd+V` |
| 3D | Rotate | Left drag or `Ctrl/Cmd` + arrow keys |
| 3D | Pan | `Shift` + drag or `Shift` + arrow keys |
| 3D | Zoom | Mouse wheel or `Ctrl/Cmd` + `+`/`-` |
| 3D | Roll | `Alt` + drag or `Alt` + left/right |
| 3D | Reset camera | Toolbar or menu |
| Global | Undo / Redo | `Ctrl/Cmd+Z`, `Ctrl/Cmd+Shift+Z` |
| Global | Delete selected objects | `Delete` or `Backspace` |

### 12.2 Command-Line Flags

| Flag | Description |
| --- | --- |
| `--atlas_block_scene_3d_apply` | Block scene loading until 3D view settings finish applying. |
| `--run_export_3d_animation` | Enter headless animation export mode; requires accompanying export flags. |
| `--filename` | Path to `.animation3d` file for CLI export. |
| `--output_filename` | Output video path (mp4, etc.). |
| `--output_fps`, `--output_start_frame`, `--output_end_frame` | Output frame timing. |
| `--output_width`, `--output_height` | Frame size. |
| `--overwrite` | Allow overwriting existing outputs. |
| `--output_image_folder_name` | Directory for per-frame exports. |
| `--skip_video_compression` | Render frames only, skip final video encoding. |
| `--output_image_name_prefix`, `--output_image_name_field_width` | Control image sequence naming. |
| `--output_tile_size`, `--output_tile_border` | Enable tiled rendering for high-resolution outputs. |
| `--limit_memory_usage_in_gb_to` | Cap GPU memory usage (GB). |
| `--use_gpu_devices` | Specify GPU indices (Linux). |
| `--__use_EGL` | Force EGL context creation (Linux headless). |
| `--v=LEVEL` | Adjust log verbosity; `--v=1` prints additional diagnostics. |

### 12.3 File Format Support at a Glance

- **Images** – Standard scientific formats via `ZImg` (TIFF/OME-TIFF, LSM, V3DRAW, MHD/RAW, PNG, JPG, EXR, BMP, NIfTI).
- **Meshes** – OBJ, PLY, STL, OFF, VTK, GIfTI (verify on load via logs).
- **SWC** – SWC/eSWC variations; duplicates avoided through canonical path checks.
- **Puncta** – APO and CSV point sets.
- **ROI/Annotations** – Native ROI/annotation formats plus conversions from mask/label images.
- **Animations** – `.animation2d`, `.animation3d` JSON structures.

Always consult log output for unsupported file types; Atlas reports when a document cannot read a file.

### 12.4 Glossary

- **Alias** – A secondary handle to an object sharing the same data but with independent view settings.
- **Document** – Class deriving from `ZObjDoc`, responsible for managing a specific object type.
- **Scene** – Serialized workspace containing all loaded objects and view states.
- **ROI (Region of Interest)** – User-defined subset of an image for focused analysis.
- **Puncta** – Point-based annotations (e.g., synapses).
- **SWC** – Skeleton file format for neuronal structures.
- **Full Resolution Rendering** – Streaming high-resolution image blocks to the GPU for detailed volume rendering.
- **Tiled Rendering** – Splitting the renderable region into tiles to overcome GPU limits.

---

> 📸 **Screenshot to add:** Closing image showing a completed workspace with 2D and 3D windows side-by-side, annotated with key features referenced in this manual.
