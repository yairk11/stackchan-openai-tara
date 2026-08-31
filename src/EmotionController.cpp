#include "EmotionController.h"
#include "HebrewFont22.h"

#include <M5StackChan.h>
#include <M5Unified.h>

namespace {
constexpr uint8_t kLedCount = 12;
constexpr uint32_t kFrameIntervalMs = 35;  // ~28 FPS for LEDs; smooth, but light on CPU/audio.
constexpr uint32_t kFaceIntervalMs = 90;   // ~11 FPS for idle LCD; avoid stealing time from audio.
constexpr uint32_t kSpeakingFaceIntervalMs = 40;  // Stable LCD cadence while speaking; keep audio safe.
constexpr uint32_t kSleepDisplayOffMs = 30000;  // Anti-retention: show sleep face briefly, then blank LCD.

const TaraGlyph22* findTaraGlyph22(uint16_t codepoint) {
  for (size_t i = 0; i < taraGlyph22Count; ++i) {
    if (taraGlyph22[i].codepoint == codepoint) {
      return &taraGlyph22[i];
    }
  }
  return nullptr;
}

void drawTaraGlyph22(
    M5Canvas& canvas,
    int x,
    int y,
    uint16_t codepoint,
    uint16_t color) {
  const TaraGlyph22* glyph = findTaraGlyph22(codepoint);
  if (glyph == nullptr || glyph->width == 0 || glyph->height == 0) {
    return;
  }

  const uint8_t bytesPerRow = (glyph->width + 7) / 8;

  for (uint8_t gy = 0; gy < glyph->height; ++gy) {
    for (uint8_t gx = 0; gx < glyph->width; ++gx) {
      const size_t byteIndex =
          static_cast<size_t>(gy) * bytesPerRow + (gx / 8);

      const uint8_t value =
          pgm_read_byte(glyph->bitmap + byteIndex);

      const uint8_t mask = 0x80 >> (gx & 7);

      if (value & mask) {
        canvas.drawPixel(x + gx, y + gy, color);
      }
    }
  }
}
size_t decodeUtf8ToCodepoints(
    const String& text,
    uint16_t* output,
    size_t maxCount) {
  size_t count = 0;
  const uint8_t* p =
      reinterpret_cast<const uint8_t*>(text.c_str());

  while (*p != 0 && count < maxCount) {
    uint32_t codepoint = 0;

    if ((*p & 0x80) == 0) {
      codepoint = *p++;
    } else if ((*p & 0xE0) == 0xC0 && p[1] != 0) {
      codepoint =
          ((static_cast<uint32_t>(p[0] & 0x1F) << 6) |
           static_cast<uint32_t>(p[1] & 0x3F));
      p += 2;
    } else if ((*p & 0xF0) == 0xE0 &&
               p[1] != 0 &&
               p[2] != 0) {
      codepoint =
          ((static_cast<uint32_t>(p[0] & 0x0F) << 12) |
           (static_cast<uint32_t>(p[1] & 0x3F) << 6) |
           static_cast<uint32_t>(p[2] & 0x3F));
      p += 3;
    } else {
      ++p;
      continue;
    }

    if (codepoint <= 0xFFFF) {
      output[count++] = static_cast<uint16_t>(codepoint);
    }
  }

  return count;
}
int measureHebrewTextWidth(const String& text) {
  uint16_t codepoints[128];
  const size_t count =
      decodeUtf8ToCodepoints(text, codepoints, 128);

  int width = 0;

  for (size_t i = 0; i < count; ++i) {
    const TaraGlyph22* glyph = findTaraGlyph22(codepoints[i]);

    if (glyph != nullptr) {
      width += glyph->xAdvance;
    }
  }

  return width;
}

size_t wrapHebrewText(
    const String& text,
    int maxWidth,
    String* lines,
    size_t maxLines) {
  if (maxLines == 0) return 0;

  String words[64];
  size_t wordCount = 0;
  int start = 0;

  while (start < text.length() && wordCount < 64) {
    while (start < text.length() && text[start] == ' ') ++start;
    if (start >= text.length()) break;

    int end = text.indexOf(' ', start);
    if (end < 0) end = text.length();

    words[wordCount++] = text.substring(start, end);
    start = end + 1;
  }

  size_t lineCount = 0;
  String current = "";

  for (size_t i = 0; i < wordCount; ++i) {
    String candidate = current.length() == 0
        ? words[i]
        : current + " " + words[i];

    if (measureHebrewTextWidth(candidate) <= maxWidth) {
      current = candidate;
      continue;
    }

    if (current.length() > 0) {
      if (lineCount < maxLines) {
        lines[lineCount++] = current;
      }
      current = words[i];
    } else {
      current = words[i];
    }
  }

  if (current.length() > 0 && lineCount < maxLines) {
    lines[lineCount++] = current;
  }

  return lineCount;
}

int drawHebrewLineRtl(
    M5Canvas& canvas,
    const String& text,
    int rightX,
    int topY,
    uint16_t color) {
  uint16_t codepoints[128];
  const size_t count =
      decodeUtf8ToCodepoints(text, codepoints, 128);

  int x = rightX;

  for (size_t i = 0; i < count; ++i) {
    const uint16_t cp = codepoints[i];
    const TaraGlyph22* glyph = findTaraGlyph22(cp);

    if (glyph == nullptr) {
      continue;
    }

    x -= glyph->xAdvance;

    drawTaraGlyph22(
        canvas,
        x,
        topY,
        cp,
        color);
  }

  return x;
}
String lowerTrimmed(String s) {
  s.trim();
  s.toLowerCase();
  return s;
}

int triWave(uint16_t phase, int amplitude) {
  uint8_t p = phase & 0xFF;
  int v = p < 128 ? p : 255 - p;
  return ((v * 2 * amplitude) / 255) - amplitude;
}
}  // namespace

