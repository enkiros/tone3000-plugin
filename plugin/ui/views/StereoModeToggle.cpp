#include "StereoModeToggle.h"

#include "core/CustomIcons.h"
#include "core/Help.h"
#include "core/Icons.h"
#include "core/Paint.h"
#include "core/Theme.h"
#include "widgets/Clickable.h"

namespace t3k::ui {

namespace {
// The highlight snaps to the selected side, inset 4px from the outer edge;
// each segment is highlight-width so its icon centres under it and takes the
// whole segment (40x40 on touch) as its hit target.
constexpr int kOuterInset = 4;
constexpr int kHighlightWidth = StereoModeToggle::kWidth / 2 - kOuterInset;
constexpr int kHighlightHeight = StereoModeToggle::kHeight - 2 * kOuterInset;
constexpr int kGlyph = theme::kTouch ? 14 : 12;
}  // namespace

class StereoModeToggle::Segment : public Clickable {
public:
  Segment(const char* svg, float glyphWidth, help::Key helpKey)
      : Clickable({}), svg_(svg), glyphWidth_(glyphWidth) {
    setHelpText(help::text(helpKey));
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
  }

  void setSelected(bool selected) {
    selected_ = selected;
    repaint();
  }

  void paintButton(juce::Graphics& g, bool, bool) override {
    const auto glyph = juce::Rectangle<float>(glyphWidth_, static_cast<float>(kGlyph))
                           .withCentre(getLocalBounds().toFloat().getCentre());
    Icons::draw(g, svg_, glyph, selected_ ? theme::kWhite : theme::kGray);
  }

private:
  const char* svg_;
  float glyphWidth_;
  bool selected_ = false;
};

StereoModeToggle::StereoModeToggle()
    : mono_(std::make_unique<Segment>(custom_icons::kCircleBold, kGlyph, help::Key::monoMode)),
      stereo_(std::make_unique<Segment>(custom_icons::kStereoCircles, kGlyph + kGlyph * 0.45f,
                                        help::Key::stereoMode)) {
  mono_->onClick = [this] { if (onToggle) onToggle(false); };
  stereo_->onClick = [this] { if (onToggle) onToggle(true); };
  addAndMakeVisible(*mono_);
  addAndMakeVisible(*stereo_);
  setStereoEnabled(false);
  setSize(kWidth, kHeight);
}

StereoModeToggle::~StereoModeToggle() = default;

void StereoModeToggle::setStereoEnabled(bool enabled) {
  stereoEnabled_ = enabled;
  mono_->setSelected(!enabled);
  stereo_->setSelected(enabled);
  repaint();
}

void StereoModeToggle::paint(juce::Graphics& g) {
  const auto box = getLocalBounds().toFloat();
  paint::fill(g, box, kHeight / 2.0f, theme::kSurfaceRaised);
  const float x = kOuterInset + (stereoEnabled_ ? kHighlightWidth : 0);
  paint::fill(g, {x, static_cast<float>(kOuterInset), static_cast<float>(kHighlightWidth),
                  static_cast<float>(kHighlightHeight)},
              kHighlightHeight / 2.0f, theme::kHighlight);
}

void StereoModeToggle::resized() {
  mono_->setBounds(kOuterInset, 0, kHighlightWidth, kHeight);
  stereo_->setBounds(kOuterInset + kHighlightWidth, 0, kHighlightWidth, kHeight);
}

}  // namespace t3k::ui
