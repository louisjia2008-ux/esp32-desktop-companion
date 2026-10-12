#pragma once
#include <Arduino_GFX_Library.h>
#include "hub_model.h"
#include "touch_driver.h"
#include "qr/qrcodegen.h"
namespace hub {
class Ui {
 public:
 Model model;
 void setSession(uint32_t first,uint32_t second){session_=first?first:1;session2_=second;}
 void setOnline(bool online,uint32_t nowMs);
 bool hit(uint8_t page,const TouchPoint& p)const;
 bool scrollHit(uint8_t page,const TouchPoint& p)const;
 void beginTouch(uint8_t page,const TouchPoint& p,uint32_t nowMs);
 void drag(uint8_t page,const TouchPoint& p);
 // Returns 1 for home, 4 for tasks, 5 for usage, 0 otherwise.
 uint8_t endTouch(uint8_t page,const TouchPoint& start,const TouchPoint& end,uint32_t nowMs);
 void enter(uint8_t page,uint32_t nowMs);
 void tick(uint8_t page,uint32_t nowMs);
 void render(Arduino_Canvas& gfx,uint8_t page,uint32_t nowMs);
 void renderHome(Arduino_Canvas& gfx,uint32_t nowMs);
 bool takeAction(Action& action);
 void result(const Result& result,uint32_t nowMs);
 void sendFailed();
 bool busy()const{return busy_;}
 private:
 void action(const char* name,const char* target="",int value=-1,bool boolean=false);
 void unavailableVolume(bool mute=false);
 const Control* status()const;
 size_t rows(const char* kind,uint8_t* indices)const;
 size_t usageRows(uint8_t* indices)const;
 const Usage* usageWindow(const char* provider,uint32_t minutes)const;
 bool online_=false,busy_=false,pending_=false,draggingVolume_=false;
 uint8_t tab_=0,listOffset_=0,card_=0,taskPage_=0,usagePage_=0,usageProvider_=0,bodyLine_=0,previewVolume_=0;
 uint32_t sequence_=0,session_=0,session2_=0,operationMs_=0,lastUsageRefreshMs_=0;
 Action action_{};
 char currentId_[33]={},reason_[129]={},operationState_[17]={};
 uint8_t qr_[qrcodegen_BUFFER_LEN_FOR_VERSION(17)]={},qrTemp_[qrcodegen_BUFFER_LEN_FOR_VERSION(17)]={};
 char qrId_[33]={};uint32_t qrRevision_=0;bool qrValid_=false;
};
}