void EmotionController::setSubtitle(const String& text) {
  subtitle_ = text;
  lastFaceMs_ = 0;
}

void EmotionController::clearSubtitle() {
  subtitle_ = "";
  lastFaceMs_ = 0;
}

void EmotionController::begin() {
  setEmotion("neutral");
}

bool EmotionController::setEmotion(const String& emotion) {
  String normalized;
  Mode next = parseMode(emotion, normalized);
  Mode previous = mode_;
  mode_ = next;
  current_ = normalized;
  frame_ = 0;
  lastFrameMs_ = 0;
  lastFaceMs_ = 0;
  sleepStartedMs_ = (mode_ == Mode::Sleep) ? millis() : 0;
  if (previous == Mode::Sleep && mode_ != Mode::Sleep) {
    restoreSleepDisplay();
  }
  drawLabel();
  loop();  // Render first LED frame immediately.
  Serial.printf("Emotion: %s\n", current_.c_str());
  return true;
}

void EmotionController::loop() {
  uint32_t now = millis();
  if (mode_ == Mode::Sleep && sleepStartedMs_ == 0) sleepStartedMs_ = now;
  if (mode_ != Mode::Sleep && sleepDisplayOff_) restoreSleepDisplay();

  bool externalPower = externalPowerPresent();
  if (mode_ == Mode::Sleep && !sleepDisplayOff_ && sleepVisualOffDue()) {
    enterSleepDisplayOff(externalPower);
  }

  uint32_t faceInterval = (mode_ == Mode::Speaking) ? kSpeakingFaceIntervalMs : kFaceIntervalMs;
  if (!sleepDisplayOff_ && (lastFaceMs_ == 0 || now - lastFaceMs_ >= faceInterval)) {
    lastFaceMs_ = now;
    renderFace();
  }
  if (lastFrameMs_ != 0 && now - lastFrameMs_ < kFrameIntervalMs) return;
  lastFrameMs_ = now;
  ++frame_;

  switch (mode_) {
    case Mode::Neutral:   renderNeutral(); break;
    case Mode::Listening: renderListening(); break;
    case Mode::Speaking:  renderSpeaking(); break;
    case Mode::Thinking:  renderThinking(); break;
    case Mode::Looking:   renderLooking(); break;
    case Mode::Happy:     renderHappy(); break;
    case Mode::Angry:     renderAngry(); break;
    case Mode::Found:     renderFound(); break;
    case Mode::Error:     renderError(); break;
    case Mode::Sleep:
      if (sleepDisplayOff_ && !externalPower) setAll(0, 0, 0);
      else renderSleep();
      break;
  }
  refresh();
}

