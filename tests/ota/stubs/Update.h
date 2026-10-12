#pragma once
#include <vector>
struct FakeUpdate {
  bool running=false,failBegin=false,failWrite=false,failEnd=false;
  unsigned begins=0,ends=0,aborts=0,writes=0;
  size_t expected=0;
  std::vector<uint8_t> data;
  bool begin(size_t n) { ++begins;expected=n;data.clear();return running=!failBegin; }
  bool isRunning() { return running; }
  void abort() { ++aborts;running=false; }
  size_t write(uint8_t* p,size_t n) { ++writes;if(failWrite)return 0;data.insert(data.end(),p,p+n);return n; }
  bool end(bool evenIfRemaining) { ++ends;running=false;return !failEnd&&!evenIfRemaining&&data.size()==expected; }
};
inline FakeUpdate Update;
