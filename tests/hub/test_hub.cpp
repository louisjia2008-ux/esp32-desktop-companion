#include "hub/hub_model.h"
#include "hub/hub_ui.h"
#include "hub/hub_protocol.h"
#include "protocol/protocol.h"
#include "ui/ui_manager.h"
#include "ui/glass_surface.h"
#include "ui/text_layout.h"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <string>
namespace uifont { const uint8_t* cjkFont(){return nullptr;} }
void Wallpaper::begin(uint32_t){} void Wallpaper::render(Arduino_Canvas&,uint32_t){}
bool Wallpaper::selectVideo(const char*){return true;} bool Wallpaper::decodeVideoFrame(const char*,const uint8_t*,size_t,uint32_t){return true;}bool Wallpaper::videoFresh(uint32_t)const{return false;}
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+" "+#x);}while(false)
static protocol::IncomingMessage packet;
hub::Page makePage(uint32_t revision,uint8_t page,uint8_t total){hub::Page p;p.section=hub::Section::Cards;p.revision=revision;p.page=page;p.pages=total?total:1;p.total=total;p.hasItem=total>0;p.observedAt=1800000000;strcpy(p.card.kind,"text");snprintf(p.card.id,sizeof(p.card.id),"card%u",unsigned(page));strcpy(p.card.title,"卡片");strcpy(p.card.body,"text");p.card.expiresAt=1800001800;return p;}
void parse(const char* s,bool expected=true){const char* reason=nullptr;CHECK(protocol::parseIncoming(reinterpret_cast<const uint8_t*>(s),strlen(s),packet,reason)==expected);}
void sample(UiManager& ui,bool down,int x,int y,uint32_t at){fake_arduino::setMillis(at);UiActionEvent e;ui.handleTouch(down,TouchPoint{uint16_t(x),uint16_t(y)},at,e);}
void lift(UiManager& ui,uint32_t at){sample(ui,false,0,0,at);sample(ui,false,0,0,at+50);sample(ui,false,0,0,at+120);}
uint32_t swipe(UiManager& ui,uint32_t at,int from=430,int to=100){sample(ui,true,from,310,at);sample(ui,true,to,310,at+100);lift(ui,at+200);return at+400;}
void paging(){
 hub::Model model;CHECK(!model.accept(makePage(1,0,2),1));CHECK(model.count(hub::Section::Cards)==0);CHECK(model.accept(makePage(1,1,2),2));CHECK(model.count(hub::Section::Cards)==2);
 auto p=makePage(2,0,3);CHECK(!model.accept(p,3));CHECK(model.count(hub::Section::Cards)==2);CHECK(!model.accept(makePage(1,0,2),4));
 model.disconnected();CHECK(!model.accept(makePage(2,1,3),5));CHECK(!model.accept(makePage(2,2,3),6));CHECK(model.revision(hub::Section::Cards)==1);CHECK(model.accept(makePage(2,0,3),7));
 CHECK(!model.cardAvailable(4,1));CHECK(!model.cardAvailable(0,1801000));
 CHECK(model.accept(makePage(3,0,0),1801001));CHECK(model.count(hub::Section::Cards)==0);
 auto bad=makePage(4,0,6);CHECK(!model.accept(bad,10));
 hub::Model full;
 for(uint8_t i=0;i<64;++i){hub::Page c;c.section=hub::Section::Control;c.revision=1;c.page=i;c.pages=64;c.total=64;c.hasItem=true;strcpy(c.control.kind,"wifi");CHECK(full.accept(c,20+i)==(i==63));}
 CHECK(full.count(hub::Section::Control)==64);
 std::cout<<"PASS atomic paging, missing pages, stale revisions, offline expiry and bounded counts\n";
}
void protocolChecks(){
 parse(R"({"v":1,"type":"hub_snapshot","section":"control","revision":1,"page":0,"pages":1,"total":1,"observed_at":1800000000,"items":[{"kind":"status","state":"not_provided","reason":"耳机待机；调节后确认状态","volume":null,"muted":null,"volume_supported":true,"mute_supported":true}]})");CHECK(!packet.hubPage.control.hasVolume);CHECK(packet.hubPage.control.volumeSupported);
 parse(R"({"v":1,"type":"hub_snapshot","section":"control","revision":2,"page":0,"pages":1,"total":1,"observed_at":1800000000,"items":[{"kind":"status","state":"cached","reason":"耳机待机；显示上次确认状态","volume":40,"muted":false,"volume_supported":true,"mute_supported":true}]})");CHECK(packet.hubPage.control.hasVolume);CHECK(packet.hubPage.control.volume==40);CHECK(!strcmp(packet.hubPage.control.state,"cached"));
 parse(R"({"v":1,"type":"hub_snapshot","section":"usage","revision":1,"page":0,"pages":1,"total":1,"observed_at":1800000000,"items":[{"provider":"codex","limit_id":"codex","window_minutes":10080,"used_percent":41,"resets_at":1800100000,"observed_at":1800000000,"state":"fresh"}]})");CHECK(packet.hubPage.usage.windowMinutes==10080);CHECK(packet.hubPage.usage.usedPercent==41);
 parse(R"({"v":1,"type":"hub_snapshot","section":"usage","revision":1,"page":0,"pages":1,"total":1,"observed_at":1800000000,"items":[{"provider":"codex","limit_id":"codex","window_minutes":300,"used_percent":null,"resets_at":null,"observed_at":1800000000,"state":"not_provided"}]})");CHECK(!packet.hubPage.usage.hasUsed);
 parse(R"({"v":1,"type":"hub_snapshot","section":"cards","revision":1,"page":0,"pages":1,"total":1,"observed_at":1800000000,"items":[{"id":"card","kind":"link","title":"test","url":"file:///secret","created_at":1800000000,"expires_at":1800001000,"pinned":false}]})",false);
 parse(R"({"v":1,"type":"hub_result","operation_id":"op","state":"succeeded","reason":"ok","command":"ls"})",false);
 parse(R"({"v":1,"type":"hub_result","operation_id":"op","state":"unconfirmed","reason":"断线"})");CHECK(packet.type==protocol::IncomingType::HUB_RESULT);
 const std::string huge(2049,' ');parse(huge.c_str(),false);
 std::cout<<"PASS hub JSON bounds, null quota fields, fixed result schema and link schemes\n";
}
hub::Page controlBatch(uint32_t revision,uint8_t page,uint8_t total){
 hub::Page p;p.section=hub::Section::Control;p.revision=revision;p.page=page;p.pages=total?(total+3)/4:1;p.total=total;p.hasItem=total>0;
 p.controlCount=total?std::min<int>(4,total-page*4):0;
 for(uint8_t i=0;i<p.controlCount;++i){strcpy(p.controls[i].kind,"audio");snprintf(p.controls[i].id,sizeof(p.controls[i].id),"audio%u",unsigned(page*4+i));}
 return p;
}
void controlBatchPaging(){
 hub::Model model;auto initial=controlBatch(1,0,1);CHECK(model.accept(initial,0));
 // All 64 legacy rows still arrive at six FPS, without a premature reset.
 for(uint8_t i=0;i<64;++i){hub::Page p;p.section=hub::Section::Control;p.revision=2;p.page=i;p.pages=64;p.total=64;p.hasItem=true;
  strcpy(p.control.kind,"audio");snprintf(p.control.id,sizeof(p.control.id),"legacy%u",unsigned(i));CHECK(model.accept(p,100+i*170)==(i==63));}
 CHECK(model.count(hub::Section::Control)==64);CHECK(!strcmp(model.controls()[63].id,"legacy63"));
 // A missing page, a repeated page, or a conflicting layout never publishes
 // half a control list or replaces a previously received page's contents.
 for(uint8_t i=0;i<16;++i)if(i!=3)CHECK(!model.accept(controlBatch(3,i,64),12000+i*170));
 CHECK(model.revision(hub::Section::Control)==2);auto duplicate=controlBatch(3,0,64);strcpy(duplicate.controls[0].id,"duplicate");CHECK(!model.accept(duplicate,14800));
 auto wrong=controlBatch(3,3,64);wrong.pages=64;wrong.controlCount=1;CHECK(!model.accept(wrong,14900));
 CHECK(model.accept(controlBatch(3,3,64),15000));for(int i=0;i<64;++i){char id[33];snprintf(id,sizeof(id),"audio%d",i);CHECK(!strcmp(model.controls()[i].id,id));}
 CHECK(!model.accept(controlBatch(2,0,64),15100));CHECK(!model.accept(controlBatch(3,0,64),15101));
 auto missing=controlBatch(4,0,9);missing.controlCount=3;CHECK(!model.accept(missing,16000));
 CHECK(!model.accept(controlBatch(4,0,9),16100));CHECK(!model.accept(controlBatch(4,2,9),16200));CHECK(model.count(hub::Section::Control)==64);
 CHECK(model.accept(controlBatch(4,1,9),16300));CHECK(model.count(hub::Section::Control)==9);CHECK(!strcmp(model.controls()[8].id,"audio8"));
 hub::Model expiry;CHECK(!expiry.accept(controlBatch(1,0,8),0));CHECK(!expiry.accept(controlBatch(1,1,8),30001));CHECK(expiry.count(hub::Section::Control)==0);CHECK(expiry.accept(controlBatch(1,0,8),30002));
 auto tooMany=controlBatch(5,0,64);tooMany.total=65;tooMany.pages=17;CHECK(!model.accept(tooMany,17000));
 std::cout<<"PASS 64 legacy control rows at six FPS, atomic 4-row pages, gaps, duplicates and bounded expiry\n";
}
std::string controlWire(unsigned page,unsigned pages,unsigned total,const std::vector<std::string>& items,const char* section="control"){
 std::string out="{\"v\":1,\"type\":\"hub_snapshot\",\"section\":\""+std::string(section)+"\",\"revision\":10,\"page\":"+std::to_string(page)+",\"pages\":"+std::to_string(pages)+",\"total\":"+std::to_string(total)+",\"observed_at\":1800000000,\"items\":[";
 for(size_t i=0;i<items.size();++i){if(i)out+=",";out+=items[i];}return out+"]}";
}
void controlBatchProtocol(){
 const std::string row=R"({"kind":"audio","id":"a","name":"speaker","selected":true})";
 parse(controlWire(0,3,9,{row,row,row,row}).c_str());CHECK(packet.hubPage.controlCount==4);CHECK(!strcmp(packet.hubPage.controls[3].id,"a"));
 parse(controlWire(2,3,9,{row}).c_str());CHECK(packet.hubPage.controlCount==1);
 parse(controlWire(0,1,3,{row,row,row}).c_str());CHECK(packet.hubPage.controlCount==3);
 parse(controlWire(0,3,3,{row}).c_str());CHECK(packet.hubPage.controlCount==1);CHECK(!strcmp(packet.hubPage.control.id,"a"));
 parse(controlWire(0,1,0,{}).c_str());CHECK(!packet.hubPage.hasItem);
 parse(controlWire(0,3,9,{row,row,row}).c_str(),false);
 parse(controlWire(2,3,9,{row,row}).c_str(),false);
 parse(controlWire(0,1,5,{row,row,row,row,row}).c_str(),false);
 parse(controlWire(0,1,2,{row,row},"cards").c_str(),false);
 parse(controlWire(0,1,4,{row,row,row,R"({"kind":"audio","id":"bad","command":"ls"})"}).c_str(),false);
 std::vector<std::string> maximal;
 for(int i=0;i<4;++i){const char c='a'+i;maximal.push_back("{\"kind\":\"status\",\"id\":\""+std::string(32,c)+"\",\"name\":\""+std::string(48,c)+"\",\"state\":\"fresh\",\"reason\":\""+std::string(128,c)+"\",\"settings\":\"audio\",\"volume\":62.5,\"muted\":false,\"volume_supported\":true,\"mute_supported\":true,\"selected\":true,\"connected\":true}");}
 const std::string full=controlWire(0,1,4,maximal);CHECK(full.size()>1700&&full.size()<=2048);parse(full.c_str());CHECK(packet.hubPage.controls[3].volume==63);
 const std::string oversized=full+std::string(2049-full.size(),' ');parse(oversized.c_str(),false);
 const String hello=protocol::makeHello("0.8.0",true,true);CHECK(std::string(hello.c_str()).find("desktop_hub_batch_v1")!=std::string::npos);CHECK(std::string(hello.c_str()).find("desktop_hub_v1")!=std::string::npos);
 std::cout<<"PASS negotiated batch hello, real JSON arena, final-page sizes and 2048-byte wire limit\n";
}
void gestures(){
 UiManager ui;fake_arduino::setMillis(0);ui.begin(7);lift(ui,0);ui.setConnectionIndicator(ConnectionIndicator::BRIDGE_CONNECTED);ui.setHubOnline(true,0);
 uint32_t at=swipe(ui,300);CHECK(ui.baseState()==UiState::HOST_STATUS);at=swipe(ui,at);CHECK(ui.baseState()==UiState::MAC_CONTROL);
 hub::Page state;state.section=hub::Section::Control;state.revision=1;state.observedAt=1800000000;state.total=1;state.hasItem=true;strcpy(state.control.kind,"status");state.control.hasVolume=true;state.control.volumeSupported=true;state.control.volume=40;CHECK(ui.hubModel().accept(state,at));
 sample(ui,true,30,140,at);sample(ui,true,390,140,at+100);lift(ui,at+200);CHECK(ui.baseState()==UiState::MAC_CONTROL);
 hub::Action action;CHECK(ui.takeHubAction(action));CHECK(!strcmp(action.action,"volume_set"));CHECK(action.value==100);CHECK(!ui.takeHubAction(action));
 hub::Result result;strcpy(result.operationId,action.operationId);strcpy(result.state,"succeeded");strcpy(result.reason,"actual");ui.hubResult(result,at+400);at+=600;
 at=swipe(ui,at);CHECK(ui.baseState()==UiState::INFO_CARDS);at=swipe(ui,at);CHECK(ui.baseState()==UiState::TASKS);at=swipe(ui,at);CHECK(ui.baseState()==UiState::USAGE);
 CHECK(ui.takeHubAction(action));CHECK(!strcmp(action.action,"usage_refresh"));strcpy(result.operationId,action.operationId);ui.hubResult(result,at);
 sample(ui,true,20,20,at+20);lift(ui,at+160);CHECK(ui.baseState()==UiState::WALLPAPER);const bool clock=ui.clockVisible();
 sample(ui,true,80,242,at+400);lift(ui,at+600);CHECK(ui.baseState()==UiState::TASKS);CHECK(ui.clockVisible()==clock);
 ui.tick(at+60601);CHECK(ui.baseState()==UiState::WALLPAPER);
 std::cout<<"PASS six pages, volume drag commit without page swipe, glass home shortcuts and idle timeout\n";
}
void navigationAndSetup(){
 UiManager ui;fake_arduino::setMillis(0);ui.begin(7);lift(ui,0);ui.setHubOnline(true,0);
 uint32_t at=swipe(ui,300);at=swipe(ui,at);CHECK(ui.baseState()==UiState::MAC_CONTROL);
 sample(ui,true,100,222,at);sample(ui,true,430,222,at+100);lift(ui,at+200);CHECK(ui.baseState()==UiState::HOST_STATUS);
 at=swipe(ui,at+500);CHECK(ui.baseState()==UiState::MAC_CONTROL);
 sample(ui,true,400,24,at);lift(ui,at+200);hub::Action action;CHECK(ui.takeHubAction(action));CHECK(!strcmp(action.action,"device_settings"));
 ui.setHubOnline(false,at+400);ui.setHubOnline(true,at+450);CHECK(!ui.takeHubAction(action));
 fake_arduino::setMillis(at+500);ui.showSetup("public-test-AP","TESTONLY1234","");lift(ui,at+520);
 sample(ui,true,100,120,at+700);lift(ui,at+800);CHECK(ui.baseState()==UiState::SETUP);CHECK(!ui.takeSetupExitRequest());
 sample(ui,true,240,269,at+1100);lift(ui,at+1250);CHECK(ui.baseState()==UiState::WALLPAPER);CHECK(ui.takeSetupExitRequest());CHECK(!ui.takeSetupExitRequest());
 // An empty card page still permits a horizontal swipe through its body.
 at=swipe(ui,at+1700);at=swipe(ui,at);at=swipe(ui,at);CHECK(ui.baseState()==UiState::INFO_CARDS);
 sample(ui,true,100,170,at);sample(ui,true,430,170,at+100);lift(ui,at+200);CHECK(ui.baseState()==UiState::MAC_CONTROL);
 std::cout<<"PASS right swipe through blank content, local settings result isolation and explicit setup exit\n";
}
void wallpaperSurfaces(){
 std::vector<uint16_t> fb(480*320,0xF800);Arduino_Canvas canvas(fb.data());hub::Ui ui;
 const auto pixel=[&](int x,int y){return fb[x*320+319-y];};
 UiManager home;fake_arduino::setMillis(0);home.begin(7);home.setClockVisible(false);home.render(canvas,0);
 for(int x=0;x<480;++x){for(int y=20;y<60;++y)CHECK(pixel(x,y)==0xF800);for(int y=100;y<205;++y)CHECK(pixel(x,y)==0xF800);for(int y=272;y<305;++y)CHECK(pixel(x,y)==0xF800);}
 CHECK(pixel(28,222)==0xF800);CHECK((pixel(84,240)>>11)>=24);CHECK(ui.hit(0,TouchPoint{80,242}));
 for(uint8_t page=2;page<6;++page){std::fill(fb.begin(),fb.end(),0xF800);ui.render(canvas,page,0);const auto red=pixel(100,240);CHECK(pixel(0,0)==0xF800);CHECK(red!=0x0863);
  std::fill(fb.begin(),fb.end(),0x001F);ui.render(canvas,page,0);CHECK(pixel(0,0)==0x001F);CHECK(pixel(100,240)!=red);
 }
 std::cout<<"PASS wallpaper visible through every tool surface; date, clock and AI HUD areas remain clear\n";
}
void packedGlassBlend(){
 // Exhaust every channel's input pair and every 8-bit alpha. Set both
 // neighbouring channels to all four source/destination extrema to expose
 // accidental packed-lane carries, as well as the one-level error bound.
 const unsigned shifts[]={11,5,0},masks[]={31,63,31};
 unsigned maximumError=0,maximumRimError=0;size_t cases=0;
 auto scalar32=[](uint16_t background,uint16_t tint,unsigned alpha){
  const unsigned inverse=32-alpha;
  const unsigned r=((background>>11)*inverse+(tint>>11)*alpha+16)/32;
  const unsigned g=(((background>>5)&63)*inverse+((tint>>5)&63)*alpha+16)/32;
  const unsigned b=((background&31)*inverse+(tint&31)*alpha+16)/32;
  return uint16_t((r<<11)|(g<<5)|b);
 };
 auto error=[&](uint16_t actual,uint16_t reference){
  unsigned largest=0;
  for(unsigned channel=0;channel<3;++channel){
   const int difference=int((actual>>shifts[channel])&masks[channel])-int((reference>>shifts[channel])&masks[channel]);
   largest=std::max(largest,unsigned(std::abs(difference)));
  }
  return largest;
 };
 for(unsigned alpha=0;alpha<=255;++alpha){
  const unsigned alpha32=(alpha*32+127)/255;
  for(unsigned channel=0;channel<3;++channel){
   const uint16_t otherMask=uint16_t(~(masks[channel]<<shifts[channel]));
   for(unsigned background=0;background<=masks[channel];++background){
    for(unsigned tint=0;tint<=masks[channel];++tint){
     for(unsigned extremes=0;extremes<4;++extremes){
      const uint16_t source=uint16_t((background<<shifts[channel])|((extremes&1)?otherMask:0));
      const uint16_t destination=uint16_t((tint<<shifts[channel])|((extremes&2)?otherMask:0));
      const uint32_t weightedTint=surface::pack565(destination)*alpha32;
      const uint16_t actual=surface::mixPacked565(source,weightedTint,32-alpha32);
      CHECK(actual==scalar32(source,destination,alpha32));
      const uint16_t reference=surface::mix(source,destination,alpha);
      maximumError=std::max(maximumError,error(actual,reference));
      CHECK(maximumError<=1);
      if(alpha==111||alpha==124){
       for(unsigned rimAlpha:{24U,70U}){
        maximumRimError=std::max(maximumRimError,error(surface::mix(actual,0xE7FF,rimAlpha),surface::mix(reference,0xE7FF,rimAlpha)));
        CHECK(maximumRimError<=1);
       }
      }
      ++cases;
     }
    }
   }
  }
 }
 CHECK(cases==6291456);CHECK(maximumError==1);CHECK(maximumRimError==1);
 std::cout<<"PASS packed RGB565: "<<cases<<" channel/extreme cases, exact scalar32, max channel/rim error "<<maximumError<<"/"<<maximumRimError<<"\n";
}
void usageTabsAndWifiLayout(){
 std::vector<uint16_t> fb(480*320,0xFFFF);Arduino_Canvas canvas(fb.data());hub::Ui ui;ui.setOnline(true,0);
 for(int i=0;i<4;++i){hub::Page p;p.section=hub::Section::Usage;p.revision=1;p.page=i;p.pages=4;p.total=4;p.hasItem=true;
  strcpy(p.usage.provider,i<2?"codex":"claude");strcpy(p.usage.limitId,p.usage.provider);strcpy(p.usage.state,i<2?"fresh":"not_provided");p.usage.windowMinutes=i%2?300:10080;p.usage.hasWindow=true;p.usage.hasUsed=i<2;p.usage.usedPercent=43;
  CHECK(ui.model.accept(p,0)==(i==3));
 }
 ui.render(canvas,5,0);CHECK(std::find(canvas.printed.begin(),canvas.printed.end(),"等待 Claude 采集器接入")==canvas.printed.end());
 ui.endTouch(5,TouchPoint{350,84},TouchPoint{350,84},1);canvas.printed.clear();ui.render(canvas,5,1);
 CHECK(std::find(canvas.printed.begin(),canvas.printed.end(),"等待 Claude 采集器接入")!=canvas.printed.end());
 ui.endTouch(5,TouchPoint{390,280},TouchPoint{390,280},2);hub::Action a;CHECK(!ui.takeAction(a));
 // Wi-Fi status and disconnect warning occupy separate measured spans.
 ui.endTouch(2,TouchPoint{420,84},TouchPoint{420,84},3);canvas.textPositions.clear();ui.render(canvas,2,3);
 const Arduino_Canvas::TextPosition *connected=nullptr,*warning=nullptr;size_t warnings=0;
 for(const auto& t:canvas.textPositions){if(t.text=="已连接 Mac")connected=&t;if(t.text=="切换网络会断开 Bridge"){warning=&t;++warnings;}}
 CHECK(connected&&warning&&warnings==1);CHECK(connected->y==warning->y);CHECK(connected->x+text::width(connected->text.c_str(),1)<warning->x);
 CHECK(warning->x+text::width(warning->text.c_str(),1)<=480);
 std::fill(fb.begin(),fb.end(),WHITE);CHECK(surface::liquid(canvas,28,222,88,40)==0x1128);std::fill(fb.begin(),fb.end(),0);CHECK(surface::liquid(canvas,28,222,88,40)==WHITE);
 std::cout<<"PASS Codex/Claude tabs, uncollected reserve, Wi-Fi text spans and adaptive glass ink\n";
}
void lateAndBusy(){
 hub::Ui ui;ui.setOnline(true,1);ui.enter(5,100);hub::Action a;CHECK(ui.takeAction(a));CHECK(ui.busy());hub::Result wrong;strcpy(wrong.operationId,"other");strcpy(wrong.state,"succeeded");ui.result(wrong,200);CHECK(ui.busy());
 ui.setOnline(false,300);CHECK(!ui.busy());ui.setOnline(true,400);CHECK(ui.takeAction(a));CHECK(!strcmp(a.action,"operation_read"));CHECK(a.target[0]);
 std::cout<<"PASS late results ignored and reconnect queries prior operation without replay\n";
}
void controlFeedbackAndDispatch(){
 std::vector<uint16_t> fb(480*320,0);Arduino_Canvas canvas(fb.data());
 const auto printed=[&](const char* label){return std::find(canvas.printed.begin(),canvas.printed.end(),label)!=canvas.printed.end();};
 hub::Ui hub;hub.setOnline(true,1);auto p=controlBatch(1,0,1);strcpy(p.controls[0].kind,"status");p.controls[0].volumeSupported=true;p.controls[0].hasVolume=true;CHECK(hub.model.accept(p,1));
 hub.endTouch(2,{40,180},{40,180},100);hub::Action action;CHECK(hub.takeAction(action));CHECK(hub.busy());hub.tick(2,100);
 hub.endTouch(2,{140,180},{140,180},44000);CHECK(!hub.takeAction(action));hub.render(canvas,2,44000);CHECK(printed("操作进行中，请稍候"));
 hub.tick(2,45101);CHECK(!hub.busy());CHECK(!hub.takeAction(action));canvas.printed.clear();hub.render(canvas,2,45101);CHECK(printed("操作未确认；重连后查询实际结果"));
 // A tap cannot extend a lost-result timeout or replay a submitted action.
 hub.tick(2,90000);CHECK(!hub.takeAction(action));
 hub::Ui unsupported;unsupported.setOnline(true,1);p.revision=2;p.controls[0].volumeSupported=false;strcpy(p.controls[0].reason,"此输出设备不支持软件音量");CHECK(unsupported.model.accept(p,1));
 unsupported.beginTouch(2,{40,140},2);unsupported.endTouch(2,{40,140},{100,140},3);CHECK(!unsupported.takeAction(action));canvas.printed.clear();unsupported.render(canvas,2,3);CHECK(printed("此输出设备不支持软件音量"));
 UiManager ui;fake_arduino::setMillis(0);ui.begin(7);lift(ui,0);ui.setHubOnline(true,0);uint32_t at=swipe(ui,300);at=swipe(ui,at);
 p=controlBatch(1,0,3);strcpy(p.controls[0].kind,"status");p.controls[0].volumeSupported=true;p.controls[0].hasVolume=true;strcpy(p.controls[2].kind,"bluetooth");CHECK(ui.hubModel().accept(p,at));
 auto tap=[&](int x,int y){sample(ui,true,x,y,at);lift(ui,at+130);at+=400;};
 auto expect=[&](const char* name){CHECK(ui.takeHubAction(action));CHECK(!strcmp(action.action,name));hub::Result r;strcpy(r.operationId,action.operationId);strcpy(r.state,"succeeded");ui.hubResult(r,at);at+=100;};
 tap(45,180);expect("volume_set");tap(162,86);tap(70,132);expect("audio_select");tap(286,86);tap(70,132);expect("bluetooth_connect");CHECK(ui.baseState()==UiState::MAC_CONTROL);
 hub::Ui feedback;feedback.setOnline(true,at);CHECK(feedback.model.accept(p,at));
 feedback.endTouch(2,{45,180},{45,180},at);CHECK(feedback.takeAction(action));
 canvas.printed.clear();canvas.textPositions.clear();feedback.render(canvas,2,at);
 bool header=false;for(const auto& pos:canvas.textPositions)if(pos.x==14&&pos.y==62&&pos.text.find("运行中")!=std::string::npos)header=true;CHECK(header);
 hub::Result failed;strcpy(failed.operationId,action.operationId);strcpy(failed.state,"failed");strcpy(failed.reason,"耳机连接超时");feedback.result(failed,at+1);
 canvas.printed.clear();canvas.textPositions.clear();feedback.render(canvas,2,at+2);
 header=false;for(const auto& pos:canvas.textPositions)if(pos.x==14&&pos.y==62&&pos.text.find("失败")!=std::string::npos)header=true;CHECK(header);CHECK(printed("耳机连接超时"));
 std::cout<<"PASS volume/output/Bluetooth tap dispatch, visible action-state header, busy feedback, unsupported volume and non-extending timeout\n";
}
void qr(){
 uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(17)],code[qrcodegen_BUFFER_LEN_FOR_VERSION(17)];
 std::string value="https://example.com/"+std::string(492,'x');CHECK(value.size()==512);CHECK(qrcodegen_encodeText(value.c_str(),temp,code,qrcodegen_Ecc_LOW,1,17,qrcodegen_Mask_AUTO,true));CHECK(qrcodegen_getSize(code)<=85);CHECK(qrcodegen_getModule(code,0,0));
 std::vector<uint16_t> fb(480*320,0);Arduino_Canvas canvas(fb.data());hub::Ui ui;auto p=makePage(1,0,1);strcpy(p.card.kind,"link");strcpy(p.card.url,value.c_str());CHECK(ui.model.accept(p,0));ui.render(canvas,3,0);
 CHECK(std::count(fb.begin(),fb.end(),uint16_t(WHITE))>500);
 for(int x=250;x<480;++x)for(int y=0;y<320;++y)if(fb[x*320+319-y]==WHITE)CHECK(y>=72&&y<248);
 std::cout<<"PASS 512-byte QR generation and landscape framebuffer rendering\n";
}
int main(){try{paging();protocolChecks();controlBatchPaging();controlBatchProtocol();gestures();navigationAndSetup();wallpaperSurfaces();packedGlassBlend();usageTabsAndWifiLayout();lateAndBusy();controlFeedbackAndDispatch();qr();std::cout<<"12/12 desktop hub test groups passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
