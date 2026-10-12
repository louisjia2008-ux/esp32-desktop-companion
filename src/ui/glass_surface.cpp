// Keep optimization local to the pixel kernel; the rest of the firmware
// retains its existing size and driver settings. Integer operations and
// rendering order are identical to the prior header implementation.
#include "glass_surface.h"

#if defined(ARDUINO_ARCH_ESP32)
#pragma GCC push_options
#pragma GCC optimize("O3")
#endif

namespace surface {
uint16_t liquid(Arduino_Canvas& gfx,int x,int y,int width,int height) {
  auto* frame=gfx.getFramebuffer();
  if(!frame||board::kDisplayRotation!=1||width>440||height>48){glass(gfx,x,y,width,height,0x0863,176,12);return WHITE;}
  constexpr int stride=112;
  static uint16_t samples[stride*14];
  const int columns=(width+3)/4+2,rows=(height+3)/4+2;
  unsigned lightSum=0;
  for(int column=0;column<columns;++column)for(int row=0;row<rows;++row){
    const int px=std::max(0,std::min<int>(board::kDisplayWidth-1,x+column*4-2));
    const int py=std::max(0,std::min<int>(board::kDisplayHeight-1,y+row*4-2));
    const uint16_t c=frame[px*board::kPanelWidth+board::kPanelWidth-1-py];samples[column*14+row]=c;
    lightSum+=77*(c>>11)*8+150*((c>>5)&63)*4+29*(c&31)*8;
  }
  const bool bright=lightSum/unsigned(columns*rows*256)>145;
  const uint16_t tint=bright?0xFFFF:0x0863;
  const unsigned alpha=bright?30:50;
  // Compose the blur/tint layers once per sample cell. The 4x4 cell's
  // softened colour is shared; only the final backdrop blend is per pixel.
  // This keeps the current wallpaper visible without repeating five-sample
  // filtering and two full RGB blends for every display pixel.
  const unsigned combined=255-((255-92)*(255-alpha)+127)/255;
  const unsigned tintShare=(alpha*255+combined/2)/combined;
  const unsigned alpha32=(combined*32+127)/255,inverse32=32-alpha32;
  uint32_t shades[14];
  int preparedColumn=-1;
  constexpr int radius=14;
  const int right=std::min<int>(board::kDisplayWidth,x+width);
  const int top=std::max(0,y),bottom=std::min<int>(board::kDisplayHeight,y+height);
  const int middle=y+height/2;
  for(int px=std::max(0,x);px<right;++px){
   const int column=(px-x)/4+1;
   if(column!=preparedColumn){
    for(int row=1;row<rows-1;++row){
     const uint16_t neighbors[]={samples[column*14+row],samples[(column-1)*14+row],samples[(column+1)*14+row],samples[column*14+row-1],samples[column*14+row+1]};
     unsigned r=0,g=0,b=0;for(uint16_t c:neighbors){r+=c>>11;g+=(c>>5)&63;b+=c&31;}
     const uint16_t softened=uint16_t(((r/5)<<11)|((g/5)<<5)|(b/5));
     shades[row]=pack565(mix(softened,tint,tintShare))*alpha32;
    }
    preparedColumn=column;
   }
   const int cx=px<x+radius?x+radius-1-px:px>=x+width-radius?px-(x+width-radius):0;
   const int cxSquared=cx*cx;
   const bool edgeColumn=px==x||px==x+width-1;
   uint16_t* const frameColumn=frame+px*board::kPanelWidth;
   for(int py=top;py<bottom;++py){
    const int cy=py<y+radius?y+radius-1-py:py>=y+height-radius?py-(y+height-radius):0;
    const int distance=cxSquared+cy*cy;
    if(cx&&cy&&distance>=radius*radius)continue;
    const int row=(py-y)/4+1;
    uint16_t& pixel=frameColumn[board::kPanelWidth-1-py];
    pixel=mixPacked565(pixel,shades[row],inverse32);
    // Keep the sparse rim exact so two approximations cannot accumulate.
    if(distance>=(radius-1)*(radius-1)||edgeColumn)pixel=mix(pixel,0xE7FF,py<middle?70:24);
   }
  }
  return bright?0x1128:WHITE;
}
}  // namespace surface
#if defined(ARDUINO_ARCH_ESP32)
#pragma GCC pop_options
#endif