EmotionController::Mode EmotionController::parseMode(const String& emotion, String& normalized) {
  String e = lowerTrimmed(emotion);
  if (e == "listen" || e == "listening") { normalized = "listening"; return Mode::Listening; }
  if (e == "speak" || e == "speaking" || e == "talking") { normalized = "speaking"; return Mode::Speaking; }
  if (e == "think" || e == "thinking") { normalized = "thinking"; return Mode::Thinking; }
  if (e == "look" || e == "looking" || e == "seeing" || e == "camera") { normalized = "looking"; return Mode::Looking; }
  if (e == "happy" || e == "joy" || e == "smile") { normalized = "happy"; return Mode::Happy; }
  if (e == "angry" || e == "mad") { normalized = "angry"; return Mode::Angry; }
  if (e == "found" || e == "success") { normalized = "found"; return Mode::Found; }
  if (e == "error" || e == "confused") { normalized = "error"; return Mode::Error; }
  if (e == "sleep" || e == "sleeping") { normalized = "sleep"; return Mode::Sleep; }
  normalized = "neutral";
  return Mode::Neutral;
}

void EmotionController::hsvToRgb(uint8_t h, uint8_t s, uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b) {
  uint8_t region = h / 43;
  uint8_t remainder = (h - (region * 43)) * 6;
  uint8_t p = (v * (255 - s)) >> 8;
  uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
  uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;

  switch (region) {
    default:
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    case 5: r = v; g = p; b = q; break;
  }
}

uint8_t EmotionController::wave8(uint16_t x, uint8_t minValue, uint8_t maxValue) {
  uint8_t phase = x & 0xFF;
  uint8_t tri = phase < 128 ? phase * 2 : (255 - phase) * 2;
  return minValue + ((uint16_t)(maxValue - minValue) * tri) / 255;
}

uint8_t EmotionController::scale8(uint8_t value, uint8_t scale) {
  return ((uint16_t)value * scale) / 255;
}

bool EmotionController::externalPowerPresent() {
  int16_t vbus = M5.Power.getVBUSVoltage();
  if (vbus > 4200) return true;
  return M5.Power.isCharging() == m5::Power_Class::is_charging;
}

bool EmotionController::sleepVisualOffDue() const {
  return mode_ == Mode::Sleep && sleepStartedMs_ != 0 && millis() - sleepStartedMs_ >= kSleepDisplayOffMs;
}

void EmotionController::enterSleepDisplayOff(bool externalPower) {
  if (sleepDisplayOff_) return;
  auto& display = M5StackChan.Display();
  savedDisplayBrightness_ = display.getBrightness();
  display.fillScreen(TFT_BLACK);
  display.sleep();
  sleepDisplayOff_ = true;
  // On USB/external power keep a tiny heartbeat in the external LEDs. On battery,
  // go visually dark after the same timeout to avoid wasting power.
  if (!externalPower) setAll(0, 0, 0);
  refresh();
  Serial.printf("Emotion: sleep display off, external_power=%s\n", externalPower ? "yes" : "no");
}

void EmotionController::restoreSleepDisplay() {
  if (!sleepDisplayOff_) return;
  auto& display = M5StackChan.Display();
  display.wakeup();
  display.setBrightness(savedDisplayBrightness_ == 0 ? 127 : savedDisplayBrightness_);
  sleepDisplayOff_ = false;
  lastFaceMs_ = 0;
  Serial.println("Emotion: sleep display restored");
}

void EmotionController::setLed(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
  if (index >= kLedCount) return;
  M5StackChan.setRgbColor(index, r, g, b);
}

void EmotionController::setAll(uint8_t r, uint8_t g, uint8_t b) {
  for (uint8_t i = 0; i < kLedCount; ++i) setLed(i, r, g, b);
}

void EmotionController::refresh() {
  M5StackChan.refreshRgb();
}

void EmotionController::drawLabel() {
  renderFace();
}

