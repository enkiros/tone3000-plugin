#include "VirtualKeyboard.h"

#include "widgets/TextField.h"
#include "core/Paint.h"
#include "core/Fonts.h"

namespace t3k::ui {

// ============================================================================
// KeyLayout Data - Using explicit struct initialization
// ============================================================================

static const std::vector<KeyLayout> buildTopRow() {
  KeyLayout k;
  k.baseChar = '`';
  k.altChar = '~';
  k.isWide = false;
  k.isAction = false;
  return {{k}};
}
const std::vector<KeyLayout> VirtualKeyboard::topRow = buildTopRow();

static const std::vector<KeyLayout> buildHomeRow() {
  KeyLayout k1;
  k1.baseChar = 'Q'; k1.altChar = 'W'; k1.isWide = false; k1.isAction = false;
  KeyLayout k2; k2.baseChar = 'E'; k2.altChar = 'R'; k2.isWide = false; k2.isAction = false;
  KeyLayout k3; k3.baseChar = 'T'; k3.altChar = 'Y'; k3.isWide = false; k3.isAction = false;
  KeyLayout k4; k4.baseChar = 'U'; k4.altChar = 'I'; k4.isWide = false; k4.isAction = false;
  KeyLayout k5; k5.baseChar = 'O'; k5.altChar = 'P'; k5.isWide = false; k5.isAction = false;
  KeyLayout k6; k6.baseChar = '~'; k6.altChar = 0; k6.isWide = true; k6.isAction = false;
  
  return {k1, k2, k3, k4, k5, k6};
}
const std::vector<KeyLayout> VirtualKeyboard::homeRow = buildHomeRow();

static const std::vector<KeyLayout> buildBottomRow() {
  KeyLayout k1; k1.baseChar = 'A'; k1.altChar = 'S'; k1.isWide = false; k1.isAction = false;
  KeyLayout k2; k2.baseChar = 'D'; k2.altChar = 'F'; k2.isWide = false; k2.isAction = false;
  KeyLayout k3; k3.baseChar = 'G'; k3.altChar = 'H'; k3.isWide = false; k3.isAction = false;
  KeyLayout k4; k4.baseChar = 'J'; k4.altChar = 'K'; k4.isWide = false; k4.isAction = false;
  KeyLayout k5; k5.baseChar = 'L'; k5.altChar = 0; k5.isWide = false; k5.isAction = false;
  KeyLayout k6; k6.baseChar = 'B'; k6.altChar = 0; k6.isWide = false; k6.isAction = true;
  
  return {k1, k2, k3, k4, k5, k6};
}
const std::vector<KeyLayout> VirtualKeyboard::bottomRow = buildBottomRow();

static const std::vector<KeyLayout> buildBottomRow2() {
  KeyLayout k1; k1.baseChar = 'Z'; k1.altChar = 'X'; k1.isWide = false; k1.isAction = false;
  KeyLayout k2; k2.baseChar = 'C'; k2.altChar = 'V'; k2.isWide = false; k2.isAction = false;
  KeyLayout k3; k3.baseChar = 'B'; k3.altChar = 'N'; k3.isWide = false; k3.isAction = false;
  KeyLayout k4; k4.baseChar = 'M'; k4.altChar = 0; k4.isWide = false; k4.isAction = false;
  KeyLayout k5; k5.baseChar = ' '; k5.altChar = 0; k5.isWide = false; k5.isAction = false;
  KeyLayout k6; k6.baseChar = 'E'; k6.altChar = 0; k6.isWide = true; k6.isAction = true;
  
  return {k1, k2, k3, k4, k5, k6};
}
const std::vector<KeyLayout> VirtualKeyboard::bottomRow2 = buildBottomRow2();

// ============================================================================
// KeyButton Implementation
// ============================================================================

KeyButton::KeyButton(const KeyLayout& layout, const std::string& displayText, int row)
    : layout_(layout), displayText_(displayText), row_(row), shiftDown_(false), pressed_(false) {
  setWantsKeyboardFocus(false);
}

void KeyButton::paint(juce::Graphics& g) {
  auto bounds = getLocalBounds().toFloat();
  
  const float padding = 4.0f;
  
  if (pressed_) {
    g.setColour(juce::Colour(0xff3a3a3e));
  } else {
    g.setColour(juce::Colour(0xff1c1c1e));
  }
  
  juce::Rectangle<float> rect(bounds.getX() + padding, bounds.getY() + padding, 
                               bounds.getWidth() - 2*padding, 
                               bounds.getHeight() - 2*padding);
  
  g.fillRoundedRectangle(rect, 6.0f);
  
  g.setColour(pressed_ ? juce::Colour(0xff8e8e93) : juce::Colour(0xff3a3a3e));
  auto borderRect = juce::Rectangle<float>(bounds.getX() + padding, bounds.getY() + padding, 
                                          bounds.getWidth() - 2*padding, 
                                          bounds.getHeight() - 2*padding);
  g.drawRoundedRectangle(
    static_cast<int>(borderRect.getX()), 
    static_cast<int>(borderRect.getY()), 
    static_cast<int>(borderRect.getWidth()), 
    static_cast<int>(borderRect.getHeight()), 
    6.0f, 2.0f);
  
  auto textBounds = getLocalBounds().toFloat();
  
  juce::String textStr;
  if (layout_.isAction) {
    textStr = displayText_;
  } else {
    textStr = std::string(1, layout_.baseChar);
  }
  
  auto font = Fonts::sans(24.0f, shiftDown_);
  
  auto stringWidth = juce::GlyphArrangement::getStringWidth(font, textStr);
  auto stringHeight = font.getHeightInPoints();
  
  float textX = textBounds.getCentreX() - stringWidth / 2.0f;
  float textY = textBounds.getCentreY() + (font.getAscent() + font.getDescent()) * 0.5f;
  
  g.setColour(juce::Colour(0xffFFFFFF));
  g.setFont(font);
  g.drawText(textStr, juce::Rectangle<int>(textX, textY - font.getAscent(), 
                                            (int)stringWidth, (int)stringHeight),
             juce::Justification::centred);
}

void KeyButton::mouseDragged(const juce::MouseEvent&) {}

void KeyButton::mouseEnter(const juce::MouseEvent&) {
  repaint();
}

void KeyButton::mouseLeftTheComponent(const juce::MouseEvent&) {
  repaint();
}

// ============================================================================
// VirtualKeyboard Implementation
// ============================================================================

VirtualKeyboard::VirtualKeyboard(TextField& /*owner*/, KeyCallback onKey, EnterCallback onEnter, EscapeCallback onEscape)
    : onKey_(std::move(onKey)), onEnter_(std::move(onEnter)), onEscape_(std::move(onEscape)) {
  setWantsKeyboardFocus(false);
}

void VirtualKeyboard::setVisible(bool visible) {
  if (visible != visible_) {
    visible_ = visible;
    if (visible_) {
      show();
    } else {
      hide();
    }
  }
}

void VirtualKeyboard::show() {
  if (isVisible()) return;
  setVisible(true);
}

void VirtualKeyboard::hide() {
  for (auto& key : keys_) {
    key->setVisible(false);
  }
  setVisible(false);
}

void VirtualKeyboard::paint(juce::Graphics& g) {
  const auto box = getLocalBounds().toFloat();
  g.setColour(juce::Colour(0x88242428));
  g.fillRect(box);
}

void VirtualKeyboard::resized() {
  keys_.clear();
  
  const std::vector<KeyLayout>* currentRow = &topRow;
  
  for (int r = 0; currentRow != nullptr; r++) {
    // Create keys for non-wide, non-action keys
    for (const auto& key : (*currentRow)) {
      if (!key.isWide && !key.isAction) {
        keys_.push_back(std::make_unique<KeyButton>(key, "", r));
      }
    }
    
    // Move to next row
    switch(r) {
      case 0: currentRow = &homeRow; break;
      case 1: currentRow = &bottomRow; break;
      case 2: currentRow = &bottomRow2; break;
      default: currentRow = nullptr; break;
    }
  }
}

void VirtualKeyboard::mouseDown(const juce::MouseEvent& e) {
  for (auto& key : keys_) {
    if (!key->isEnabled()) continue;
    
    auto bounds = key->getBounds().toFloat();
    int x = e.getPosition().getX(), y = e.getPosition().getY();
    if (bounds.contains(x, y)) {
      repaint();
      
      const auto& layout = key->getLayout();
      processKeyPress(key->isShifted() ? layout.altChar : layout.baseChar);
      return;
    }
  }
}

void VirtualKeyboard::mouseUp(const juce::MouseEvent&) {}

void VirtualKeyboard::mouseEnter(const juce::MouseEvent&) {}

void VirtualKeyboard::mouseDragged(const juce::MouseEvent&) {}

void VirtualKeyboard::mouseLeftTheComponent(const juce::MouseEvent&) {}

void VirtualKeyboard::processKeyPress(char c) {
  if (!visible_) return;
  
  if (onKey_) onKey_(c);
  
  if (shiftDown_) hide();
}

}  // namespace t3k::ui
