Scene and Animation: User Guide

Loading a Scene (.scene)

- Use File → Load Scene… or drag a `.scene` file into the app.
- The app loads all objects and 2D/3D view settings. If 3D view is needed, the 3D window opens automatically.
- For very large scenes, 3D settings may apply asynchronously as 3D views become ready.

Make load synchronous (optional)

- Launch with the flag `--atlas_block_scene_3d_apply` to make scene load wait until all 3D settings are applied (slower but deterministic).

Saving a Scene

- Use File → Save Scene… to write all objects and their 2D/3D visualization settings into a `.scene` file.

Loading a 3D Animation (.animation3d)

- Load an `.animation3d` via the object manager or the 3D animation menu.
- Once loaded, the animation parameters bind to the 3D renderer; the first scene is shown.
- If some objects load after the animation file, the app binds them when ready and applies the current time to keep the visual state consistent.

Exporting a 3D Animation (GUI)

- Use the animation export widget:
  - Choose video size and FPS.
  - Choose mono or stereo (half/full SBS).
  - Press export; progress is shown, and you can cancel.

Headless/Server Export (CLI)

- Run with `--run_export_3d_animation` and arguments:
  - `--filename <file.animation3d>`
  - `--output_filename <file.mp4>`
  - `--output_fps`, `--output_width`, `--output_height`
  - Optional tiled rendering: `--output_tile_size`, `--output_tile_border`
  - Optional: `--skip_video_compression` and `--output_image_folder_name` to inspect frames
- On Linux: `--use_gpu_devices 0,1,...` to distribute frames; EGL can be used for offscreen context.

Troubleshooting

- “Partially applied 3D state after load”
  - The app now queues 3D settings and applies them when 3D views are ready. If you prefer to wait until finishing, enable `--atlas_block_scene_3d_apply`.
- “3D window closed during playback” crash
  - Fixed: the 3D canvas now guards engine access against late signals during teardown.
- “Animation doesn’t reflect settings immediately”
  - Fixed: parameters update on the rendering thread and newly ready objects snap to the current time.

Useful Logs

- When scene 3D settings finish applying: “3D scene parameters applied”.
- When animation parameters first bind: “3D animation parameters bound”.

