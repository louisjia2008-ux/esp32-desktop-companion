#include "ota_receiver.h"
#include "wallpaper_format.h"
#include <Update.h>
#include <esp_ota_ops.h>
#include <cstring>

namespace storage {
OtaReceiver::OtaReceiver() { mbedtls_sha256_init(&sha_); }
OtaReceiver::~OtaReceiver() { mbedtls_sha256_free(&sha_); }
void OtaReceiver::acknowledge(const char* state, const char* reason) {
  strlcpy(ack_.transferId, id_, sizeof(ack_.transferId));
  ack_.state = state; ack_.reason = reason; ack_.offset = offset_; ackPending_ = true;
}
bool OtaReceiver::popAck(OtaAck& ack) {
  if (!ackPending_) return false;
  ack = ack_; ackPending_ = false; return true;
}
bool OtaReceiver::offer(const char* id, const char* version, uint32_t size,
                        const char* sha, uint32_t now) {
  if (busy() || !id || strlen(id) != 16 || strspn(id,"0123456789abcdef") != 16 ||
      !version || strlen(version) > 24 || !sha || strlen(sha) != 64 ||
      strspn(sha,"0123456789abcdef") != 64 || size < 288 || size > 0x640000) return false;
  strlcpy(id_, id, sizeof(id_)); strlcpy(version_, version, sizeof(version_));
  strlcpy(expectedSha_, sha, sizeof(expectedSha_));
  key_ = strtoull(id_, nullptr, 16); expected_ = size; offset_ = 0;
  lastSize_ = 0; lastAt_ = now; state_ = State::Offered;
  return true;
}
bool OtaReceiver::approve(uint32_t now) {
  if (!offered()) return false;
  const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
  esp_ota_img_states_t bootState;
  if ((esp_ota_get_state_partition(esp_ota_get_running_partition(), &bootState) == ESP_OK &&
       bootState == ESP_OTA_IMG_PENDING_VERIFY) || !partition || expected_ > partition->size) {
    cancel("partition_unavailable"); return false;
  }
  if (!Update.begin(expected_) || mbedtls_sha256_starts_ret(&sha_, 0) != 0) {
    cancel("begin_failed"); return false;
  }
  state_ = State::Receiving; lastAt_ = now; acknowledge("ready"); return true;
}
void OtaReceiver::cancel(const char* reason) {
  if (state_ == State::Complete) return;  // boot partition already selected
  if (state_ == State::Receiving || Update.isRunning()) Update.abort();
  state_ = State::Failed;
  acknowledge(strcmp(reason,"cancelled") == 0 || strcmp(reason,"approval_timeout") == 0 ? "cancelled" : "error", reason);
}
void OtaReceiver::disconnected() {
  if (offered() || state_ == State::Receiving) cancel("disconnected");
}
void OtaReceiver::abort(const char* id) {
  if (id && strcmp(id, id_) == 0 && busy()) cancel();
}
void OtaReceiver::acceptChunk(const uint8_t* data, size_t size, uint32_t now) {
  if (state_ != State::Receiving) return;
  if (!data || size <= 24 || size > 24 + 12 * 1024 || memcmp(data,"DWO1",4) != 0) {
    cancel("chunk_header"); return;
  }
  if (little64(data + 4) != key_) return;  // unrelated transfer cannot write
  const uint32_t offset = little32(data+12), count = little32(data+16), crc = little32(data+20);
  if (count != size - 24 || cacheCrc32(0,data+24,count) != crc) {
    cancel("chunk_crc"); return;
  }
  if (lastSize_ && offset == lastOffset_ && count == lastSize_ && crc == lastCrc_) {
    lastAt_ = now; acknowledge("progress"); return; // lost ACK retry
  }
  if (offset != offset_ || count > expected_ - offset_) { cancel("chunk_offset"); return; }
  if (offset_ == 0 && (count < 288 || data[24] != 0xe9 || little16(data+24+12) != 9 ||
      little32(data+24+32) != 0xabcd5432)) { cancel("image_header"); return; }
  if (Update.write(const_cast<uint8_t*>(data+24),count) != count ||
      mbedtls_sha256_update_ret(&sha_,data+24,count) != 0) { cancel("write_failed"); return; }
  lastOffset_ = offset; lastSize_ = count; lastCrc_ = crc;
  offset_ += count; lastAt_ = now; acknowledge("progress");
}
void OtaReceiver::commit(const char* id, uint32_t now) {
  if (!id || strcmp(id,id_) != 0) return;
  if (state_ == State::Complete) { acknowledge("complete"); return; }
  if (state_ != State::Receiving) return;
  if (offset_ != expected_) { cancel("incomplete"); return; }
  uint8_t digest[32]; char hex[65];
  if (mbedtls_sha256_finish_ret(&sha_,digest) != 0) { cancel("sha_failed"); return; }
  for (size_t i=0;i<32;++i) snprintf(hex+i*2,3,"%02x",digest[i]);
  if (strcmp(hex,expectedSha_) != 0) { cancel("sha_mismatch"); return; }
  // end(false) requires the exact advertised byte count and validates the
  // ESP image before selecting the other application slot for next boot.
  if (!Update.end(false)) { cancel("image_invalid"); return; }
  state_ = State::Complete; completedAt_ = now; acknowledge("complete");
}
void OtaReceiver::tick(uint32_t now) {
  if (offered() && now-lastAt_ >= 120000) cancel("approval_timeout");
  if (state_ == State::Receiving && now-lastAt_ >= 30000) cancel("transfer_timeout");
}
}  // namespace storage
