// Shared theme tokens (port of theme.ts): black surfaces, white/gray chrome
// and three brand accents. Every colour/size literal in the UI comes from
// here, so the palette changes in one place.
#pragma once

#include <juce_graphics/juce_graphics.h>

#include "Design.h"

namespace t3k::ui::theme {

// Coarse-pointer (touch) builds enlarge chrome controls to ~32px targets;
// desktop keeps the compact 20px chrome.
inline constexpr bool kTouch = design::kCoarsePointer;

// Lucide / custom glyph size inside ICON_BOX_SIZE chrome boxes.
inline constexpr int kIconSize = kTouch ? 22 : 14;
// Square hit-target for icon buttons beside knobs and in card headers.
inline constexpr int kIconBoxSize = kTouch ? 32 : 20;
// Corner radius for every icon/text chrome box.
inline constexpr float kIconBoxRadius = kTouch ? 4.0f : 2.0f;
// Height for text chrome (EQ, PRE, LITE/FULL segments).
inline constexpr int kTextBoxHeight = kTouch ? 32 : 20;

// Text-chrome (EQ / PRE / segmented) font and side pad grow with the box.
inline constexpr float kTextChromeFontPx = kTouch ? 16.0f : 12.0f;
inline constexpr int kTextChromePadX = kTouch ? 6 : 4;

// Block-card header must fit the taller chrome.
inline constexpr int kCardHeaderHeight = kTouch ? 56 : 45;

// Top-bar chrome (PluginHeader, PresetBar, StereoModeToggle). The bar keeps
// its 64px height on touch builds (PluginHeader::kHeight), so the targets grow
// inside it instead of pushing the main screen down. 40px is the finger-sized
// box for the primary actions; desktop keeps the compact 28px box.
inline constexpr int kTopBarButtonSize = kTouch ? 40 : 28;
// Preset pill: the touch pill is taller and wider, the extra width paying for
// ‹ / › hit zones a finger can hit (30px rather than 22px).
inline constexpr int kPresetPillWidth = kTouch ? 230 : 202;
inline constexpr int kPresetPillHeight = kTouch ? 40 : 36;
inline constexpr int kPresetChevronWidth = kTouch ? 30 : 22;
inline constexpr int kPresetChevronGlyph = kTouch ? 18 : 14;
// Mono/stereo segmented pill; each segment is half the track, so the touch
// segments are 40x40 hit targets.
inline constexpr int kStereoToggleWidth = kTouch ? 88 : 80;
inline constexpr int kStereoToggleHeight = kTouch ? 40 : 36;

// The two knob footprints; every knob is one of these. Deck popovers and the
// pan rail keep these compact sizes (their fixed panels are sized to them);
// the faceplate uses the enlarged touch footprints below instead.
inline constexpr int kKnobSizePrimary = 48;
inline constexpr int kKnobSizeSecondary = 36;

// Faceplate knob footprints. Touch builds enlarge these for the 7" kiosk
// display and the faceplate grows to match (Design.h kPlateHeight); the deck
// popovers and the pan rail keep the compact sizes above.
inline constexpr int kKnobSizeFacePrimary = kTouch ? 60 : 48;
inline constexpr int kKnobSizeFaceSecondary = kTouch ? 45 : 36;

// The one dim applied to every disabled/off control (`.ui-off`).
inline constexpr float kDisabledOpacity = 0.45f;

// Knob-to-label gap.
inline constexpr int kKnobLabelGap = 8;

// Vertical lift for a chrome icon box in a bottom-aligned faceplate row: from
// the label baseline up to the centre of a secondary knob. (19 == Knob::kLabelSlot,
// the label-slot height the knob column reserves above the baseline.)
inline constexpr int faceplateChromeLift(int secondaryKnobSize) {
  return -(kKnobLabelGap + 19 + secondaryKnobSize / 2 - kIconBoxSize / 2);
}

inline const juce::Colour kBrandBlue{0xff0000ff};
inline const juce::Colour kBrandYellow{0xffffff00};
inline const juce::Colour kBrandRed{0xffff0000};
// Inline doc / "Learn More" links in settings.
inline const juce::Colour kLinkBlue{0xff40a6ff};

inline const juce::Colour kWhite{0xffffffff};
inline const juce::Colour kBlack{0xff000000};

// rgba(235, 235, 245, a) family: primary muted text, secondary labels,
// pressed/active fill.
inline const juce::Colour kMuted = juce::Colour(235, 235, 245).withAlpha(0.60f);
inline const juce::Colour kSubtle = juce::Colour(235, 235, 245).withAlpha(0.40f);
inline const juce::Colour kHighlight = juce::Colour(235, 235, 245).withAlpha(0.18f);
// Disabled/idle icon gray.
inline const juce::Colour kGray{0xff8d8d93};
// Hairline used by every card/section/segment border (1px).
inline const juce::Colour kBorder = juce::Colour(84, 84, 88).withAlpha(0.65f);
// Card body background / raised chrome.
inline const juce::Colour kSurface{0xff151517};
inline const juce::Colour kSurfaceRaised{0xff1c1c1e};
// Shared fill behind LITE/FULL and the EQ view switcher.
inline const juce::Colour kSegmentedTrack = juce::Colour(120, 120, 128).withAlpha(0.36f);
// Floating panels (popovers, menus, the image deck): #141416, hairline
// border, 14px corners.
inline const juce::Colour kPanelBg{0xff141416};
inline constexpr float kPanelCorner = 14.0f;

}  // namespace t3k::ui::theme
