// A compact QWERTY virtual keyboard designed for the 1024x600 embedded
// touchscreen interface. Shows as a bottom overlay when triggered by a
// TextField; occupies ~245px height with large 56x56px touch keys.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace t3k::ui {

class TextField;

// Key layout structure for each key
struct KeyLayout {
  char baseChar;        // Character for unshifted state (or keycap text)
  char altChar;         // Character for shifted state  
  bool isWide;          // Extra wide key (2x width)
  bool isAction;        // Action key (Enter, Backspace, Escape)
};

// Helper function to create KeyLayout with initializer list
inline KeyLayout makeKeyLayout(char base, char alt, bool wide, bool action) {
  KeyLayout k;
  k.baseChar = base;
  k.altChar = alt;
  k.isWide = wide;
  k.isAction = action;
  return k;
}

// Individual key button that paints itself with touch feedback.
class KeyButton : public juce::Component {
public:
  KeyButton(const KeyLayout& layout, const std::string& displayText, int row);
  ~KeyButton() = default;

  void setEnabled(bool enabled) { enabled_ = enabled; repaint(); }
  bool isEnabled() const { return enabled_; }
  const KeyLayout& getLayout() const { return layout_; }
  bool isShifted() const { return shiftDown_; }
  int getRow() const { return row_; }

private:
  void paint(juce::Graphics& g) override;
  void mouseDragged(const juce::MouseEvent&);
  void mouseEnter(const juce::MouseEvent&);
  void mouseLeftTheComponent(const juce::MouseEvent&);

  const KeyLayout& layout_;
  std::string displayText_;
  int row_ = 0;
  bool shiftDown_ = false, enabled_ = true, pressed_ = false;
};

// Virtual keyboard that appears below the main content area.
class VirtualKeyboard : public juce::Component {
public:
  using KeyCallback = std::function<void(char)>;
  using EnterCallback = std::function<void()>;
  using EscapeCallback = std::function<void()>;

  VirtualKeyboard(TextField& owner, KeyCallback onKey, EnterCallback onEnter, EscapeCallback onEscape);
  ~VirtualKeyboard() override = default;

  void setVisible(bool visible) override;
  void requestFocus() {}
  
  void show();
  void hide();
  bool isVisible() const { return visible_; }

private:
  void paint(juce::Graphics& g) override;
  void resized();
  void mouseDown(const juce::MouseEvent&);
  void mouseUp(const juce::MouseEvent&);
  void mouseEnter(const juce::MouseEvent&);
  void mouseDragged(const juce::MouseEvent&);
  void mouseLeftTheComponent(const juce::MouseEvent&);

  // Keyboard dimensions for the 1024x600 display.
  static constexpr float kKeyboardWidth = 740.0f;
  static constexpr float kKeyboardHeight = 245.0f;
  static constexpr float kKeySize = 56.0f;      // Square touch targets
  static constexpr float kKeyGap = 8.0f;        // Gap between keys
  static constexpr float kTitleGapTop = 4.0f;   // Gap above first row
  static constexpr float kRowGap = 12.0f;       // Vertical gap between rows

  KeyCallback onKey_;
  EnterCallback onEnter_;
  EscapeCallback onEscape_;

  bool visible_ = false, shiftDown_ = false;

  // Keyboard layout arrays - defined below in .cpp
  static const std::vector<KeyLayout> topRow;
  static const std::vector<KeyLayout> homeRow;
  static const std::vector<KeyLayout> bottomRow;
  static const std::vector<KeyLayout> bottomRow2;

  std::vector<std::unique_ptr<KeyButton>> keys_;

  void processKeyPress(char c);
};

}  // namespace t3k::ui
