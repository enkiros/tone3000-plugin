# AGENTS.md

JUCE/C++ audio plugin (VST3/AU/AAX/LV2/CLAP/Standalone) that loads Neural Amp
Modeler captures and IRs from TONE3000, with a fully native C++ UI (no browser
engine). Deep dives live in [`README.md`](README.md) (build/run, DSP signal
flow, settings stores), [`plugin/ui/README.md`](plugin/ui/README.md) (UI
architecture, testbed, conventions), and
[`docs/rpi-wayland-osk.md`](docs/rpi-wayland-osk.md) (native Wayland and the
on-screen keyboard). Read those before touching code; this file is only the
tripwires.

## Build — get these right or you'll waste a cycle

- **Submodules are required** and not auto-initialized:
  `git submodule update --init --recursive`
  (`plugin/NeuralAmpModelerCore`, `plugin/AudioDSPTools`).
- **`libs/` is gitignored and regenerated.** JUCE, GoogleTest, freetype,
  clap-juce-extensions are fetched by CPM into `libs/` on first configure.
  Never hand-edit or commit anything under `libs/`; your changes are discarded
  on the next configure. A `libs/juce` left at an older pin (with the T3K
  patches applied) is reset to the pinned SHA automatically at configure, then
  re-patched — a pin bump needs no manual sync.
- **Linux must use the toolchain file** (`cmake/linux-toolchain.cmake`), which
  pulls pkg-config + GTK3 and wires the static-FreeType block:
  ```sh
  cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=cmake/linux-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
  ```
- Format/target toggles: `-DHEADLESS=ON`, `-DBUILD_AAX=OFF`, `-DBUILD_LV2=OFF`,
  `-DBUILD_CLAP=OFF`, `-DT3K_BUILD_UI_TESTBED=ON`. The Pi kiosk adds
  `-DT3K_TOUCH_UI=ON` (enlarges knobs, chrome and the faceplate for coarse
  pointers; `kPlateHeight` grows 108→120 and the main screen shrinks to match).
- **On the Pi, build the standalone only:** `cmake --build build --target
  TONE3000_Standalone -j4` with the format toggles above; the first build is
  long on 4 GB RAM / 4 cores (drop to `-j2` if linking runs out of memory).
- **`VERSION` (repo root) is the version source of truth.** CMake, packaging
  scripts, and CI all read it; it auto-triggers reconfigure when edited. Don't
  bump versions in CMake files.
- **Config comes from `.env` → `.env.local` → configure env**, read at configure
  time into the generated `plugin/ui/T3kConfig.h` (`plugin/ui/NativeUi.cmake`).
  Changing `T3K_PUBLISHABLE_KEY` / `T3K_API_DOMAIN` / `T3K_UPDATE_NOTICE`
  requires a reconfigure, not just a rebuild.

## JUCE source patches (the biggest trap)

The root `CMakeLists.txt` patches JUCE **in place under `libs/juce` at configure
time**. Each patch is guarded by a `T3K_*` marker string so re-running is a
no-op, and a moved/missing anchor raises `FATAL_ERROR` at configure. JUCE is
pinned by commit SHA (the `wayland` preview branch, 9.0.5) — do not switch it
back to a branch ref. If you bump JUCE or a patch stops applying, re-audit the
anchors in `CMakeLists.txt`; do not "fix" the tree by editing `libs/juce`.

## Native Wayland and the on-screen keyboard

- **Backend selection is JUCE's.** `shouldUseWaylandBackend()` picks Wayland
  only for standalone apps with `WAYLAND_DISPLAY` set; plugin windows stay on
  X11/XWayland. Don't add a parallel Wayland plugin path.
- **OSK = compositor text-input, not an in-app keyboard.** The standalone
  binds `zwp_text_input_manager_v3`; labwc forwards it to squeekboard
  (input-method-v2). The abandoned in-app `VirtualKeyboard` (commit `44310c7`)
  was reverted — don't resurrect it.
- **Keep focused text fields above the OSK.** `T3K_OSK_HEIGHT` (design px,
  default `0`) is read by `plugin/ui/core/Keyboard.h`; `DragScroller::reveal`
  applies it as a bottom inset for a focused `juce::TextEditor`. The Pi kiosk
  sets ~`220`. Don't hardcode keyboard heights in views.
- **squeekboard must already be running** when a field gains focus. RPiOS's
  Control Centre writes `/etc/xdg/autostart/squeekboard.desktop`, which runs
  `/usr/bin/sbtest` (starts it only if a touch device is detected).
- **Debugging:** run with `WAYLAND_DEBUG=1` and look for
  `zwp_text_input_v3.enable` / `set_cursor_rectangle` / `commit_string`;
  `busctl --user get-property sm.puri.OSK0 /sm/puri/OSK0 sm.puri.OSK0 Visible`;
  `grim` for screenshots. Full guide:
  [`docs/rpi-wayland-osk.md`](docs/rpi-wayland-osk.md).

## Tests

- DSP suite: `./script/test-dsp.sh ['GtestFilter*']` — builds the `DspTests`
  target then runs it (uses the existing `build/`).
- Registered tests: `ctest --test-dir build/test`.
- **UI testbed** (`plugin/ui/testbed`) iterates on the UI without rebuilding
  DSP. The repo root must be configured once first (to fetch JUCE), then:
  ```sh
  cmake -S plugin/ui/testbed -B build-ui -DCMAKE_BUILD_TYPE=Debug
  cmake --build build-ui -j4
  ```
  Run the binary with `--selftest`, `--scenario <id>`, `--capture`,
  `--compare`, or `--bench`. Scenario ids come from
  `testbed/fixtures/scenarios.json`.
- **Linux UI self-tests need a display**: `xvfb-run -a ctest --test-dir build/plugin/ui/testbed`.
- Plugin format validation: `./script/validate-plugin.sh [VST3|AU|CLAP|LV2] [Debug]`.

## Conventions that differ from the obvious

- **Presets are binary.** `*.t3kpreset` files are magic-prefixed ValueTree
  streams (`.gitattributes` marks them binary). Never diff, merge, or edit them
  as text; regenerate via `tools/` (`PresetTool`).
- **UI is laid out in a fixed 1024×578 design space**; `NativeEditor` applies a
  single `AffineTransform`. Never scale per-view by hand. (The kiosk target is
  1024×600.)
- **UI data flow**: `NativeEditor` → `Services` → `PluginRoot` → views. Stores
  are the *only* callers of `Backend`; views subscribe to stores. No singletons
  or globals beyond `core/` constexpr tables.
- **Linux FreeType is static by design** (issue #181: hosts like Ardour/Mixbus
  bundle an old `libfreetype.so.6`; a dynamic import dies with
  `undefined symbol: FT_Get_Paint`). The custom pkgconfig block in
  `CMakeLists.txt` exists to keep `FT_*` symbols local — don't remove it to
  "simplify".
- **iOS configure is special**: pass `-DCMAKE_SYSTEM_NAME=iOS` on the command
  line (or use the `ios-simulator` / `ios-device` presets). A toolchain file
  sets it too late, and the build `FATAL_ERROR`s to prevent building desktop
  formats for iOS.

## Operational notes

- CI (`build.yml`) is dispatch-only and currently a single `Linux aarch64`
  matrix (`ubuntu-24.04-arm`); it enables the UI testbed and runs both the DSP
  and UI self-tests.
- `.env` / `.env.local` / `opencode.jsonc` are gitignored; `opencode.jsonc`
  points a `lmstudio` provider at a LAN host for local model use.
