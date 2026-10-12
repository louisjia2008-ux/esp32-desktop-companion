#include "hub_model.h"
#include <algorithm>
#include <cstring>
namespace hub {
bool Model::accept(const Page& p,uint32_t nowMs){
 const auto s=static_cast<size_t>(p.section);const size_t capacities[]={kControls,kCards,kTasks,kUsage};
 if(s>=4||!p.revision||p.total>capacities[s]||!p.pages||p.page>=p.pages||p.hasItem!=(p.total>0))return false;
 const uint8_t singlePages=p.total?p.total:1;
 const bool batch=p.section==Section::Control&&p.pages!=singlePages;
 const uint8_t stride=batch?Page::kControlBatch:1;
 const uint8_t expectedPages=p.total?(p.total+stride-1)/stride:1;
 if(p.pages!=expectedPages)return false;
 const size_t first=size_t(p.page)*stride;
 const uint8_t items=p.total?std::min<size_t>(stride,p.total-first):0;
 if(p.section==Section::Control){
  if(p.controlCount>Page::kControlBatch||
     (p.controlCount?p.controlCount!=items:batch))return false;
 }
 if(static_cast<int32_t>(p.revision-revision_[s])<=0)return false;
 if(pendingRevision_[s]&&static_cast<int32_t>(p.revision-pendingRevision_[s])<0)return false;
 if(pendingRevision_[s]!=p.revision){pendingRevision_[s]=p.revision;pendingCount_[s]=p.total;pendingPages_[s]=p.pages;mask_[s]=0;startedMs_[s]=nowMs;}
 if(pendingCount_[s]!=p.total||pendingPages_[s]!=p.pages)return false;
 // Legacy control lists can have 64 frames. At six frames per second their
 // receipt alone exceeds ten seconds; retain a bounded longer staging window.
 const uint32_t timeout=p.section==Section::Control?30000:10000;
 if(nowMs-startedMs_[s]>timeout){mask_[s]=0;startedMs_[s]=nowMs;}
 if(mask_[s]&(uint64_t(1)<<p.page))return false;
 if(p.hasItem){
  switch(p.section){
   case Section::Control:
    if(p.controlCount)for(uint8_t i=0;i<items;++i)pendingControls_[first+i]=p.controls[i];
    else pendingControls_[first]=p.control;
    break;
   case Section::Cards:pendingCards_[p.page]=p.card;break;
   case Section::Tasks:pendingTasks_[p.page]=p.task;break;
   case Section::Usage:pendingUsage_[p.page]=p.usage;break;
  }
 }
 mask_[s]|=uint64_t(1)<<p.page;
 const uint64_t fullMask=p.pages==64?UINT64_MAX:((uint64_t(1)<<p.pages)-1);
 if(mask_[s]!=fullMask)return false;
 switch(p.section){
  case Section::Control:controls_=pendingControls_;break;
  case Section::Cards:cards_=pendingCards_;break;
  case Section::Tasks:tasks_=pendingTasks_;break;
  case Section::Usage:usage_=pendingUsage_;break;
 }
 count_[s]=p.total;revision_[s]=p.revision;pendingRevision_[s]=0;mask_[s]=0;
 if(p.observedAt>=epoch(nowMs)){epoch_=p.observedAt;epochMs_=nowMs;}
 return true;
}
void Model::disconnected(){for(size_t s=0;s<4;++s){pendingRevision_[s]=0;mask_[s]=0;}}
uint32_t Model::epoch(uint32_t nowMs)const{return epoch_?epoch_+(nowMs-epochMs_)/1000U:0;}
bool Model::cardAvailable(size_t index,uint32_t nowMs)const{
 return index<count(Section::Cards)&&(cards_[index].pinned||!epoch_||cards_[index].expiresAt>epoch(nowMs));
}
void Model::expire(uint32_t nowMs){
 const uint32_t now=epoch(nowMs);
 for(size_t i=0;i<count(Section::Usage);++i){auto& u=usage_[i];if(strcmp(u.state,"fresh")==0&&now>u.observedAt&&now-u.observedAt>600)strcpy(u.state,"stale");}
 for(size_t i=0;i<count(Section::Tasks);++i){auto& t=tasks_[i];if(!t.session&&(strcmp(t.state,"running")==0||strcmp(t.state,"waiting_input")==0)&&now>t.updatedAt&&now-t.updatedAt>=45)strcpy(t.state,"stale");}
}
}
