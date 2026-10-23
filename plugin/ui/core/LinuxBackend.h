// Startup guard for the Linux display backend.
//
// JUCE picks between Wayland and X11 from the environment:
// `WaylandWindowSystem::shouldUseWaylandBackend()` requires a standalone app
// with WAYLAND_DISPLAY set, and JUCE's X11 path defaults DISPLAY to ":0.0"
// when it is unset. An app launched from a bare SSH shell therefore lands on
// XWayland while the labwc session is right there — and X11 has no
// zwp_text_input_v3, so the compositor's on-screen keyboard never appears.
//
// Kiosk builds fill WAYLAND_DISPLAY in from $XDG_RUNTIME_DIR before JUCE
// touches a display (see the pre-main initialiser in LinuxBackend.cpp) and
// the editor logs which backend it ended up on; NativeEditor also warns on
// screen when the guard could not rescue the session.
#pragma once

#include <juce_core/juce_core.h>

#include <functional>

namespace t3k::ui::linuxbackend {

// How many wayland-N sockets to probe when WAYLAND_DISPLAY is unset.
inline constexpr int kMaxDiscoveredDisplays = 3;

// Chooses the display name to hand JUCE. `currentValue` wins whenever it is
// non-empty — an explicit WAYLAND_DISPLAY (bare name or absolute socket path)
// is never overridden. Otherwise the first existing `wayland-N` socket in
// `runtimeDir` is returned, or "" to leave JUCE's default alone.
//
// `socketExists` receives the full socket path so unit tests can drive the
// probe without touching the filesystem.
juce::String discoverWaylandDisplay(const juce::String& currentValue,
                                    const juce::String& runtimeDir,
                                    const std::function<bool(const juce::String&)>& socketExists);

#if JUCE_LINUX || JUCE_BSD
// If WAYLAND_DISPLAY is unset and a compositor socket exists, puts it in the
// environment (kiosk builds only; a no-op elsewhere). Returns the display now
// configured, or "" when nothing was selected. Safe to call before main():
// it only reads the environment, stats the socket and writes to stderr.
juce::String ensureWaylandDisplay();

// True when WAYLAND_DISPLAY names a socket that exists, i.e. JUCE will choose
// its Wayland backend (it additionally requires a standalone app and a
// successful connect). Used for the startup log and the kiosk warning.
bool usingWaylandDisplay();
#else
inline juce::String ensureWaylandDisplay() { return {}; }
inline bool usingWaylandDisplay() { return false; }
#endif

}  // namespace t3k::ui::linuxbackend