void EmotionController::renderFace() {
  auto& display = M5StackChan.Display();
  static M5Canvas canvas(&display);
  static bool canvasReady = false;
  if (!canvasReady) {
    canvas.setColorDepth(8);
    canvasReady = canvas.createSprite(display.width(), display.height()) != nullptr;
    if (!canvasReady) {
      Serial.println("Emotion: face canvas allocation failed");
      return;
    }
  }

  auto& d = canvas;
  const int w = display.width();
  const int h = display.height();
  const int cx = w / 2;
  const int cy = h / 2;
  d.fillSprite(TFT_BLACK);

  if (mode_ == Mode::Speaking) {
    const int marginX = 14;
    const int bubbleTop = 42;
    const int bubbleBottom = h - 18;
    const int bubbleWidth = w - (marginX * 2);
    const int bubbleHeight = bubbleBottom - bubbleTop;
    const int radius = 14;
    const int border = 3;

    d.drawRoundRect(
        marginX,
        bubbleTop,
        bubbleWidth,
        bubbleHeight,
        radius,
        TFT_YELLOW);

    d.drawRoundRect(
        marginX + 1,
        bubbleTop + 1,
        bubbleWidth - 2,
        bubbleHeight - 2,
        radius - 1,
        TFT_YELLOW);

    d.drawLine(
        marginX + 28,
        bubbleBottom - 1,
        marginX + 16,
        h - 6,
        TFT_YELLOW);

    d.drawLine(
        marginX + 29,
        bubbleBottom - 1,
        marginX + 18,
        h - 6,
        TFT_YELLOW);

    d.drawLine(
        marginX + 16,
        h - 6,
        marginX + 44,
        bubbleBottom - 1,
        TFT_YELLOW);

    d.drawLine(
        marginX + 18,
        h - 6,
        marginX + 45,
        bubbleBottom - 1,
        TFT_YELLOW);

    String text = subtitle_;
    text.trim();

    if (text.length() > 0) {
      const int textRight = w - marginX - 14;
      const int textLeft = marginX + 14;
      const int textTop = bubbleTop + 14;
      const int textBottom = bubbleBottom - 12;
      const int textWidth = textRight - textLeft;
      const int lineHeight = 27;
      const size_t maxVisibleLines =
          static_cast<size_t>((textBottom - textTop) / lineHeight);

      String wrappedLines[32];
      const size_t lineCount = wrapHebrewText(
          text,
          textWidth,
          wrappedLines,
          32);

      size_t firstLine = 0;
      if (lineCount > maxVisibleLines) {
        firstLine = lineCount - maxVisibleLines;
      }

      int y = textTop;
      for (size_t i = firstLine; i < lineCount; ++i) {
        drawHebrewLineRtl(
            d,
            wrappedLines[i],
            textRight,
            y,
            TFT_WHITE);

        y += lineHeight;
      }
    }

    canvas.pushSprite(&display, 0, 0);
    return;
  }
  uint16_t eyeColor = TFT_CYAN;
  uint16_t accent = TFT_DARKCYAN;
  bool closed = false;
  int pupilDx = triWave(frame_ * 3, 4);
  int pupilDy = 0;
  int mouthW = 76;
  int mouthH = 8;
  int mouthY = cy + 44;
  bool smile = false;
  bool frown = false;
  bool talking = false;
  uint8_t talkShape = 0;

  switch (mode_) {
    case Mode::Listening:
      eyeColor = TFT_SKYBLUE;
      accent = TFT_BLUE;
      pupilDx = triWave(frame_ * 6, 10);
      mouthW = 44;
      mouthH = 5;
      break;
    case Mode::Speaking:
      eyeColor = TFT_GREENYELLOW;
      accent = TFT_GREEN;
      talking = true;
      // Advance viseme shapes faster than the LCD redraw cadence. This makes
      // the mouth feel closer to speech speed without increasing display load
      // (28ms redraw cadence previously caused audible voice stutter).
      talkShape = (frame_ * 2) % 8;
      mouthW = 56 + ((talkShape % 3) * 14);
      mouthH = 10 + (uint8_t)wave8(frame_ * 46, 4, 34);
      pupilDx = triWave(frame_ * 8, 7);
      pupilDy = triWave(frame_ * 5, 3);
      break;
    case Mode::Thinking:
      eyeColor = TFT_YELLOW;
      accent = TFT_ORANGE;
      pupilDx = triWave(frame_ * 4, 14);
      pupilDy = triWave(frame_ * 2, 4);
      mouthW = 34;
      mouthH = 5;
      mouthY = cy + 48;
      break;
    case Mode::Looking:
      // Vision/camera state: big attentive eyes, no head motion.
      eyeColor = TFT_CYAN;
      accent = TFT_BLUE;
      pupilDx = triWave(frame_ * 5, 16);
      pupilDy = triWave(frame_ * 3, 5);
      mouthW = 30;
      mouthH = 4;
      mouthY = cy + 50;
      break;
    case Mode::Happy:
    case Mode::Found:
      eyeColor = TFT_YELLOW;
      accent = TFT_ORANGE;
      smile = true;
      mouthW = 92;
      mouthH = 8;
      break;
    case Mode::Angry:
      eyeColor = TFT_RED;
      accent = TFT_ORANGE;
      frown = true;
      mouthW = 72;
      mouthH = 6;
      break;
    case Mode::Error:
      eyeColor = TFT_RED;
      accent = TFT_RED;
      frown = true;
      mouthW = 56;
      mouthH = 6;
      break;
    case Mode::Sleep:
      eyeColor = TFT_DARKGREY;
      accent = TFT_NAVY;
      closed = true;
      mouthW = 40;
      mouthH = 4;
      break;
    case Mode::Neutral:
    default:
      // Occasional soft blink in idle.
      closed = ((frame_ / 55) % 18) == 0;
      break;
  }

  // Subtle cheek/accent dots.
  d.fillCircle(cx - 92, cy + 22, 5, accent);
  d.fillCircle(cx + 92, cy + 22, 5, accent);

  const int lx = cx - 58;
  const int rx = cx + 58;
  const int ey = cy - 26;
  if (closed) {
    d.fillRect(lx - 24, ey - 2, 48, 5, eyeColor);
    d.fillRect(rx - 24, ey - 2, 48, 5, eyeColor);
  } else {
    // Camera/looking mode should be visibly different: wider "aperture" eyes
    // make it obvious that StackChan is looking, while servo/head stays still.
    const int eyeR = (mode_ == Mode::Looking) ? 34 : 25;
    const int pupilR = (mode_ == Mode::Looking) ? 12 : 10;
    d.fillCircle(lx, ey, eyeR, eyeColor);
    d.fillCircle(rx, ey, eyeR, eyeColor);
    d.fillCircle(lx + pupilDx, ey + pupilDy, pupilR, TFT_BLACK);
    d.fillCircle(rx + pupilDx, ey + pupilDy, pupilR, TFT_BLACK);
  }

  if (mode_ == Mode::Angry || mode_ == Mode::Error) {
    d.drawLine(lx - 28, ey - 36, lx + 18, ey - 24, accent);
    d.drawLine(rx - 18, ey - 24, rx + 28, ey - 36, accent);
  } else if (mode_ == Mode::Thinking || mode_ == Mode::Looking) {
    d.drawLine(lx - 22, ey - 34, lx + 18, ey - 38, accent);
    d.drawLine(rx - 18, ey - 38, rx + 22, ey - 34, accent);
  }

  if (talking) {
    // Viseme-ish talking mouth: vary width/height and leave a dark inner mouth,
    // so speech is visually obvious rather than a single static open blob.
    const int openH[] = {8, 28, 14, 36, 10, 24, 18, 32};
    const int openW[] = {46, 70, 88, 58, 42, 82, 64, 74};
    int ow = openW[talkShape];
    int oh = openH[talkShape];
    int y = mouthY + triWave(frame_ * 22, 3);
    d.fillRoundRect(cx - ow / 2 - 6, y - oh / 2 - 5, ow + 12, oh + 10, 12, accent);
    d.fillRoundRect(cx - ow / 2, y - oh / 2, ow, oh, 9, TFT_BLACK);
    if (oh > 22) {
      d.fillRect(cx - ow / 2 + 10, y - oh / 2 + 3, ow - 20, 4, TFT_WHITE);
    }
    if ((talkShape % 4) == 1) {
      d.fillCircle(cx, y + oh / 2 - 5, min(14, oh / 3), TFT_RED);
    }
  } else if (smile) {
    d.fillRect(cx - mouthW / 2, mouthY, mouthW, mouthH, eyeColor);
    d.fillCircle(cx - mouthW / 2, mouthY, mouthH, TFT_BLACK);
    d.fillCircle(cx + mouthW / 2, mouthY, mouthH, TFT_BLACK);
    d.fillCircle(cx, mouthY - 10, 34, TFT_BLACK);
  } else if (frown) {
    d.fillRect(cx - mouthW / 2, mouthY, mouthW, mouthH, eyeColor);
    d.fillCircle(cx, mouthY + 15, 36, TFT_BLACK);
  } else {
    d.fillRect(cx - mouthW / 2, mouthY - mouthH / 2, mouthW, mouthH, eyeColor);
    d.fillCircle(cx - mouthW / 2, mouthY, mouthH / 2, eyeColor);
    d.fillCircle(cx + mouthW / 2, mouthY, mouthH / 2, eyeColor);
  }

  if (mode_ == Mode::Sleep) {
    d.setTextSize(2);
    d.setTextColor(TFT_DARKCYAN);
    d.setCursor(cx + 78, cy - 64);
    d.print("Zz");
  }

  canvas.pushSprite(&display, 0, 0);
}

