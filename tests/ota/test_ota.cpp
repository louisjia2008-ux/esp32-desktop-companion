#include "storage/ota_receiver.h"
#include "storage/wallpaper_format.h"
#include <Update.h>
#include <esp_ota_ops.h>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
using storage::OtaReceiver;
static const char* id="0123456789abcdef";
std::string digest(const std::vector<uint8_t>& data) {
  unsigned char sha[32];CC_SHA256(data.data(),data.size(),sha);char hex[65];
  for(size_t i=0;i<32;++i)snprintf(hex+2*i,3,"%02x",sha[i]);return hex;
}
std::vector<uint8_t> image() {
  std::vector<uint8_t> data(900,0x31);data[0]=0xe9;data[12]=9;data[13]=0;
  storage::put32(data.data()+32,0xabcd5432);return data;
}
std::vector<uint8_t> chunk(const std::vector<uint8_t>& data,uint32_t offset,size_t size) {
  std::vector<uint8_t> out(24+size);memcpy(out.data(),"DWO1",4);
  uint64_t key=strtoull(id,nullptr,16);for(int i=0;i<8;++i)out[4+i]=key>>(8*i);
  storage::put32(out.data()+12,offset);storage::put32(out.data()+16,size);
  memcpy(out.data()+24,data.data()+offset,size);
  storage::put32(out.data()+20,storage::cacheCrc32(0,out.data()+24,size));return out;
}
void reset(){Update=FakeUpdate{};testPartition.size=0x640000;testBootState=2;}
int main(){
  const auto data=image();const auto sha=digest(data);const auto first=chunk(data,0,500),last=chunk(data,500,400);
  { reset();OtaReceiver receiver;assert(receiver.offer(id,"0.7.1",data.size(),sha.c_str(),1));
    receiver.acceptChunk(first.data(),first.size(),2);assert(Update.begins==0&&Update.writes==0);
    assert(receiver.approve(3));receiver.acceptChunk(first.data(),first.size(),4);
    receiver.acceptChunk(first.data(),first.size(),5);assert(Update.writes==1);
    receiver.acceptChunk(last.data(),last.size(),6);receiver.commit(id,7);
    assert(receiver.state()==OtaReceiver::State::Complete&&Update.ends==1&&Update.data==data);
    receiver.commit(id,8);receiver.disconnected();assert(Update.ends==1&&Update.aborts==0);
    assert(!receiver.rebootDue(100)&&receiver.rebootDue(1507)); }
  for(int fault=0;fault<9;++fault){
    reset();OtaReceiver receiver;std::string expected=fault==0?std::string(64,'0'):sha;
    assert(receiver.offer(id,"0.7.1",data.size(),expected.c_str(),1));
    if(fault==8){receiver.tick(120001);assert(Update.begins==0);continue;}
    assert(receiver.approve(2));
    if(fault==1){auto bad=first;bad.back()^=1;receiver.acceptChunk(bad.data(),bad.size(),3);}
    else if(fault==2){receiver.acceptChunk(last.data(),last.size(),3);}
    else if(fault==3){receiver.acceptChunk(first.data(),first.size(),3);receiver.disconnected();}
    else if(fault==4){receiver.tick(30002);}
    else if(fault==5){receiver.commit(id,3);}
    else if(fault==6){Update.failWrite=true;receiver.acceptChunk(first.data(),first.size(),3);}
    else if(fault==7){receiver.cancel();}
    else {receiver.acceptChunk(first.data(),first.size(),3);receiver.acceptChunk(last.data(),last.size(),4);receiver.commit(id,5);}
    assert(receiver.state()==OtaReceiver::State::Failed&&Update.ends==0&&Update.aborts==1);
  }
  { reset();testBootState=ESP_OTA_IMG_PENDING_VERIFY;OtaReceiver r;
    assert(r.offer(id,"0.7.1",data.size(),sha.c_str(),1));assert(!r.approve(2));assert(Update.begins==0); }
  { reset();testPartition.size=512;OtaReceiver r;
    assert(r.offer(id,"0.7.1",data.size(),sha.c_str(),1));assert(!r.approve(2));assert(Update.begins==0); }
  { reset();OtaReceiver r;assert(r.offer(id,"0.7.1",data.size(),sha.c_str(),1));assert(r.approve(2));
    auto wrong=first;wrong[4]^=1;r.acceptChunk(wrong.data(),wrong.size(),3);
    assert(Update.writes==0&&r.state()==OtaReceiver::State::Receiving);r.cancel(); }
  puts("PASS OTA receiver: physical consent, retry, CRC, SHA, offsets, disconnect, timeout, rollback guard, partition bounds");
}
