#include "hub_ui.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "board_config.h"
#include "ui/cjk_font.h"
#include "ui/glass_surface.h"
#include "ui/text_layout.h"
namespace {
void rect(Arduino_Canvas& g,int x,int y,int w,int h,uint16_t c){
 const int right=std::min<int>(x+w,board::kDisplayWidth),bottom=std::min<int>(y+h,board::kDisplayHeight);x=std::max(x,0);y=std::max(y,0);
 if(right<=x||bottom<=y)return;
 auto* fb=g.getFramebuffer();if(board::kDisplayRotation!=1||!fb){g.fillRect(x,y,right-x,bottom-y,c);return;}
 for(int px=x;px<right;++px)std::fill_n(fb+px*board::kPanelWidth+board::kPanelWidth-bottom,bottom-y,c);
}
void line(Arduino_Canvas& g,const char* s,int x,int y,int width=448,uint16_t color=WHITE){
 char normalized[513],visible[513];text::normalize(s,normalized,sizeof(normalized));text::ellipsize(normalized,width,1,visible,sizeof(visible));
 g.setFont(uifont::cjkFont());g.setUTF8Print(true);g.setTextWrap(false);g.setTextSize(1);g.setTextColor(color);g.setCursor(x,y+14);g.print(visible);
 g.setFont(static_cast<const GFXfont*>(nullptr));g.setUTF8Print(false);g.setTextWrap(true);
}
void button(Arduino_Canvas& g,const char* label,int x,int y,int width=80,bool selected=false){
 surface::glass(g,x,y,width,32,selected?0x1B71:0x0863,selected?214:188,10);
 g.drawRoundRect(x,y,width,32,10,selected?0x7DFF:0x52AA);line(g,label,x+8,y+8,width-16);
}
bool in(const TouchPoint& p,int x,int y,int w,int h){return int(p.x)>=x&&int(p.x)<x+w&&int(p.y)>=y&&int(p.y)<y+h;}
const char* stateText(const char* state){
 if(!strcmp(state,"running"))return "运行中";if(!strcmp(state,"waiting_input"))return "等待输入";if(!strcmp(state,"success"))return "成功";if(!strcmp(state,"failed"))return "失败";
 if(!strcmp(state,"stale"))return "长时间未更新";if(!strcmp(state,"round_done"))return "本轮结束";if(!strcmp(state,"round_stopped"))return "本轮已停止";
 if(!strcmp(state,"error"))return "读取失败";if(!strcmp(state,"not_provided"))return "暂未提供";if(!strcmp(state,"fresh"))return "最新";
 if(!strcmp(state,"succeeded"))return "成功";if(!strcmp(state,"unconfirmed"))return "结果未确认";return state;
}
}
namespace hub {
const Control* Ui::status()const{for(size_t i=0;i<model.count(Section::Control);++i)if(!strcmp(model.controls()[i].kind,"status"))return &model.controls()[i];return nullptr;}
size_t Ui::rows(const char* kind,uint8_t* indices)const{size_t n=0;for(size_t i=0;i<model.count(Section::Control);++i)if(!strcmp(model.controls()[i].kind,kind))indices[n++]=i;return n;}
size_t Ui::usageRows(uint8_t* indices)const{size_t n=0;for(size_t i=0;i<model.count(Section::Usage);++i)if(!strcmp(model.usage()[i].provider,usageProvider_?"claude":"codex"))indices[n++]=i;return n;}
const Usage* Ui::usageWindow(const char* provider,uint32_t minutes)const{
 const Usage* selected=nullptr;for(size_t i=0;i<model.count(Section::Usage);++i){const auto& u=model.usage()[i];if(strcmp(u.provider,provider)||u.windowMinutes!=minutes)continue;
  if(!selected||(u.hasUsed&&(!selected->hasUsed||u.usedPercent>selected->usedPercent)))selected=&u;
 }return selected;
}
void Ui::action(const char* name,const char* target,int value,bool boolean){
 if(busy_||pending_){strcpy(reason_,"操作进行中，请稍候");return;}
 if(!online_&&strcmp(name,"device_settings")){strcpy(reason_,"Bridge 已断开；连接后再操作");strcpy(operationState_,"unconfirmed");return;}
 action_=Action{};snprintf(action_.operationId,sizeof(action_.operationId),"h%08lx%08lx-%08lx",(unsigned long)session_,(unsigned long)session2_,(unsigned long)++sequence_);
 strlcpy(action_.action,name,sizeof(action_.action));strlcpy(action_.target,target,sizeof(action_.target));
 if(value>=0){action_.hasValue=true;action_.isBoolean=boolean;action_.booleanValue=value!=0;action_.value=value;}
 strlcpy(currentId_,action_.operationId,sizeof(currentId_));strcpy(reason_,"操作进行中");strcpy(operationState_,"running");busy_=true;pending_=true;operationMs_=0;
 Serial.printf("HUB_ACTION action=%s state=queued\n",name);
}
void Ui::unavailableVolume(bool mute){
 if(busy_||pending_){strcpy(reason_,"操作进行中，请稍候");return;}
 const auto* c=status();
 strlcpy(reason_,c&&c->reason[0]?c->reason:!c?"Mac 状态尚未同步，请刷新":mute?"此输出设备不支持软件静音":"此输出设备不支持软件音量",sizeof(reason_));
 strcpy(operationState_,"not_provided");
}
void Ui::setOnline(bool online,uint32_t nowMs){
 if(online_&&!online){model.disconnected();if(busy_){busy_=false;strcpy(operationState_,"unconfirmed");strcpy(reason_,"断线，结果未确认；重连后查询");}}
 if(!online_&&online&&currentId_[0]&&!strcmp(operationState_,"unconfirmed")){
  action_=Action{};snprintf(action_.operationId,sizeof(action_.operationId),"q%08lx%08lx-%08lx",(unsigned long)session_,(unsigned long)session2_,(unsigned long)++sequence_);
  strcpy(action_.action,"operation_read");strlcpy(action_.target,currentId_,sizeof(action_.target));pending_=true;
 }
 online_=online;if(!session_)session_=nowMs^0x8f73a951U;
}
bool Ui::takeAction(Action& a){if(!pending_)return false;a=action_;pending_=false;return true;}
void Ui::result(const Result& r,uint32_t nowMs){if(strcmp(r.operationId,currentId_))return;strlcpy(operationState_,r.state,sizeof(operationState_));strlcpy(reason_,r.reason,sizeof(reason_));busy_=!strcmp(r.state,"running");operationMs_=nowMs;Serial.printf("HUB_RESULT state=%s\n",r.state);}
void Ui::sendFailed(){busy_=false;strcpy(operationState_,"unconfirmed");strcpy(reason_,"操作未确认；重连后查询实际结果");}
void Ui::enter(uint8_t page,uint32_t nowMs){listOffset_=0;bodyLine_=0;if(page==5){lastUsageRefreshMs_=nowMs;action("usage_refresh");}}
void Ui::tick(uint8_t page,uint32_t nowMs){
 model.expire(nowMs);
 if(busy_&&!operationMs_)operationMs_=nowMs;
 if(busy_&&nowMs-operationMs_>=45000){sendFailed();operationMs_=0;}
 if(page==5&&nowMs-lastUsageRefreshMs_>=60000&&!busy_){lastUsageRefreshMs_=nowMs;action("usage_refresh");}
}
bool Ui::hit(uint8_t page,const TouchPoint& p)const{
 if(page==0)return in(p,28,222,88,40)||in(p,124,222,160,40)||in(p,292,222,160,40);
 if(page<2)return false;
 if(in(p,10,8,72,32)||in(p,374,8,96,32))return true;
 // Only real controls own a contact; labels, gaps and empty lists stay
 // available for left/right page gestures.
 if(page==2){
  if(in(p,10,70,460,34)||in(p,20,262,130,32)||in(p,270,262,95,32)||in(p,370,262,100,32))return true;
  if(tab_==0)return in(p,30,130,360,28)||in(p,30,170,80,32)||in(p,120,170,80,32)||in(p,210,170,80,32)||in(p,320,170,130,32);
  if(tab_==3){uint8_t indices[Model::kControls];const size_t n=rows("wifi",indices);for(size_t i=listOffset_;i<n&&i<listOffset_+3;++i)if(in(p,350,115+int(i-listOffset_)*50,102,32))return true;}
  return false;
 }
 if(page==3){
  if(!model.count(Section::Cards))return false;
  return in(p,10,254,110,32)||in(p,128,254,92,32)||in(p,228,254,72,32)||
         (card_<model.count(Section::Cards)&&!strcmp(model.cards()[card_].kind,"file")&&in(p,308,254,160,32));
 }
 if(page==4){
  if(model.count(Section::Tasks)>4&&in(p,350,268,118,32))return true;
  for(size_t i=taskPage_*4;i<model.count(Section::Tasks)&&i<taskPage_*4+4;++i){const auto& task=model.tasks()[i];const int y=76+int(i%4)*46;
   if((!task.session&&in(p,338,y+5,62,32))||(task.hasDetails&&in(p,406,y+5,62,32)))return true;
  }
  return false;
 }
 if(page==5)return in(p,10,70,460,32)||in(p,10,266,90,32)||in(p,110,266,90,32)||in(p,340,266,128,32);
 return false;
}
bool Ui::scrollHit(uint8_t page,const TouchPoint& p)const{
 if(page==2&&tab_>0){
  uint8_t indices[Model::kControls];const size_t n=rows(tab_==1?"audio":tab_==2?"bluetooth":"wifi",indices);
  for(size_t i=listOffset_;i<n&&i<listOffset_+3;++i)if(in(p,20,110+int(i-listOffset_)*50,440,42))return true;
 }
 if(page==3&&model.count(Section::Cards))return in(p,10,78,460,172);
 if(page==4&&model.count(Section::Tasks))return in(p,10,76,460,std::min<size_t>(4,model.count(Section::Tasks))*46);
 if(page==5&&model.count(Section::Usage))return in(p,10,110,460,146);
 return false;
}
void Ui::beginTouch(uint8_t page,const TouchPoint& p,uint32_t nowMs){
 if(!session_)session_=nowMs^0x8f73a951U;
 if(page==2&&tab_==0&&in(p,30,130,360,28)){draggingVolume_=true;drag(page,p);}
}
void Ui::drag(uint8_t page,const TouchPoint& p){if(page==2&&draggingVolume_)previewVolume_=std::max(0,std::min(100,(int(p.x)-30)*100/360));}
uint8_t Ui::endTouch(uint8_t page,const TouchPoint& start,const TouchPoint& end,uint32_t nowMs){
 const int dx=int(end.x)-int(start.x),dy=int(end.y)-int(start.y);const bool tap=std::abs(dx)<20&&std::abs(dy)<20;
 if(draggingVolume_){draggingVolume_=false;const auto* c=status();if(c&&c->volumeSupported)action("volume_set","",previewVolume_);else unavailableVolume();return 0;}
 if(page==0){if(tap&&in(start,28,222,88,40))return 4;if(tap&&(in(start,124,222,160,40)||in(start,292,222,160,40))){usageProvider_=start.x>=292;usagePage_=0;return 5;}return 0;}
 if(page>=2&&tap&&in(start,10,8,72,32))return 1;
 if(page>=2&&tap&&in(start,374,8,96,32)){
  if(!busy_&&!pending_){action_=Action{};strcpy(action_.action,"device_settings");pending_=true;currentId_[0]=0;reason_[0]=0;operationState_[0]=0;}
  return 0;
 }
 if(page==2){
  if(tap&&in(start,10,70,460,34)){tab_=std::min<int>(3,(int(start.x)-10)/116);listOffset_=0;return 0;}
  if(tap&&in(start,20,262,130,32)){action("settings_open",tab_==2?"bluetooth":tab_==3?"wifi":"audio");return 0;}
  if(tap&&in(start,270,262,95,32)){action("control_refresh");return 0;}
  if(tap&&in(start,370,262,100,32)){action("airdrop_open");return 0;}
  if(tab_==0){const auto* c=status();if(tap&&in(start,30,170,270,32)){if(c&&c->volumeSupported)action("volume_set","",20+20*((int(start.x)-30)/90));else unavailableVolume();}
   else if(tap&&in(start,320,170,130,32)){if(c&&c->muteSupported)action("mute_set","",!c->muted,true);else unavailableVolume(true);}return 0;}
  uint8_t indices[Model::kControls];const char* kind=tab_==1?"audio":tab_==2?"bluetooth":"wifi";const size_t n=rows(kind,indices);
  if(std::abs(dy)>=30){if(dy<0&&listOffset_+3<n)++listOffset_;else if(dy>0&&listOffset_)--listOffset_;return 0;}
  if(tap&&in(start,20,110,440,150)){size_t index=listOffset_+(start.y-110)/50;if(index>=n)return 0;const auto& c2=model.controls()[indices[index]];
   if(tab_==1)action("audio_select",c2.id);
   else if(tab_==2)action(c2.connected?"bluetooth_disconnect":"bluetooth_connect",c2.id);
   else if(in(start,350,110+int(index-listOffset_)*50,110,42))action("wifi_select",c2.id);
   else {strcpy(reason_,"切换可能断开 Bridge；点击右侧确认切换");strcpy(operationState_,"unconfirmed");}
  }return 0;
 }
 if(page==3){
  if(std::abs(dy)>=30&&in(start,10,78,460,172)){if(dy<0&&bodyLine_<24)++bodyLine_;else if(dy>0&&bodyLine_)--bodyLine_;return 0;}
  const size_t n=model.count(Section::Cards);
  if(tap&&in(start,10,254,110,32)&&n){const int direction=start.x<65?-1:1;for(size_t i=0;i<n;++i){card_=uint8_t((card_+n+direction)%n);if(model.cardAvailable(card_,nowMs))break;}bodyLine_=0;qrId_[0]=0;return 0;}
  if(card_>=n||!model.cardAvailable(card_,nowMs)||!tap)return 0;
  const auto& c=model.cards()[card_];if(in(start,128,254,92,32))action("card_pin",c.id,!c.pinned,true);
  else if(in(start,228,254,72,32))action("card_clear",c.id);
  else if(in(start,308,254,160,32)&&!strcmp(c.kind,"file"))action("file_share",c.fileRef);
  return 0;
 }
 if(page==4){
  if(std::abs(dy)>=30){if(dy<0&&model.count(Section::Tasks)>4)taskPage_=1;else if(dy>0)taskPage_=0;return 0;}
  if(tap&&in(start,350,268,118,26)){taskPage_=taskPage_?0:1;return 0;}
  if(tap&&in(start,10,76,460,184)){const size_t index=taskPage_*4+(start.y-76)/46;if(index>=model.count(Section::Tasks))return 0;const auto& t=model.tasks()[index];
   if(!t.session&&in(start,338,76+int(index%4)*46,62,42))action("task_viewed",t.id);
   else if(t.hasDetails&&in(start,406,76+int(index%4)*46,62,42))action("details_open",t.id);
   else {strlcpy(reason_,t.summary,sizeof(reason_));strcpy(operationState_,t.state);}
  }return 0;
 }
 if(page==5){
  uint8_t indices[Model::kUsage];const size_t n=usageRows(indices);
  if(tap&&in(start,10,70,460,32)){usageProvider_=start.x>=244;usagePage_=0;reason_[0]=0;return 0;}
  if(std::abs(dy)>=30){if(dy<0&&(usagePage_+1)*2<n)++usagePage_;else if(dy>0&&usagePage_)--usagePage_;return 0;}
  if(tap&&in(start,340,266,128,28)){
   if(usageProvider_){strcpy(reason_,"Claude 数据由 Mac 采集器更新");strcpy(operationState_,"not_provided");}
   else{action("usage_refresh");lastUsageRefreshMs_=nowMs;}return 0;
  }
  if(tap&&in(start,10,266,90,28)&&usagePage_)--usagePage_;
  else if(tap&&in(start,110,266,90,28)&&(usagePage_+1)*2<n)++usagePage_;
 }
 return 0;
}
void Ui::renderHome(Arduino_Canvas& g,uint32_t nowMs){
 size_t active=0;for(size_t i=0;i<model.count(Section::Tasks);++i)if(!strcmp(model.tasks()[i].state,"running")||!strcmp(model.tasks()[i].state,"waiting_input"))++active;
 char tasks[32];if(model.revision(Section::Tasks))snprintf(tasks,sizeof(tasks),"任务 %u",unsigned(active));else strcpy(tasks,"任务 --");
 const uint16_t taskInk=surface::liquid(g,28,222,88,40);g.fillCircle(43,242,3,taskInk==WHITE?0x7DFF:0x2BD0);line(g,tasks,58,234,56,taskInk);
 for(int provider=0;provider<2;++provider){const int x=provider?292:124;const char* key=provider?"claude":"codex";
  const auto* week=usageWindow(key,10080);const auto* hours=usageWindow(key,300);
  auto value=[](const Usage* window,char* out,size_t size){if(window&&window->hasUsed&&!strcmp(window->state,"fresh"))snprintf(out,size,"%.0f%%",100-window->usedPercent);else strlcpy(out,window&&!strcmp(window->state,"stale")?"过期":window&&!strcmp(window->state,"error")?"失败":"--",size);};
  char weekly[16],hourly[16],top[64],bottom[64];value(week,weekly,sizeof(weekly));value(hours,hourly,sizeof(hourly));
  snprintf(top,sizeof(top),"%s 周余 %s",provider?"Claude":"Codex",weekly);
  if(hours&&hours->hasUsed)snprintf(bottom,sizeof(bottom),"5h 余 %s",hourly);
  else strcpy(bottom,provider&&(!week||!week->observedAt)?"5h 待接入":"5h 暂未提供");
  const uint16_t ink=surface::liquid(g,x,222,160,40);line(g,top,x+10,228,140,ink);line(g,bottom,x+10,246,140,ink);
 }
 (void)nowMs;
}
void Ui::render(Arduino_Canvas& g,uint8_t page,uint32_t nowMs){
 surface::glass(g,8,68,464,228,0x0863,118,16);
 surface::glass(g,90,8,276,32,0x0863,172,12);surface::glass(g,10,44,460,20,0x0863,160,10);
 button(g,"主页",10,8,72);const char* titles[]={"","","Mac 控制","信息卡","任务","AI 用量"};
 char heading[48];if(page==4)snprintf(heading,sizeof(heading),"任务 %u",unsigned(model.count(Section::Tasks)));else strlcpy(heading,titles[page],sizeof(heading));
 line(g,heading,182,16,182);button(g,"设备设置",374,8,96);
 if(page==2&&tab_!=3&&reason_[0]){
  char feedback[192];snprintf(feedback,sizeof(feedback),"%s：%s",stateText(operationState_),reason_);
  const uint16_t color=!strcmp(operationState_,"failed")?0xF9E7:!strcmp(operationState_,"unconfirmed")?0xFDE0:!strcmp(operationState_,"succeeded")?0x9FE7:WHITE;
  line(g,feedback,14,48,450,color);
 }else if(page==2&&tab_==3){
  line(g,online_?"已连接 Mac":"Bridge 离线",14,48,160,online_?0x9FE7:0xFDE0);
  line(g,"切换网络会断开 Bridge",190,48,276,0xFDE0);
 }else{
  line(g,online_?"已连接 Mac":"Bridge 离线：查看缓存",14,48,270,online_?0x9FE7:0xFDE0);
  line(g,"左右滑动翻页",340,48,132,0xBDF7);
 }
 if(page==2){
  const char* tabs[]={"音量","输出","蓝牙","Wi-Fi"};for(int i=0;i<4;++i)button(g,tabs[i],10+i*116,70,108,i==tab_);
  if(tab_==0){const auto* c=status();char value[64];if(c&&c->hasVolume)snprintf(value,sizeof(value),"音量 %u%%",unsigned(draggingVolume_?previewVolume_:c->volume));else strcpy(value,c&&!c->volumeSupported?"请在输出设备上调节音量":"音量暂未提供");
   line(g,value,30,110,400);rect(g,30,140,360,8,0x3186);if(c&&c->hasVolume)rect(g,30,140,360*(draggingVolume_?previewVolume_:c->volume)/100,8,0x9FE7);
   button(g,"20%",30,170,80);button(g,"40%",120,170,80);button(g,"60%",210,170,80);button(g,c&&c->muted?"取消静音":"静音",320,170,130);
   const char* cursor=c?c->reason:"在 Mac 构建并启动原生助手";
   for(int row=0;row<2&&*cursor;++row){size_t next=0;size_t bytes=text::fitLine(cursor,432,1,next);char visible[129];bytes=std::min(bytes,sizeof(visible)-1);memcpy(visible,cursor,bytes);visible[bytes]=0;line(g,visible,24,214+row*20,432,0xD6BA);if(!next)break;cursor+=next;}
  }else{uint8_t indices[Model::kControls];const char* kind=tab_==1?"audio":tab_==2?"bluetooth":"wifi";const size_t n=rows(kind,indices);
   if(!n)line(g,"暂未提供：在 Mac 检查配对、权限或网络",20,128,440);
   for(size_t i=listOffset_;i<n&&i<listOffset_+3;++i){const auto& c=model.controls()[indices[i]];const int y=110+(i-listOffset_)*50;surface::glass(g,20,y,440,42,0x0863,148,10);line(g,c.name,28,y+4,tab_==3?306:412);line(g,tab_==2?(c.connected?"已连接；点击断开":"未连接；点击连接"):c.selected?"当前":"点击选择",28,y+23,312,0xBDF7);if(tab_==3)button(g,"确认切换",350,y+5,102);}
  }
  button(g,"Mac 设置",20,262,130);button(g,"刷新",270,262,95);button(g,"隔空投送",370,262,100);
 }else if(page==3){
  const size_t n=model.count(Section::Cards);size_t available=0;for(size_t i=0;i<n;++i)if(model.cardAvailable(i,nowMs))++available;
  if(!model.cardAvailable(card_,nowMs)){for(size_t i=0;i<n;++i)if(model.cardAvailable(i,nowMs)){card_=i;break;}}
  if(!available)line(g,"暂无未到期信息卡；在 Mac 创建",24,130,440);
  else{const auto& c=model.cards()[card_];const bool link=!strcmp(c.kind,"link");line(g,c.title,16,80,link?224:448);
   const char* content=link?c.url:c.body;char normalized[513];text::normalize(content,normalized,sizeof(normalized));const char* cursor=normalized;const int width=link?224:448;
   for(size_t skip=0;skip<bodyLine_&&*cursor;++skip){size_t next=0;text::fitLine(cursor,width,1,next);if(!next)break;cursor+=next;}
   for(int l=0;l<7&&*cursor;++l){size_t next=0;const size_t bytes=text::fitLine(cursor,width,1,next);char visible[513];memcpy(visible,cursor,std::min(bytes,sizeof(visible)-1));visible[std::min(bytes,sizeof(visible)-1)]=0;line(g,visible,16,108+l*20,width);if(!next)break;cursor+=next;}
   if(link){if(strcmp(qrId_,c.id)||qrRevision_!=model.revision(Section::Cards)){strlcpy(qrId_,c.id,sizeof(qrId_));qrRevision_=model.revision(Section::Cards);qrValid_=qrcodegen_encodeText(c.url,qrTemp_,qr_,qrcodegen_Ecc_LOW,1,15,qrcodegen_Mask_AUTO,true);}
    if(qrValid_){const int size=qrcodegen_getSize(qr_),scale=std::min(4,172/(size+8)),side=(size+8)*scale,x=258+(196-side)/2,y=72;rect(g,x,y,side,side,WHITE);for(int row=0;row<size;++row)for(int col=0;col<size;++col)if(qrcodegen_getModule(qr_,col,row))rect(g,x+(col+4)*scale,y+(row+4)*scale,scale,scale,0);}
    else line(g,"二维码生成失败",250,160,220);
   }
   button(g,"上张",10,254,52);button(g,"下张",66,254,54);button(g,c.pinned?"取消固定":"固定",128,254,92);button(g,"清除",228,254,72);if(!strcmp(c.kind,"file"))button(g,"Mac 分享面板",308,254,160);
  }
 }else if(page==4){
  const size_t n=model.count(Section::Tasks);if(!n)line(g,"暂无任务；在 Mac 使用 task-run",20,130,440);
  if(taskPage_*4>=n)taskPage_=0;
  for(size_t i=taskPage_*4;i<n&&i<taskPage_*4+4;++i){const auto& t=model.tasks()[i];const int y=76+(i%4)*46;surface::glass(g,10,y,460,42,0x0863,156,10);line(g,t.title,18,y+2,310);char state[112];const uint32_t epoch=model.epoch(nowMs);snprintf(state,sizeof(state),"%s %s %lus前",t.session?"会话":t.source,stateText(t.state),(unsigned long)(epoch>t.updatedAt?epoch-t.updatedAt:0));line(g,state,18,y+22,310,0xBDF7);
   if(!t.session)button(g,t.viewed?"已看":"查看",338,y+5,62,t.viewed);if(t.hasDetails)button(g,"详情",406,y+5,62);
  }if(n>4)button(g,taskPage_?"上一页":"下一页",350,268,118);
 }else if(page==5){
  button(g,"Codex",10,70,226,!usageProvider_);button(g,"Claude Code",244,70,226,usageProvider_);
  uint8_t indices[Model::kUsage];const size_t n=usageRows(indices);if(!n)line(g,usageProvider_?"等待 Claude 采集器接入":"Codex 用量暂未提供",20,142,440);if(usagePage_*2>=n)usagePage_=0;
  for(size_t i=usagePage_*2;i<n&&i<usagePage_*2+2;++i){const auto& u=model.usage()[indices[i]];const int y=110+(i%2)*76;surface::glass(g,10,y,460,70,0x0863,158,12);
   const bool claudePending=!strcmp(u.provider,"claude")&&!u.hasUsed&&!strcmp(u.state,"not_provided");
   const char* provider=!strcmp(u.provider,"codex")?"Codex":!strcmp(u.provider,"claude")?"Claude Code":u.provider;
   char header[96];if(u.windowMinutes==300)snprintf(header,sizeof(header),"%s 五小时",provider);else if(u.windowMinutes==10080)snprintf(header,sizeof(header),"%s 周",provider);else snprintf(header,sizeof(header),"%s %lum",provider,(unsigned long)u.windowMinutes);line(g,header,18,y+4,440);
   char value[96];if(u.hasUsed)snprintf(value,sizeof(value),"剩余 %.1f%%  已用 %.1f%%  %s",100-u.usedPercent,u.usedPercent,!strcmp(u.state,"stale")?"已过期":stateText(u.state));else snprintf(value,sizeof(value),"%s",claudePending?"等待 Claude 采集器接入":stateText(u.state));line(g,value,18,y+28,440);
   const uint32_t epoch=model.epoch(nowMs);if(u.hasReset){const uint32_t seconds=u.resetsAt>epoch?u.resetsAt-epoch:0;snprintf(value,sizeof(value),seconds?"重置倒计时 %luh %lum":"等待重新读取确认重置",(unsigned long)(seconds/3600),(unsigned long)(seconds/60%60));}else strcpy(value,claudePending?"接入后显示真实剩余与重置时间":"重置时间暂未提供");line(g,value,18,y+52,440,0xBDF7);
  }button(g,"上一页",10,266,90);button(g,"下一页",110,266,90);button(g,usageProvider_?"Mac 更新":"刷新 Codex",340,266,128);
  if(n){char count[24];snprintf(count,sizeof(count),"%u/%u",unsigned(usagePage_+1),unsigned((n+1)/2));line(g,count,242,274,80,0xBDF7);}
 }
 if(reason_[0]){surface::glass(g,10,298,460,22,0x0863,208,8);line(g,reason_,14,300,450,!strcmp(operationState_,"failed")?0xF9E7:0xD6BA);}
}
}
