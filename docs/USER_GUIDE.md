Atlas User Guide

Overview

- Atlas is an interactive 2D/3D visualization and analysis application with support for images, meshes, trees (SWC), puncta, ROIs, SVG, and 3D animation.
- You can manage objects, customize 2D/3D views, save/restore scenes, and export animations (GUI and CLI/headless).

Quick Start

1) Build and launch Atlas (see Installation in `readme.md`).
2) Drag data files into the main window or use File menu actions to load.
3) Open the 3D window via View → Open 3D Window.
4) Adjust view settings, take screenshots, or export animations.

Core Concepts

- Objects: Data items managed by Atlas (e.g., images, meshes, SWCs, puncta, ROIs, animations). Each object has an ID.
- Documents: Each object type has a document (e.g., `ZImgDoc`, `ZMeshDoc`) that owns objects and actions.
- Views: 2D view (main window) and 3D view (separate 3D window). Each object has a 2D view state and, if applicable, a 3D view state.
- Scene: A `.scene` file stores the set of loaded objects and their 2D/3D view settings.
- Animation: A `.animation3d` file stores animation timelines for global and per-object parameters.

User Interface

- Main window
  - Object Manager (dock): add/remove/select objects and open edit widgets.
  - 2D View: interact with data in 2D.
- 3D window
  - Canvas: 3D viewport (OpenGL-based).
  - Docks: Global View Setting, Object View Setting, Background, Axis, Capture, Help.
  - Menus/Toolbars: Zoom, Reset Camera, Screenshot, Animation tools.

Loading and Saving Scenes (.scene)

- Load: File → Load Scene… or drag a `.scene` into Atlas. The app loads objects and applies 2D and 3D settings.
- Save: File → Save Scene… to write all objects and their view settings.
- Deterministic 3D apply (optional): launch with `--atlas_block_scene_3d_apply` to block until all 3D parameters are applied by the rendering engine. Without it, 3D applies asynchronously as views become ready.
- Diagnostics: When 3D settings finish applying, Atlas logs: “3D scene parameters applied”.

3D View Controls

- Zoom: mouse wheel; or Command/Ctrl + +/- keys.
- Rotate: drag with mouse; or Command/Ctrl + arrow keys.
- Pan: Shift + drag; or Shift + arrow keys.
- Roll: Alt + drag; or Alt + Left/Right.
- Background/Axis: open the corresponding docks to change settings.
- Screenshots: use the Capture dock for single-frame captures (mono or stereo variants).

Loading 3D Animations (.animation3d)

- Load via the object manager or dedicated menu. Animation parameters bind to the current 3D view settings.
- The first scene is shown; if objects become ready after the animation loads, Atlas binds them and applies the current animation time automatically.
- Diagnostics: When animation parameters first bind, Atlas logs once: “3D animation parameters bound”.

Exporting 3D Animations (GUI)

- Open the Capture/Export UI and choose:
  - Output size (or use canvas size)
  - Frame rate (FPS), start/end frame
  - Output mode (mono, half/full side-by-side stereo)
  - Optional tiled rendering (for very large resolutions)
- Start export. Progress is shown and can be canceled.

Headless/Server Export (CLI)

- Enable CLI mode: `--run_export_3d_animation`
- Required flags:
  - `--filename <file.animation3d>`
  - `--output_filename <file.mp4>`
  - `--output_fps <int>`
  - `--output_start_frame <int>` (or `--output_start_time` deprecated)
  - `--output_end_frame <int>` (or `--output_end_time` deprecated)
  - `--output_width <int>` `--output_height <int>`
- Optional flags:
  - `--overwrite` (replace output)
  - `--output_image_folder_name <folder>` (emit frames to folder)
  - `--skip_video_compression` (skip ffmpeg; combine later)
  - `--output_image_name_prefix <str>`
  - `--output_image_name_field_width <int>`
  - `--output_tile_size <int>` `--output_tile_border <int>` (tiled rendering)
  - `--limit_memory_usage_in_gb_to <N>`
  - Linux: `--use_gpu_devices 0,1,...` and EGL (`--__use_EGL`)

Troubleshooting

- Partial 3D apply after scene load
  - Atlas queues 3D state and applies it when object views are ready. For deterministic load, use `--atlas_block_scene_3d_apply`.
- Closing 3D window during animation playback crashed
  - Fixed: the 3D canvas guards engine access against late signals during teardown.
- Animation not reflecting settings immediately
  - Fixed: parameter updates run on the engine thread; late-linked objects are synced to the current time.

Logging Tips

- Set verbosity: `--v=1` (or higher) to enable `VLOG` messages.
- Look for “3D scene parameters applied” and “3D animation parameters bound” for scene/animation status.

