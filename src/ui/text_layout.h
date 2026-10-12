#pragma once

#include <cstddef>
#include <cstdint>

// UTF-8 measurement and line fitting for the 16 px Unifont CJK font.
// Pure functions: no graphics state, so the host tests exercise them as-is.
namespace text {

// Unifont glyph cell: 16 px tall; halfwidth 8 px, fullwidth 16 px.
constexpr int16_t kGlyphHeight = 16;
constexpr int16_t kAscent = 14;

// Decodes one code point. Malformed or truncated sequences consume one byte
// and yield U+FFFD, so callers always advance and never split a character.
size_t decode(const char* text, uint32_t& codePoint);

int16_t advance(uint32_t codePoint);
int16_t width(const char* text, uint8_t scale);

// Copies `source` replacing punctuation the CJK font lacks (ellipsis,
// curly quotes, dashes, middle dot) with ASCII and control characters with
// spaces. Output is always terminated and never ends mid-character.
void normalize(const char* source, char* destination, size_t capacity);

// Bytes of `source` that fit in maximumWidth for one line. Prefers to break
// after a space when the cut would split an ASCII word. `next` receives the
// offset where the following line starts (leading spaces skipped).
size_t fitLine(const char* source, int16_t maximumWidth, uint8_t scale,
               size_t& next);

// Copies as much of `source` as fits in maximumWidth, ending with "..."
// when it had to be shortened.
void ellipsize(const char* source, int16_t maximumWidth, uint8_t scale,
               char* destination, size_t capacity);

}  // namespace text
