#include "logging/runtime_log.h"
#include <cassert>
#include <cstdio>
SerialStub Serial;
int main() {
  RuntimeLog log;
  log.printf("sample=%u\n", 123u);
  assert(Serial.output == "sample=123\n" && Serial.writes == 1);
  Serial.room = 0;
  log.println("monitor closed");
  assert(Serial.writes == 1 && log.droppedLines() == 1);
  Serial.room = 3;
  log.println("congestion");
  assert(Serial.writes == 1 && log.droppedLines() == 2);
  Serial.room = 512;
  log.printf("%0600u", 1u);
  assert(Serial.writes == 1 && log.droppedLines() == 3);
  Serial.shortWrite = true;
  log.println("short");
  assert(Serial.writes == 2 && log.droppedLines() == 4);
  Serial.shortWrite = false;
  log.println("recovered");
  assert(Serial.output.find("recovered\n") != std::string::npos);
  puts("runtime log: disconnected, congestion, format bounds, partial write and recovery PASS");
}
