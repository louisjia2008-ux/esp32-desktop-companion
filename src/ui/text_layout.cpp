#include "text_layout.h"

#include <cstring>

namespace text {

namespace {

constexpr uint32_t kReplacement = 0xFFFD;

void appendBytes(char* destination, size_t capacity, size_t& used,
                 const char* bytes, size_t count) {
  if (used + count + 1 > capacity) return;
  memcpy(destination + used, bytes, count);
  used += count;
  destination[used] = '\0';
}

const char* asciiSubstitute(uint32_t codePoint) {
  switch (codePoint) {
    case 0x2026: return "...";
    case 0x00B7: case 0x2022: case 0x30FB: return "-";
    case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
    case 0x2018: case 0x2019: case 0x201A: return "'";
    case 0x201C: case 0x201D: case 0x201E: return "\"";
    case 0x00A0: case 0x3000: return " ";
    case 0x00D7: return "x";
    case 0x2192: return "->";
    case 0x2190: return "<-";
    default: return nullptr;
  }
}

}  // namespace

size_t decode(const char* text, uint32_t& codePoint) {
  const uint8_t lead = static_cast<uint8_t>(text[0]);
  if (lead < 0x80) {
    codePoint = lead;
    return 1;
  }
  size_t length = 0;
  uint32_t value = 0;
  uint32_t minimum = 0;
  if ((lead & 0xE0) == 0xC0) { length = 2; value = lead & 0x1F; minimum = 0x80; }
  else if ((lead & 0xF0) == 0xE0) { length = 3; value = lead & 0x0F; minimum = 0x800; }
  else if ((lead & 0xF8) == 0xF0) { length = 4; value = lead & 0x07; minimum = 0x10000; }
  else { codePoint = kReplacement; return 1; }
  for (size_t i = 1; i < length; ++i) {
    const uint8_t next = static_cast<uint8_t>(text[i]);
    if ((next & 0xC0) != 0x80) { codePoint = kReplacement; return 1; }
    value = (value << 6) | (next & 0x3F);
  }
  if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
    codePoint = kReplacement;
    return 1;
  }
  codePoint = value;
  return length;
}

int16_t advance(uint32_t codePoint) {
  // Unifont draws everything before the Hangul Jamo block halfwidth; CJK,
  // Hangul and fullwidth forms are 16 px. Missing glyphs are skipped by the
  // renderer, so over-estimating keeps layout conservative.
  if (codePoint < 0x1100) return 8;
  if (codePoint >= 0xFF61 && codePoint <= 0xFFDC) return 8;  // halfwidth forms
  return 16;
}

int16_t width(const char* text, uint8_t scale) {
  if (text == nullptr) return 0;
  int32_t total = 0;
  for (size_t i = 0; text[i] != '\0';) {
    uint32_t codePoint = 0;
    i += decode(text + i, codePoint);
    total += advance(codePoint);
  }
  total *= scale;
  return total > 0x7FFF ? 0x7FFF : static_cast<int16_t>(total);
}

void normalize(const char* source, char* destination, size_t capacity) {
  if (capacity == 0) return;
  destination[0] = '\0';
  if (source == nullptr) return;
  size_t used = 0;
  for (size_t i = 0; source[i] != '\0';) {
    uint32_t codePoint = 0;
    const size_t length = decode(source + i, codePoint);
    const char* substitute = codePoint < 0x20 || codePoint == 0x7F ? " " :
        codePoint == kReplacement ? "?" : asciiSubstitute(codePoint);
    const size_t before = used;
    if (substitute != nullptr) {
      appendBytes(destination, capacity, used, substitute, strlen(substitute));
    } else {
      appendBytes(destination, capacity, used, source + i, length);
    }
    if (used == before) break;  // out of room: stop at a whole character
    i += length;
  }
}

size_t fitLine(const char* source, int16_t maximumWidth, uint8_t scale,
               size_t& next) {
  next = 0;
  if (source == nullptr || source[0] == '\0') return 0;
  int32_t used = 0;
  size_t end = 0;
  size_t lastSpace = 0;  // byte offset of the last space inside the line
  bool sawSpace = false;
  while (source[end] != '\0' && source[end] != '\n') {
    uint32_t codePoint = 0;
    const size_t length = decode(source + end, codePoint);
    const int32_t glyph = advance(codePoint) * scale;
    if (used + glyph > maximumWidth) break;
    if (codePoint == ' ') { lastSpace = end; sawSpace = true; }
    used += glyph;
    end += length;
  }
  size_t take = end;
  const bool more = source[end] != '\0' && source[end] != '\n';
  if (more && end > 0) {
    uint32_t following = 0;
    decode(source + end, following);
    uint32_t last = 0;
    size_t start = end - 1;
    while (start > 0 && (static_cast<uint8_t>(source[start]) & 0xC0) == 0x80) --start;
    decode(source + start, last);
    const bool splitsWord = following < 0x80 && following != ' ' && last < 0x80 && last != ' ';
    if (splitsWord && sawSpace && lastSpace > 0) take = lastSpace;
  }
  if (take == 0 && source[0] != '\0' && source[0] != '\n') {
    // A single glyph wider than the line: emit it anyway to make progress.
    uint32_t codePoint = 0;
    take = decode(source, codePoint);
  }
  next = take;
  while (source[next] == ' ' || source[next] == '\n') ++next;
  size_t trimmed = take;
  while (trimmed > 0 && source[trimmed - 1] == ' ') --trimmed;
  return trimmed;
}

void ellipsize(const char* source, int16_t maximumWidth, uint8_t scale,
               char* destination, size_t capacity) {
  if (capacity == 0) return;
  destination[0] = '\0';
  if (source == nullptr || capacity < 4) return;
  if (width(source, scale) <= maximumWidth && strlen(source) < capacity) {
    memcpy(destination, source, strlen(source) + 1);
    return;
  }
  const int16_t dots = width("...", scale);
  size_t used = 0;
  int32_t total = 0;
  for (size_t i = 0; source[i] != '\0';) {
    uint32_t codePoint = 0;
    const size_t length = decode(source + i, codePoint);
    const int32_t glyph = advance(codePoint) * scale;
    if (total + glyph + dots > maximumWidth || used + length + 4 > capacity) break;
    memcpy(destination + used, source + i, length);
    used += length;
    total += glyph;
    i += length;
  }
  while (used > 0 && destination[used - 1] == ' ') --used;
  memcpy(destination + used, "...", 4);
}

}  // namespace text
