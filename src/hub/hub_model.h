#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace hub {
enum class Section : uint8_t { Control, Cards, Tasks, Usage };
struct Card {
 char id[33]={},kind[9]={},title[49]={},body[513]={},url[513]={},fileRef[33]={};
 uint32_t createdAt=0,expiresAt=0;bool pinned=false;
};
struct Control {
 char kind[17]={},id[33]={},name[49]={},state[17]={},reason[129]={},settings[17]={};
 bool selected=false,connected=false,hasVolume=false,volumeSupported=false,muted=false,muteSupported=false;
 uint8_t volume=0;
};
struct Task {
 char id[33]={},source[33]={},title[49]={},state[17]={},summary[129]={};
 uint32_t updatedAt=0,seq=0;bool viewed=false,hasDetails=false,session=false;
};
struct Usage {
 char provider[33]={},limitId[49]={},state[17]={};
 uint32_t windowMinutes=0,resetsAt=0,observedAt=0;float usedPercent=0;
 bool hasWindow=false,hasUsed=false,hasReset=false;
};
struct Page {
 static constexpr uint8_t kControlBatch=4;
 Section section=Section::Control;uint32_t revision=0,observedAt=0;
 uint8_t page=0,pages=1,total=0;bool hasItem=false;
 Card card{};Control control{};Task task{};Usage usage{};
 // control remains the legacy one-row representation. Batched control pages
 // carry up to four rows; every non-final page has exactly four.
 std::array<Control,kControlBatch> controls{};uint8_t controlCount=0;
};
struct Result { char operationId[33]={},state[17]={},reason[129]={}; };
struct Action {
 char operationId[33]={},action[33]={},target[33]={};
 bool hasValue=false,booleanValue=false,isBoolean=false;uint8_t value=0;
};
class Model {
 public:
 static constexpr size_t kControls=64,kCards=5,kTasks=8,kUsage=16;
 bool accept(const Page& page,uint32_t nowMs);
 void disconnected();
 void expire(uint32_t nowMs);
 uint32_t epoch(uint32_t nowMs) const;
 const std::array<Control,kControls>& controls()const{return controls_;}
 const std::array<Card,kCards>& cards()const{return cards_;}
 const std::array<Task,kTasks>& tasks()const{return tasks_;}
 const std::array<Usage,kUsage>& usage()const{return usage_;}
 size_t count(Section section)const{return count_[static_cast<size_t>(section)];}
 uint32_t revision(Section section)const{return revision_[static_cast<size_t>(section)];}
 bool cardAvailable(size_t index,uint32_t nowMs)const;
 private:
 std::array<Control,kControls> controls_{},pendingControls_{};
 std::array<Card,kCards> cards_{},pendingCards_{};
 std::array<Task,kTasks> tasks_{},pendingTasks_{};
 std::array<Usage,kUsage> usage_{},pendingUsage_{};
 uint32_t revision_[4]={},pendingRevision_[4]={},startedMs_[4]={};
 uint64_t mask_[4]={};
 uint8_t count_[4]={},pendingCount_[4]={},pendingPages_[4]={};
 uint32_t epoch_=0,epochMs_=0;
};
}
