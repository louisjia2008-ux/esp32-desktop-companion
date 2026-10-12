#include <cassert>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <vector>
#include <SD_MMC.h>
#include "storage/sd_wallpaper.h"

using namespace storage;

std::vector<uint8_t> package(const char* id, uint8_t content = 42,
                             uint32_t frames = 2, uint8_t fps = 10) {
  const std::vector<uint8_t> jpeg = {0xff, 0xd8, 0, content, 0, 0xff, 0xd9};
  std::vector<uint8_t> bytes(kCacheHeaderBytes + (4 + jpeg.size()) * frames);
  memcpy(bytes.data(), "DWC1", 4);
  bytes[4] = 1; bytes[6] = 40;
  bytes[8] = 0x40; bytes[9] = 1; bytes[10] = 0xe0; bytes[11] = 1;
  bytes[12] = fps;
  put32(bytes.data() + 16, frames); put32(bytes.data() + 20, bytes.size());
  put64(bytes.data() + 24, wallpaperKey(id));
  for (size_t i = 40; i < bytes.size(); i += 4 + jpeg.size()) {
    put32(bytes.data() + i, jpeg.size());
    memcpy(bytes.data() + i + 4, jpeg.data(), jpeg.size());
  }
  put32(bytes.data() + 32, cacheCrc32(0, bytes.data() + 40, bytes.size() - 40));
  put32(bytes.data() + 36, cacheCrc32(0, bytes.data(), 36));
  return bytes;
}

std::vector<uint8_t> chunk(const char* transfer, const std::vector<uint8_t>& bytes,
                           uint32_t offset = 0) {
  std::vector<uint8_t> packet(20 + bytes.size());
  memcpy(packet.data(), "DWS1", 4); put64(packet.data() + 4, wallpaperKey(transfer));
  put32(packet.data() + 12, offset);
  put32(packet.data() + 16, cacheCrc32(0, bytes.data(), bytes.size()));
  memcpy(packet.data() + 20, bytes.data(), bytes.size());
  return packet;
}

CacheAck pop(SdWallpaper& store) {
  CacheAck ack; assert(store.popAck(ack)); return ack;
}

void upload(SdWallpaper& store, const char* id, const char* transfer,
            const std::vector<uint8_t>& bytes) {
  store.beginUpload(transfer, id, bytes.size(), cacheCrc32(0, bytes.data(), bytes.size()), 1);
  assert(std::string(pop(store).state) == "ready");
  for (size_t offset=0; offset<bytes.size(); offset+=kCacheChunkBytes) {
    const size_t end=std::min(bytes.size(),offset+kCacheChunkBytes);
    auto packet=chunk(transfer,std::vector<uint8_t>(bytes.begin()+offset,bytes.begin()+end),offset);
    store.acceptChunk(packet.data(), packet.size(), 2);
    assert(pop(store).offset == end);
    store.acceptChunk(packet.data(), packet.size(), 3);
    assert(pop(store).offset == end);
  }
  store.finishUpload(transfer);
  assert(std::string(pop(store).state) == "verifying");
}