void EmotionController::renderNeutral() {
  uint8_t v = wave8(frame_ * 2, 4, 18);
  setAll(0, 0, v);
}

void EmotionController::renderListening() {
  uint8_t v = wave8(frame_ * 3, 10, 55);
  setAll(0, scale8(80, v), v);
}

void EmotionController::renderSpeaking() {
  uint8_t v = wave8(frame_ * 10, 25, 95);
  for (uint8_t i = 0; i < kLedCount; ++i) {
    uint8_t local = (i % 2 == 0) ? v : scale8(v, 120);
    setLed(i, scale8(60, local), local, scale8(120, local));
  }
}

void EmotionController::renderThinking() {
  uint8_t head = (frame_ / 2) % kLedCount;
  for (uint8_t i = 0; i < kLedCount; ++i) {
    uint8_t dist = (i + kLedCount - head) % kLedCount;
    uint8_t v = 2;
    if (dist == 0) v = 95;
    else if (dist == 1 || dist == kLedCount - 1) v = 42;
    else if (dist == 2 || dist == kLedCount - 2) v = 14;
    setLed(i, 0, scale8(120, v), v);
  }
}

void EmotionController::renderLooking() {
  // Camera assist light: bright neutral-white LEDs to signal capture/analyze and
  // add a little illumination. No servo/head motion is triggered from this state.
  uint8_t pulse = wave8(frame_ * 4, 120, 210);
  uint8_t sweep = (frame_ / 3) % kLedCount;
  for (uint8_t i = 0; i < kLedCount; ++i) {
    uint8_t v = pulse;
    if (i == sweep || i == (sweep + kLedCount / 2) % kLedCount) v = 245;
    setLed(i, v, v, v);
  }
}

