// The fixed design space every native component is laid out in (port of
// useUiScale.ts constants). One unit here is one design px; the
// editor scales the whole root with a single AffineTransform, so components
// never see the window size.
#pragma once

#include <cmath>

#include <juce_core/system/juce_TargetPlatform.h>

namespace t3k::ui::design {

// Pixel snapping the way Blink does it: half away from zero, so a control
// centred at 10.5 lands on 11. juce::roundToInt rounds half to even (10.5
// gives 10), which is one pixel off for every odd centring gap.
inline int snap(float v) { return static_cast<int>(std::lround(v)); }

inline constexpr int kWidth = 1024;
// Core UI height (content only). The Figma canvas is 600, of which 22px is a
// mock title bar. The 1024x600 kiosk has no title bar, so the kiosk build
// (T3K_TOUCH_UI, which defines T3K_UI_COARSE_POINTER) reclaims it and lays
// out 600 tall. Everything else stays 578 (desktop/iOS/Android still have OS
// chrome or letterbox). Keyed on the macro, not kCoarsePointer: iOS/Android
// reach kCoarsePointer via JUCE_IOS/JUCE_ANDROID below without defining the
// macro, so they keep 578.
#if defined(T3K_UI_COARSE_POINTER)
inline constexpr int kHeight = 600;
#else
inline constexpr int kHeight = 578;
#endif

// Platform flags, known at compile time. kCoarsePointer is a touch
// platform (no hover, ever); it fixes the hint copy and seeds
// Services::pointer, which on a desktop build follows the input that
// arrives instead (a Windows or Linux tablet). T3K_UI_COARSE_POINTER
// overrides it for a touch build on a desktop.
#if JUCE_IOS
inline constexpr bool kIos = true;
#else
inline constexpr bool kIos = false;
#endif

#if defined(T3K_UI_COARSE_POINTER)
inline constexpr bool kCoarsePointer = T3K_UI_COARSE_POINTER;
#elif JUCE_IOS || JUCE_ANDROID
inline constexpr bool kCoarsePointer = true;
#else
inline constexpr bool kCoarsePointer = false;
#endif

// Chrome strips that grow the window instead of squishing the core.
inline constexpr int kBannerHeight = 44;   // AppBanner.tsx
inline constexpr int kHeaderHeight = 45;   // chainLayout.tsx
// The faceplate grows for the larger touch knobs (a 60px primary knob plus
// its 16px label column needs ~120px); desktop keeps 108. The main screen
// above the plate takes the difference.
inline constexpr int kPlateHeight = kCoarsePointer ? 120 : 108;  // Faceplate.tsx

// Editor window scale range (aspect-locked corner drags).
inline constexpr double kMaxScale = 2.0;

#if JUCE_MAC || JUCE_IOS
inline constexpr bool kAppleModifierGlyphs = true;
#else
inline constexpr bool kAppleModifierGlyphs = false;
#endif

}  // namespace t3k::ui::design