int main(int argc, char** argv) {
  assert(argc == 2); SD_MMC.root = argv[1];
  const char* first = "0123456789abcdef";
  const char* second = "fedcba9876543210";
  const char* token = "aaaaaaaaaaaaaaaa";
  const char* nextToken = "bbbbbbbbbbbbbbbb";
  auto bytes = package(first);
  assert(cacheCrc32(0, reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xcbf43926);
  CacheHeader header; assert(decodeCacheHeader(bytes.data(), 40, header));
  auto malformed = bytes; malformed[8] ^= 1;
  assert(!decodeCacheHeader(malformed.data(), 40, header));
  std::cout << "PASS shared format CRC and malformed header\n";
  {
    SdWallpaper store; store.begin(); assert(store.available() && !SD_MMC.formatted);
    upload(store, first, token, bytes);
    store.tick(4); store.tick(5); assert(store.candidateAvailable() && !store.ready());
    assert(!SD_MMC.exists("/companion-wallpaper/active0.bin"));
    SdWallpaperFrame frame; assert(store.readCandidate(frame));
    assert(store.acceptCandidate(5)); assert(store.ready());
    assert(std::string(pop(store).state) == "complete");
    assert(store.nextFrame(105, false, frame)); assert(frame.sequence == 1);
    assert(store.nextFrame(205, false, frame)); assert(frame.sequence == 0);
    store.beginUpload(nextToken, first, bytes.size(), cacheCrc32(0, bytes.data(), bytes.size()), 6);
    assert(std::string(pop(store).state) == "cached");
    std::cout << "PASS upload, repeated chunk, commit, loop and cached reuse\n";

    const auto newBytes = package(second, 43);
    upload(store, second, token, newBytes);
    corruptStagingReads = true; store.tick(7); store.tick(8); corruptStagingReads = false;
    assert(std::string(pop(store).state) == "error");
    assert(store.ready() && std::string(store.activeId()) == first);
    assert(SD_MMC.exists("/companion-wallpaper/wallpaper0.dwc"));
    std::cout << "PASS corrupt readback retains current wallpaper\n";

    upload(store, second, token, newBytes); store.tick(9); store.tick(10);
    assert(store.readCandidate(frame));
    failRecordWrites = true; assert(!store.acceptCandidate(11)); failRecordWrites = false;
    assert(std::string(pop(store).state) == "error");
    assert(store.ready() && std::string(store.activeId()) == first);
    std::cout << "PASS journal write failure retains previous active record\n";

    store.beginUpload(token, second, newBytes.size(), cacheCrc32(0, newBytes.data(), newBytes.size()), 12);
    assert(std::string(pop(store).state) == "ready");
    store.beginUpload(nextToken, first, bytes.size(), cacheCrc32(0, bytes.data(), bytes.size()), 13);
    auto busy = pop(store); assert(std::string(busy.transferId) == nextToken && std::string(busy.reason) == "busy");
    store.tick(16013); assert(std::string(pop(store).state) == "error");
    assert(store.ready() && std::string(store.activeId()) == first);
    std::cout << "PASS bounded concurrent begin rejection and upload timeout\n";
  }
  {
    SdWallpaper store; store.begin(); store.tick(1); store.tick(2);
    assert(store.ready() && std::string(store.activeId()) == first);
    auto newBytes = package(second, 44); upload(store, second, token, newBytes);
    store.tick(3); store.tick(4);
    SdWallpaperFrame frame; assert(store.readCandidate(frame)); assert(store.acceptCandidate(5));
    pop(store);
    assert(!SD_MMC.exists("/companion-wallpaper/wallpaper0.dwc"));
    assert(SD_MMC.exists("/companion-wallpaper/wallpaper1.dwc"));
    std::cout << "PASS reboot load and old file deleted only after replacement commits\n";
  }
  {
    SdWallpaper store; store.begin(); store.tick(1); store.tick(2);
    assert(store.ready() && std::string(store.activeId()) == second);
    SdWallpaperFrame frame; assert(!store.nextFrame(3, true, frame));
    assert(store.nextFrame(4, false, frame));
    store.clear(token, first); assert(std::string(pop(store).state) == "error" && store.ready());
    store.clear(token, second); assert(std::string(pop(store).state) == "cleared" && !store.ready());
    std::cout << "PASS overlay pause/resume and targeted cache clearing\n";
  }
  {
    auto longBytes=package(first,45,7200,12);
    CacheHeader decoded; assert(decodeCacheHeader(longBytes.data(),40,decoded));
    auto invalid=longBytes; put32(invalid.data()+16,7201);
    put32(invalid.data()+36,cacheCrc32(0,invalid.data(),36));
    assert(!decodeCacheHeader(invalid.data(),40,decoded));
    for(uint8_t fps:{6,8,10}) {
      auto boundary=package(first,45,uint32_t(fps)*600,fps);
      assert(decodeCacheHeader(boundary.data(),40,decoded));
      put32(boundary.data()+16,uint32_t(fps)*600+1);
      put32(boundary.data()+36,cacheCrc32(0,boundary.data(),36));
      assert(!decodeCacheHeader(boundary.data(),40,decoded));
    }
    SdWallpaper store; store.begin();
    upload(store,first,token,longBytes);
    for(uint32_t i=0;i<7200;++i)store.tick(4+i);
    assert(store.candidateAvailable());
    SdWallpaperFrame frame; assert(store.readCandidate(frame)); assert(frame.sequence==0);
    assert(store.acceptCandidate(10000)); assert(std::string(pop(store).state)=="complete");
    for(uint32_t i=1;i<7200;++i) {
      assert(store.nextFrame(10000+83*i,false,frame)); assert(frame.sequence==i);
    }
    assert(store.nextFrame(10000+83*7200,false,frame)); assert(frame.sequence==0);
    std::cout << "PASS ten-minute per-FPS bounds, chunked 7200-frame commit and full playback loop\n";
  }
  std::cout << "8/8 SD storage tests passed\n";
}
