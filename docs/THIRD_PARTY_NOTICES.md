# Third-Party Notices

Atlas includes and/or redistributes third-party software. License texts and required notices are shipped with deployed app bundles; for source checkouts, third-party license files live under `src/3rdparty/`.

## Where to find license texts

- **Deployed app bundles**: license texts are copied into the app under `Resources/licenses/` (platform-specific location; see below).
  - macOS: `Atlas.app/Contents/Resources/licenses/`
  - Linux: `Atlas.AppDir/Resources/licenses/`
  - Windows: `Atlas/Resources/licenses/`

## Runtime binary sources

- FFmpeg binaries are sourced from:
  - macOS: https://www.osxexperts.net/
  - Linux/Windows: https://github.com/BtbN/FFmpeg-Builds/releases
- JRE binaries are sourced from Eclipse Temurin (Adoptium): https://adoptium.net/temurin/releases/
- Vulkan SDK binaries/tools are sourced from LunarG: https://vulkan.lunarg.com/sdk/home
