#include "ui/glass_clock.h"
#include "board_config.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <vector>
int main(int argc, char** argv) {
  constexpr size_t count = 320 * 480;
  std::vector<uint16_t> buffer(count + 32, 0xA55A);
  uint16_t* pixels = buffer.data() + 16;
  Arduino_Canvas canvas(pixels);
  uifont::GlassClock clock;
  struct tm local{};
  local.tm_mon = 9; local.tm_mday = 1; local.tm_wday = 4;
  // Exercise every hour and all minute digits with the real generated data.
  for (int hour = 0; hour < 24; ++hour) {
    for (int minute : {0, 11, 22, 33, 44, 55, 59}) {
      std::fill(pixels, pixels + count, 0x4C56);
      local.tm_hour = hour; local.tm_min = minute;
      clock.render(canvas, local);
      assert(std::all_of(buffer.begin(), buffer.begin()+16, [](uint16_t p){return p == 0xA55A;}));
      assert(std::all_of(buffer.end()-16, buffer.end(), [](uint16_t p){return p == 0xA55A;}));
      assert(std::count(pixels, pixels+count, 0x4C56) < count-1000);
      const std::vector<uint16_t> first(pixels, pixels + count);
      std::fill(pixels, pixels + count, 0x4C56);
      clock.render(canvas, local);  // cached geometry must produce the same image
      assert(std::equal(first.begin(), first.end(), pixels));
      std::fill(pixels, pixels + count, 0x1024);
      clock.render(canvas, local);  // only geometry is cached, never the wallpaper
      assert(!std::equal(first.begin(), first.end(), pixels));
    }
  }
  if (argc == 3) {
    std::ifstream raw(argv[1], std::ios::binary);
    raw.read(reinterpret_cast<char*>(pixels), count * 2);
    assert(raw.gcount() == count * 2);
    local.tm_hour = 20; local.tm_min = 15;
    clock.render(canvas, local);
    std::ofstream out(argv[2], std::ios::binary);
    out << "P6\n480 320\n255\n";
    for (int y=0; y<320; ++y) for(int x=0; x<480; ++x) {
      uint16_t p=pixels[x*320+319-y];
      unsigned char rgb[]={static_cast<unsigned char>(((p>>11)&31)*255/31),
        static_cast<unsigned char>(((p>>5)&63)*255/63), static_cast<unsigned char>((p&31)*255/31)};
      out.write(reinterpret_cast<char*>(rgb),3);
    }
  }
  puts("PASS actual glyph renderer: 168 times, cached layouts, fresh backgrounds, guards, ASan/UBSan");
}
