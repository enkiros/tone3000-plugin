# Raspberry Pi OS (Trixie) — Wayland on-screen keyboard

TONE3000's standalone build runs natively on Wayland (JUCE's `wayland`
preview branch) and drives Raspberry Pi OS's on-screen keyboard
(**squeekboard**) through the standard Wayland text-input protocol. This page
covers building, launching, and verifying that on a Raspberry Pi 5 kiosk.

## How it works

- The standalone app uses JUCE's Wayland backend when `WAYLAND_DISPLAY` is
  set (plugin windows stay on X11/XWayland by JUCE design — no OSK there).
- Focusing any text field enables `zwp_text_input_v3`; labwc forwards the
  request to squeekboard (input-method-v2), which shows the keyboard. Tapping
  keys commits text back through the same surface.
- squeekboard is an **overlay** — it covers the bottom ~205 px of the 1024×600
  panel instead of resizing the window. The UI keeps a focused text field
  scrolled above it via the `T3K_OSK_HEIGHT` inset (see below).

## Prerequisites (already present on RPiOS Trixie)

- Compositor `labwc` (0.20.x) advertising `zwp_text_input_manager_v3`,
  `zwp_input_method_manager_v2`, `zwp_virtual_keyboard_manager_v1`.
- `squeekboard`, enabled from **Control Centre → On-screen Keyboard**. That
  writes `/etc/xdg/autostart/squeekboard.desktop` (which runs `/usr/bin/sbtest`
  and starts squeekboard when a touch device is present) and sets
  `SQUEEKBOARD_PREFERRED_OUTPUT`. It must be running **before** a field asks
  for input — a fresh login starts it automatically.

## Build

```sh
cd /path/to/tone3000-plugin
git submodule update --init --recursive
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/linux-toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_AAX=OFF -DBUILD_LV2=OFF -DBUILD_CLAP=OFF
cmake --build build --target TONE3000_Standalone -j4
# binary: build/plugin/TONE3000_artefacts/RelWithDebInfo/Standalone/TONE3000
```

## Run (from an SSH shell, or a session launcher)

```sh
export XDG_RUNTIME_DIR=/run/user/1000
export WAYLAND_DISPLAY=wayland-0
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
export T3K_OSK_HEIGHT=220        # keep focused fields above the keyboard

build/plugin/TONE3000_artefacts/RelWithDebInfo/Standalone/TONE3000
```

`T3K_OSK_HEIGHT` is the keyboard's on-screen height in **design px** (editor
scale 1). `0` (default) leaves desktop behaviour unchanged; the Pi kiosk uses
`220` (squeekboard's 4-row layout measures ~205 at 1024 wide). Only the
focused `TextEditor` is affected — `DragScroller` scrolls it above the
keyboard on focus; other controls keep their normal focus margin.

## Verify

```sh
# OSK is running and (once a field is focused) visible
pgrep -x squeekboard
busctl --user get-property sm.puri.OSK0 /sm/puri/OSK0 sm.puri.OSK0 Visible

# Text-input protocol traffic (run the app with WAYLAND_DEBUG=1)
grep -E 'zwp_text_input_v3#.*(enable|disable|set_cursor_rectangle|commit_string)' /tmp/app.log

# Screenshot of the live session
grim /tmp/osk.png
```

Expected: `enable` + `set_cursor_rectangle` on focus (the caret y should sit
above ~373 so the field is not under the keyboard), `commit_string` per key,
`disable` on blur, and `Visible` flipping `false ↔ true`.

## Troubleshooting

- **No keyboard appears.** Confirm squeekboard is running before the app
  (`pgrep -x squeekboard`) and `WAYLAND_DISPLAY` is set in the app's
  environment. `WAYLAND_DEBUG=1` on the app shows whether
  `zwp_text_input_manager_v3` is bound and `enable` is sent.
- **Field hidden behind the keyboard.** Raise `T3K_OSK_HEIGHT` (the value is
  the occluded band); the field scrolls up further on focus.
- **Falls back to X11.** `WAYLAND_DISPLAY` must be set *and* the app must be
  the standalone target (plugin formats are X11-only by design).