void EmotionController::renderHappy() {
  uint8_t baseHue = frame_ * 3;
  for (uint8_t i = 0; i < kLedCount; ++i) {
    uint8_t r, g, b;
    hsvToRgb(baseHue + i * 21, 230, 85, r, g, b);
    setLed(i, r, g, b);
  }
}

void EmotionController::renderAngry() {
  uint8_t v = wave8(frame_ * 9, 25, 110);
  for (uint8_t i = 0; i < kLedCount; ++i) {
    uint8_t local = (i == ((frame_ / 3) % kLedCount)) ? 130 : v;
    setLed(i, local, 0, 0);
  }
}

void EmotionController::renderFound() {
  uint8_t v = wave8(frame_ * 8, 25, 120);
  for (uint8_t i = 0; i < kLedCount; ++i) {
    if ((i + frame_ / 4) % 3 == 0) setLed(i, v, scale8(220, v), 0);
    else setLed(i, scale8(30, v), scale8(70, v), 0);
  }
}

void EmotionController::renderError() {
  bool on = ((frame_ / 8) % 2) == 0;
  setAll(on ? 110 : 0, on ? 12 : 0, 0);
}

void EmotionController::renderSleep() {
  uint8_t v = wave8(frame_, 0, 14);
  setAll(0, 0, v);
}
