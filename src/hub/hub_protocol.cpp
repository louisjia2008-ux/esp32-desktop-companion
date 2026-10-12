#include "hub_protocol.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <initializer_list>
namespace {
bool keys(JsonObject o,std::initializer_list<const char*> allowed){for(JsonPair f:o){bool found=false;for(auto a:allowed)if(strcmp(a,f.key().c_str())==0){found=true;break;}if(!found)return false;}return true;}
bool str(JsonObject o,const char* key,char* dest,size_t size,bool required=false){
 if(!o.containsKey(key)){return !required;}
 if(!o[key].is<const char*>())return false;
 const JsonString v=o[key].as<JsonString>();if(v.size()>=size||v.size()!=strlen(v.c_str())||(required&&!v.size()))return false;
 for(size_t i=0;i<v.size();++i)if(uint8_t(v.c_str()[i])<32&&v.c_str()[i]!='\n'&&v.c_str()[i]!='\t')return false;
 strlcpy(dest,v.c_str(),size);return true;
}
bool u32(JsonObject o,const char* key,uint32_t& value,bool required=true){if(!o.containsKey(key))return !required;if(o[key].is<bool>()||!o[key].is<uint32_t>())return false;value=o[key].as<uint32_t>();return true;}
bool flag(JsonObject o,const char* key,bool& value){if(!o.containsKey(key)||o[key].isNull())return true;if(!o[key].is<bool>())return false;value=o[key].as<bool>();return true;}
bool id(const char* s){return *s&&strlen(s)<=32&&strspn(s,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.")==strlen(s);}
bool control(JsonObject o,hub::Control& c){
 if(o.isNull()||!keys(o,{"kind","id","name","state","reason","settings","volume","muted","volume_supported","mute_supported","selected","connected"})||!str(o,"kind",c.kind,sizeof(c.kind),true)||!str(o,"id",c.id,sizeof(c.id))||!str(o,"name",c.name,sizeof(c.name))||!str(o,"state",c.state,sizeof(c.state))||!str(o,"reason",c.reason,sizeof(c.reason))||!str(o,"settings",c.settings,sizeof(c.settings))||!flag(o,"muted",c.muted)||!flag(o,"volume_supported",c.volumeSupported)||!flag(o,"mute_supported",c.muteSupported)||!flag(o,"selected",c.selected)||!flag(o,"connected",c.connected))return false;
 if(strcmp(c.kind,"status")&&strcmp(c.kind,"permission")&&strcmp(c.kind,"audio")&&strcmp(c.kind,"bluetooth")&&strcmp(c.kind,"wifi"))return false;
 if(strcmp(c.kind,"audio")==0||strcmp(c.kind,"bluetooth")==0||strcmp(c.kind,"wifi")==0)if(!id(c.id))return false;
 if(o.containsKey("volume")&&!o["volume"].isNull()){if(o["volume"].is<bool>()||!o["volume"].is<float>())return false;const float v=o["volume"].as<float>();if(!std::isfinite(v)||v<0||v>100)return false;c.hasVolume=true;c.volume=uint8_t(v+0.5F);}
 return true;
}
}
namespace hub {
bool parsePage(JsonObject root,Page& p){
 if(!keys(root,{"v","type","section","revision","page","pages","total","observed_at","items"}))return false;
 const char* section=root["section"]|"";
 if(strcmp(section,"control")==0)p.section=Section::Control;else if(strcmp(section,"cards")==0)p.section=Section::Cards;
 else if(strcmp(section,"tasks")==0)p.section=Section::Tasks;else if(strcmp(section,"usage")==0)p.section=Section::Usage;else return false;
 if(!u32(root,"revision",p.revision)||!p.revision||!u32(root,"observed_at",p.observedAt))return false;
 uint32_t page=0,pages=0,total=0;const uint32_t capacities[]={Model::kControls,Model::kCards,Model::kTasks,Model::kUsage};
 if(!u32(root,"page",page)||!u32(root,"pages",pages)||!u32(root,"total",total)||total>capacities[size_t(p.section)]||!pages||page>=pages)return false;
 const uint32_t singlePages=total?total:1;
 const bool batch=p.section==Section::Control&&pages!=singlePages;
 const uint32_t stride=batch?Page::kControlBatch:1;
 if(pages!=(total?(total+stride-1)/stride:1))return false;
 const uint32_t expectedItems=total?std::min(stride,total-page*stride):0;
 p.page=page;p.pages=pages;p.total=total;
 JsonArray items=root["items"].as<JsonArray>();if(items.isNull()||items.size()!=expectedItems)return false;
 p.hasItem=total>0;if(!p.hasItem)return true;
 JsonObject o=items[0].as<JsonObject>();if(o.isNull())return false;
 switch(p.section){
 case Section::Cards:{
  auto& c=p.card;
  if(!keys(o,{"id","kind","title","body","url","file_ref","pinned","created_at","expires_at"})||!str(o,"id",c.id,sizeof(c.id),true)||!id(c.id)||!str(o,"kind",c.kind,sizeof(c.kind),true)||!str(o,"title",c.title,sizeof(c.title),true)||!str(o,"body",c.body,sizeof(c.body))||!str(o,"url",c.url,sizeof(c.url))||!str(o,"file_ref",c.fileRef,sizeof(c.fileRef))||!flag(o,"pinned",c.pinned)||!u32(o,"created_at",c.createdAt)||!u32(o,"expires_at",c.expiresAt))return false;
  if(strcmp(c.kind,"text")&&strcmp(c.kind,"link")&&strcmp(c.kind,"file"))return false;
  if(strcmp(c.kind,"link")==0&&strncmp(c.url,"https://",8)&&strncmp(c.url,"http://",7))return false;
  if(strcmp(c.kind,"file")==0&&!id(c.fileRef))return false;
  return true;}
 case Section::Control:{
  p.controlCount=items.size();
  for(uint8_t i=0;i<p.controlCount;++i)if(!control(items[i].as<JsonObject>(),p.controls[i]))return false;
  p.control=p.controls[0];
  return true;}
 case Section::Tasks:{
  auto& t=p.task;
  if(!keys(o,{"id","source","title","state","seq","updated_at","summary","viewed","has_details","session"})||!str(o,"id",t.id,sizeof(t.id),true)||!id(t.id)||!str(o,"source",t.source,sizeof(t.source),true)||!str(o,"title",t.title,sizeof(t.title),true)||!str(o,"state",t.state,sizeof(t.state),true)||!str(o,"summary",t.summary,sizeof(t.summary))||!u32(o,"updated_at",t.updatedAt)||!u32(o,"seq",t.seq,false)||!flag(o,"viewed",t.viewed)||!flag(o,"has_details",t.hasDetails)||!flag(o,"session",t.session))return false;
  const char* states[]={"running","waiting_input","success","failed","stale","round_done","round_stopped"};for(auto state:states)if(strcmp(state,t.state)==0)return true;return false;}
 case Section::Usage:{
  auto& u=p.usage;
  if(!keys(o,{"provider","limit_id","window_minutes","used_percent","resets_at","observed_at","state"})||!str(o,"provider",u.provider,sizeof(u.provider),true)||!str(o,"limit_id",u.limitId,sizeof(u.limitId),true)||!str(o,"state",u.state,sizeof(u.state),true)||!u32(o,"observed_at",u.observedAt))return false;
  if(strcmp(u.state,"fresh")&&strcmp(u.state,"stale")&&strcmp(u.state,"error")&&strcmp(u.state,"not_provided"))return false;
  u.hasWindow=!o["window_minutes"].isNull();u.hasReset=!o["resets_at"].isNull();u.hasUsed=!o["used_percent"].isNull();
  if(u.hasWindow&&(!u32(o,"window_minutes",u.windowMinutes)||!u.windowMinutes||u.windowMinutes>525600))return false;
  if(u.hasReset&&(!u32(o,"resets_at",u.resetsAt)||!u.resetsAt))return false;
  if(u.hasUsed){if(o["used_percent"].is<bool>()||!o["used_percent"].is<float>())return false;u.usedPercent=o["used_percent"].as<float>();if(!std::isfinite(u.usedPercent)||u.usedPercent<0||u.usedPercent>100)return false;}
  return strcmp(u.state,"fresh")!=0||(u.hasWindow&&u.hasUsed&&u.hasReset);}
 }
 return false;
}
bool parseResult(JsonObject root,Result& r){
 if(!keys(root,{"v","type","operation_id","state","reason"})||!str(root,"operation_id",r.operationId,sizeof(r.operationId),true)||!id(r.operationId)||!str(root,"state",r.state,sizeof(r.state),true)||!str(root,"reason",r.reason,sizeof(r.reason)))return false;
 return strcmp(r.state,"running")==0||strcmp(r.state,"succeeded")==0||strcmp(r.state,"failed")==0||strcmp(r.state,"unconfirmed")==0;
}
String encodeAction(const Action& a){
 StaticJsonDocument<384> doc;doc["v"]=1;doc["type"]="hub_action";doc["operation_id"]=a.operationId;doc["action"]=a.action;
 if(a.target[0])doc["target"]=a.target;
 if(a.hasValue){if(a.isBoolean)doc["value"]=a.booleanValue;else doc["value"]=a.value;}
 String result;serializeJson(doc,result);return result;
}
}
