#pragma once
#include <cstdint>
#include <string>
#include <vector>
constexpr uint16_t WHITE = 0xFFFF;
struct GFXfont;
class Arduino_Canvas {
 public:
  explicit Arduino_Canvas(uint16_t* framebuffer = nullptr) : framebuffer_(framebuffer) {}
  template <typename... T> void setFont(T...) {}
  template <typename... T> void setUTF8Print(T...) {}
  template <typename... T> void setTextWrap(T...) {}
  void drawLine(int x1, int y1, int x2, int y2, uint16_t color) {
    lines.push_back({x1, y1, x2, y2, color});
  }
  uint16_t* getFramebuffer() { return framebuffer_; }
  template <typename... T> void drawCircle(T...) {}
  template <typename... T> void drawRoundRect(T...) {}
  void fillCircle(int x, int y, int radius, uint16_t color) {
    circles.push_back({x, y, radius, color});
  }
  template <typename... T> void fillRect(T...) {}
  template <typename... T> void print(T...) {}
  void print(const char* value) { printed.emplace_back(value);textPositions.push_back({cursorX_,cursorY_,value}); }
  void print(char* value) { print(static_cast<const char*>(value)); }
  void setCursor(int x,int y) { cursorX_=x;cursorY_=y; }
  template <typename... T> void setTextColor(T...) {}
  template <typename... T> void setTextSize(T...) {}
  struct TextPosition { int x,y;std::string text; };
  struct Line { int x1,y1,x2,y2;uint16_t color; };
  struct Circle { int x,y,radius;uint16_t color; };
  std::vector<std::string> printed;
  std::vector<TextPosition> textPositions;
  std::vector<Line> lines;
  std::vector<Circle> circles;
 private:
  uint16_t* framebuffer_;
  int cursorX_=0,cursorY_=0;
};
