#include "protocol.h"

#include <ArduinoJson.h>

#include <cstring>
#include <initializer_list>
#include <new>

#include "board_config.h"
#include "hub/hub_protocol.h"

namespace {

struct PacketReader {
  const uint8_t* data;
  size_t length;
  size_t position = 0;
  PacketReader(const uint8_t* bytes, size_t count) : data(bytes), length(count) {}
  int read() { return position < length ? data[position++] : -1; }
  size_t readBytes(char* destination, size_t count) {
    const size_t available = length - position;
    if (count > available) count = available;
    memcpy(destination, data + position, count);
    position += count;
    return count;
  }
  bool onlyTrailingWhitespace() const {
    for (size_t i = position; i < length; ++i) {
      if (data[i] != ' ' && data[i] != '\n' && data[i] != '\r' && data[i] != '\t') return false;
    }
    return true;
  }
};

bool noEmbeddedNulls(const JsonObject& object) {
  for (JsonPair field : object) {
    if (field.key().size() != strlen(field.key().c_str())) return false;
    if (field.value().is<const char*>()) {
      const JsonString value = field.value().as<JsonString>();
      if (value.size() != strlen(value.c_str())) return false;
    }
  }
  return true;
}

bool validText(const char* value, size_t maximumLength, bool allowEmpty) {
  if (value == nullptr) {
    return allowEmpty;
  }
  const size_t length = strnlen(value, maximumLength + 1);
  if (length > maximumLength || (!allowEmpty && length == 0)) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    const uint8_t character = static_cast<uint8_t>(value[index]);
    if (character < 0x20U && character != '\t') {
      return false;
    }
  }
  return true;
}

bool validId(const char* value, size_t maximumLength) {
  if (!validText(value, maximumLength, false)) return false;
  for (const char* p = value; *p; ++p) {
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
          *p == '.' || *p == ':')) return false;
  }
  return true;
}

bool onlyKeys(const JsonObject& object, std::initializer_list<const char*> allowed) {
  for (JsonPair field : object) {
    bool found = false;
    for (const char* key : allowed) {
      if (strcmp(field.key().c_str(), key) == 0) { found = true; break; }
    }
    if (!found) return false;
  }
  return true;
}

bool validWallpaperId(const char* value) {
  if (value == nullptr || strlen(value) != 16) {
    return false;
  }
  for (size_t index = 0; index < 16; ++index) {
    const char character = value[index];
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool validSessionId(const char* value) {
  return value != nullptr && strlen(value) == 8 &&
         strspn(value, "0123456789abcdef") == 8;
}

void copyText(char* destination, size_t capacity, const char* source) {
  if (source == nullptr) {
    destination[0] = '\0';
    return;
  }
  strlcpy(destination, source, capacity);
}

bool readOptionalUnsigned(const JsonObject& root, const char* name,
                          uint32_t maximum, bool& present,
                          uint32_t& value) {
  present = false;
  value = 0;
  if (!root.containsKey(name)) {
    return true;
  }
  const JsonVariant field = root[name];
  if (field.is<bool>() || !field.is<uint32_t>()) {
    return false;
  }
  const uint32_t parsed = field.as<uint32_t>();
  if (parsed > maximum) {
    return false;
  }
  present = true;
  value = parsed;
  return true;
}

bool validHostStatusKey(const char* key) {
  return strcmp(key, "v") == 0 || strcmp(key, "type") == 0 ||
         strcmp(key, "cpu_percent") == 0 ||
         strcmp(key, "memory_percent") == 0 ||
         strcmp(key, "memory_used_mb") == 0 ||
         strcmp(key, "memory_total_mb") == 0 ||
         strcmp(key, "uptime_seconds") == 0;
}

}  // namespace

namespace protocol {

bool parseIncoming(const uint8_t* payload, size_t length,
                   IncomingMessage& message, const char*& rejectionReason) {
  rejectionReason = "malformed JSON";
  // Reset in place: a temporary IncomingMessage would otherwise land on
  // the loop task's stack inside the deep WebSocket receive callback.
  new (&message) IncomingMessage();
  if (payload == nullptr || length == 0 || length > kMaximumPacketBytes) {
    rejectionReason = "packet size";
    return false;
  }

  // Static, not on the stack: parsing only ever runs on the loop task, and
  // deserializeJson() resets the document on every call.
  // The JSON arena includes object slots and copied strings, not just wire
  // bytes. Four bounded control rows need more space than their 2048-byte frame.
  static StaticJsonDocument<4096> document;
  PacketReader reader(payload, length);
  const DeserializationError error = deserializeJson(document, reader);
  if (error || !document.is<JsonObject>() || !reader.onlyTrailingWhitespace()) {
    return false;
  }

  const JsonObject root = document.as<JsonObject>();
  if (!noEmbeddedNulls(root)) { rejectionReason = "embedded null"; return false; }
  if (root["v"].is<bool>() || !root["v"].is<uint8_t>() || root["v"].as<uint8_t>() != kVersion) {
    rejectionReason = "protocol version";
    return false;
  }

  const char* type = root["type"];
  if (!validText(type, 24, false)) {
    rejectionReason = "message type";
    return false;
  }

  if (strcmp(type,"hub_snapshot")==0) {
    message.type=IncomingType::HUB_SNAPSHOT; rejectionReason="hub snapshot fields";
    return hub::parsePage(root,message.hubPage);
  }
  if (strcmp(type,"hub_result")==0) {
    message.type=IncomingType::HUB_RESULT; rejectionReason="hub result fields";
    return hub::parseResult(root,message.hubResult);
  }
  if (strcmp(type, "status") == 0) {
    const char* state = root["state"];
    const char* title = root["title"] | "";
    const char* detail = root["message"] | "";
    if (!validText(state, 24, false) || !validText(title, 48, true) ||
        !validText(detail, 128, true)) {
      rejectionReason = "status fields";
      return false;
    }
    message.type = IncomingType::STATUS;
    copyText(message.state, sizeof(message.state), state);
    copyText(message.title, sizeof(message.title), title);
    copyText(message.message, sizeof(message.message), detail);
    return true;
  }

  if (strcmp(type, "notification") == 0) {
    const char* id = root["event_id"];
    const char* level = root["level"];
    const char* source = root["source"];
    const char* title = root["title"];
    const char* detail = root["message"];
    if (!onlyKeys(root, {"v", "type", "event_id", "level", "source", "title", "message", "auto_dismiss_ms"}) ||
        !validId(id, 48) || !validText(level, 16, false) ||
        !validText(source, 32, false) || !validText(title, 48, false) || !validText(detail, 128, false)) {
      rejectionReason = "notification fields";
      return false;
    }
    if (strcmp(level, "info") == 0) message.event.type = EventType::Info;
    else if (strcmp(level, "success") == 0) message.event.type = EventType::Success;
    else if (strcmp(level, "warning") == 0) message.event.type = EventType::Warning;
    else if (strcmp(level, "error") == 0) message.event.type = EventType::Error;
    else { rejectionReason = "unsupported notification level"; return false; }
    uint32_t duration = 0;
    bool hasDuration = false;
    if (!readOptionalUnsigned(root, "auto_dismiss_ms", 600000, hasDuration, duration) ||
        (hasDuration && duration > 0 && duration < 1000)) {
      rejectionReason = "notification duration";
      return false;
    }
    message.event.autoDismissMs = hasDuration ? duration :
        (message.event.type <= EventType::Success ? 4000 : 0);
    copyText(message.event.id, sizeof(message.event.id), id);
    copyText(message.event.source, sizeof(message.event.source), source);
    copyText(message.event.title, sizeof(message.event.title), title);
    copyText(message.event.message, sizeof(message.event.message), detail);
    message.type = IncomingType::NOTIFICATION;
    copyText(message.title, sizeof(message.title), title);
    copyText(message.message, sizeof(message.message), detail);
    return true;
  }

  if (strcmp(type, "action_required") == 0) {
    const char* requestId = root["request_id"];
    const char* title = root["title"];
    const char* detail = root["message"];
    const char* source = root["source"];
    const JsonArray actions = root["actions"].as<JsonArray>();
    if (!onlyKeys(root, {"v", "type", "request_id", "source", "title", "message", "actions"}) ||
        !validId(requestId, 48) || !validText(source, 32, false) || !validText(title, 48, false) ||
        !validText(detail, 128, false) || actions.isNull() ||
        actions.size() == 0 || actions.size() > message.actions.size()) {
      rejectionReason = "action request fields";
      return false;
    }

    message.type = IncomingType::ACTION_REQUIRED;
    copyText(message.requestId, sizeof(message.requestId), requestId);
    copyText(message.title, sizeof(message.title), title);
    copyText(message.message, sizeof(message.message), detail);
    size_t index = 0;
    for (JsonVariant actionValue : actions) {
      if (!actionValue.is<JsonObject>()) {
        rejectionReason = "action object";
        return false;
      }
      const JsonObject action = actionValue.as<JsonObject>();
      if (!noEmbeddedNulls(action)) { rejectionReason = "embedded action null"; return false; }
      const char* id = action["id"];
      const char* label = action["label"];
      if (!onlyKeys(action, {"id", "label", "hold_ms"}) || !validId(id, 32) || !validText(label, 48, false)) {
        rejectionReason = "action fields";
        return false;
      }
      uint32_t holdMs = 0;
      bool hasHold = false;
      if (!readOptionalUnsigned(action, "hold_ms", 2000, hasHold, holdMs) ||
          (hasHold && holdMs < 300)) {
        rejectionReason = "action hold duration";
        return false;
      }
      message.actions[index].holdMs = static_cast<uint16_t>(holdMs);
      for (size_t prior = 0; prior < index; ++prior) {
        if (strcmp(message.actions[prior].id, id) == 0) {
          rejectionReason = "duplicate action id";
          return false;
        }
      }
      copyText(message.actions[index].id, sizeof(message.actions[index].id),
               id);
      copyText(message.actions[index].label,
               sizeof(message.actions[index].label), label);
      ++index;
    }
    message.actionCount = index;
    message.event.type = EventType::ActionRequired;
    copyText(message.event.id, sizeof(message.event.id), requestId);
    copyText(message.event.source, sizeof(message.event.source), source);
    copyText(message.event.title, sizeof(message.event.title), title);
    copyText(message.event.message, sizeof(message.event.message), detail);
    message.event.actions = message.actions;
    message.event.actionCount = index;
    return true;
  }

  if (strcmp(type, "action_ack") == 0) {
    const char* requestId = root["request_id"];
    const char* actionId = root["action_id"];
    if (!onlyKeys(root, {"v", "type", "request_id", "action_id", "accepted"}) ||
        !validId(requestId, 48) || !validId(actionId, 32) || !root["accepted"].is<bool>()) {
      rejectionReason = "action ack fields";
      return false;
    }
    message.type = IncomingType::ACTION_ACK;
    copyText(message.requestId, sizeof(message.requestId), requestId);
    copyText(message.actionId, sizeof(message.actionId), actionId);
    message.ackAccepted = root["accepted"].as<bool>();
    return true;
  }

  if (strcmp(type, "dismiss") == 0) {
    const char* requestId = root["request_id"];
    if (!validText(requestId, 48, false)) {
      rejectionReason = "dismiss request id";
      return false;
    }
    message.type = IncomingType::DISMISS;
    copyText(message.requestId, sizeof(message.requestId), requestId);
    return true;
  }

  if (strcmp(type, "wallpaper_sync") == 0) {
    const char* op = root["op"];
    const char* transferId = root["transfer_id"];
    const char* wallpaperId = root["wallpaper_id"];
    if (!validText(op, 8, false) || !validWallpaperId(transferId) ||
        !validWallpaperId(wallpaperId) ||
        (strcmp(op, "begin") && strcmp(op, "commit") &&
         strcmp(op, "abort") && strcmp(op, "clear"))) {
      rejectionReason = "wallpaper sync fields"; return false;
    }
    for (JsonPair field : root) {
      const char* key = field.key().c_str();
      if (strcmp(key, "v") && strcmp(key, "type") && strcmp(key, "op") &&
          strcmp(key, "transfer_id") && strcmp(key, "wallpaper_id") &&
          (strcmp(op, "begin") || (strcmp(key, "bytes") && strcmp(key, "crc32")))) {
        rejectionReason = "wallpaper sync key"; return false;
      }
    }
    if (strcmp(op, "begin") == 0) {
      if (!root["bytes"].is<uint32_t>() || root["bytes"].is<bool>() ||
          !root["crc32"].is<uint32_t>() || root["crc32"].is<bool>()) {
        rejectionReason = "wallpaper sync size"; return false;
      }
      message.cacheBytes = root["bytes"].as<uint32_t>();
      message.cacheCrc = root["crc32"].as<uint32_t>();
      if (message.cacheBytes <= 40 || message.cacheBytes > 128UL * 1024 * 1024) {
        rejectionReason = "wallpaper sync bounds"; return false;
      }
    }
    message.type = IncomingType::WALLPAPER_SYNC;
    copyText(message.transferId, sizeof(message.transferId), transferId);
    copyText(message.wallpaperId, sizeof(message.wallpaperId), wallpaperId);
    copyText(message.syncOperation, sizeof(message.syncOperation), op);
    return true;
  }

  if (strcmp(type, "video_wallpaper") == 0) {
    const char* wallpaperId = root["wallpaper_id"];
    const char* wallpaperName = root["name"] | "";
    if (!validWallpaperId(wallpaperId) ||
        !validText(wallpaperName, 48, true) ||
        !root["width"].is<uint16_t>() ||
        !root["height"].is<uint16_t>() ||
        !root["fps"].is<uint8_t>() ||
        !root["frame_count"].is<uint32_t>()) {
      rejectionReason = "video wallpaper fields";
      return false;
    }
    const uint16_t width = root["width"].as<uint16_t>();
    const uint16_t height = root["height"].as<uint16_t>();
    const uint8_t fps = root["fps"].as<uint8_t>();
    const uint32_t frameCount = root["frame_count"].as<uint32_t>();
    if (width != board::kPanelWidth || height != board::kPanelHeight ||
        fps < 4 || fps > 12 || frameCount == 0 || frameCount > 3600) {
      rejectionReason = "video wallpaper bounds";
      return false;
    }
    message.type = IncomingType::VIDEO_WALLPAPER;
    copyText(message.wallpaperId, sizeof(message.wallpaperId), wallpaperId);
    copyText(message.wallpaperName, sizeof(message.wallpaperName),
             wallpaperName);
    message.videoWidth = width;
    message.videoHeight = height;
    message.videoFps = fps;
    message.videoFrameCount = frameCount;
    return true;
  }

  if (strcmp(type, "ota_offer") == 0) {
    const char* id = root["transfer_id"], *version = root["version"], *build = root["build"];
    const char* sha = root["sha256"], *mac = root["mac"];
    if (!onlyKeys(root, {"v", "type", "transfer_id", "version", "build", "bytes", "sha256", "mac"}) ||
        !validWallpaperId(id) || !validId(version, 24) || !validText(build, 39, false) ||
        !sha || strlen(sha) != 64 || strspn(sha, "0123456789abcdef") != 64 ||
        !mac || strlen(mac) != 64 || strspn(mac, "0123456789abcdef") != 64 ||
        root["bytes"].is<bool>() || !root["bytes"].is<uint32_t>() ||
        root["bytes"].as<uint32_t>() < 288 || root["bytes"].as<uint32_t>() > 0x640000) {
      rejectionReason = "ota offer fields"; return false;
    }
    // Canonical HMAC fields must be ASCII and cannot contain separators.
    for (const char* p = build; *p; ++p) {
      if (*p < 32 || *p > 126) { rejectionReason = "ota build text"; return false; }
    }
    message.type = IncomingType::OTA_OFFER;
    copyText(message.transferId, sizeof(message.transferId), id);
    copyText(message.otaVersion, sizeof(message.otaVersion), version);
    copyText(message.otaBuild, sizeof(message.otaBuild), build);
    copyText(message.otaSha, sizeof(message.otaSha), sha);
    copyText(message.otaMac, sizeof(message.otaMac), mac);
    message.cacheBytes = root["bytes"].as<uint32_t>();
    return true;
  }
  if (strcmp(type, "ota_control") == 0) {
    const char* id = root["transfer_id"], *op = root["op"];
    if (!onlyKeys(root, {"v", "type", "transfer_id", "op"}) ||
        !validWallpaperId(id) || !op || (strcmp(op,"commit") && strcmp(op,"abort"))) {
      rejectionReason = "ota control fields"; return false;
    }
    message.type = IncomingType::OTA_CONTROL;
    copyText(message.transferId, sizeof(message.transferId), id);
    copyText(message.syncOperation, sizeof(message.syncOperation), op);
    return true;
  }
  if (strcmp(type, "clock") == 0) {
    const JsonVariant epoch = root["epoch_s"];
    const JsonVariant offset = root["utc_offset_s"];
    if (!onlyKeys(root, {"v", "type", "epoch_s", "utc_offset_s"}) ||
        epoch.is<bool>() || !epoch.is<uint32_t>() || offset.is<bool>() ||
        !offset.is<int32_t>() || offset.as<int32_t>() < -14 * 3600 ||
        offset.as<int32_t>() > 14 * 3600 || epoch.as<uint32_t>() < 1600000000UL) {
      rejectionReason = "clock fields";
      return false;
    }
    message.type = IncomingType::CLOCK;
    message.clockEpoch = epoch.as<uint32_t>();
    message.clockOffsetS = offset.as<int32_t>();
    return true;
  }

  if (strcmp(type, "display_settings") == 0) {
    if (!onlyKeys(root, {"v", "type", "night_start_min", "night_end_min", "day_level",
                         "night_level", "night_wake_level", "idle_dim_s", "idle_level"})) {
      rejectionReason = "display settings keys";
      return false;
    }
    DisplaySettings& s = message.displaySettings;
    bool present = false;
    uint32_t value = 0;
    struct Field { const char* name; uint32_t maximum; uint32_t* out; };
    uint32_t nightStart = 0, nightEnd = 0, day = 0, night = 0, wake = 0, idleS = 0, idle = 0;
    const Field fields[] = {{"night_start_min", 1439, &nightStart}, {"night_end_min", 1439, &nightEnd},
                            {"day_level", 100, &day}, {"night_level", 100, &night},
                            {"night_wake_level", 100, &wake}, {"idle_dim_s", 3600, &idleS},
                            {"idle_level", 100, &idle}};
    for (const Field& field : fields) {
      if (!readOptionalUnsigned(root, field.name, field.maximum, present, value) || !present) {
        rejectionReason = "display settings fields";
        return false;
      }
      *field.out = value;
    }
    s.nightStartMin = nightStart; s.nightEndMin = nightEnd;
    s.dayLevel = day; s.nightLevel = night; s.nightWakeLevel = wake;
    s.idleDimS = idleS; s.idleLevel = idle;
    if (!validDisplaySettings(s)) {
      rejectionReason = "display settings bounds";
      return false;
    }
    message.type = IncomingType::DISPLAY_SETTINGS;
    return true;
  }

  if (strcmp(type, "auth_challenge") == 0) {
    const char* nonce = root["nonce"];
    if (!onlyKeys(root, {"v", "type", "nonce"}) || nonce == nullptr ||
        strlen(nonce) != 32 || strspn(nonce, "0123456789abcdef") != 32) {
      rejectionReason = "auth challenge fields";
      return false;
    }
    message.type = IncomingType::AUTH_CHALLENGE;
    copyText(message.nonce, sizeof(message.nonce), nonce);
    return true;
  }

  if (strcmp(type, "agent_status") == 0) {
    const JsonArray sessions = root["sessions"].as<JsonArray>();
    if (!onlyKeys(root, {"v", "type", "sessions"}) || sessions.isNull() ||
        sessions.size() > message.agentStatus.sessions.size()) {
      rejectionReason = "agent status fields";
      return false;
    }
    size_t index = 0;
    for (JsonVariant value : sessions) {
      if (!value.is<JsonObject>()) { rejectionReason = "agent session object"; return false; }
      const JsonObject item = value.as<JsonObject>();
      AgentSession& session = message.agentStatus.sessions[index];
      const char* id = item["id"];
      const char* project = item["project"];
      const char* state = item["state"];
      const char* tool = item["tool"] | "";
      const char* source = item["source"] | "claude";
      if (!noEmbeddedNulls(item) ||
          !onlyKeys(item, {"id", "source", "project", "state", "tool", "elapsed_s", "age_s", "pending"}) ||
          !validSessionId(id) ||
          !validText(project, 32, false) || !validText(state, 8, false) ||
          !validText(tool, 32, true) || (strcmp(source, "claude") && strcmp(source, "codex")) ||
          (item.containsKey("source") && !item["source"].is<const char*>())) {
        rejectionReason = "agent session fields";
        return false;
      }
      if (strcmp(state, "working") == 0) session.state = AgentState::Working;
      else if (strcmp(state, "waiting") == 0) session.state = AgentState::Waiting;
      else if (strcmp(state, "done") == 0) session.state = AgentState::Done;
      else if (strcmp(state, "idle") == 0) session.state = AgentState::Idle;
      else if (strcmp(state, "stopped") == 0 && item.containsKey("source")) session.state = AgentState::Stopped;
      else { rejectionReason = "agent session state"; return false; }
      session.source = strcmp(source, "codex") == 0 ? AgentSource::Codex : AgentSource::Claude;
      bool present = false;
      uint32_t pending = 0;
      if (!readOptionalUnsigned(item, "elapsed_s", 0xFFFFFF, present, session.elapsedS) || !present ||
          !readOptionalUnsigned(item, "age_s", 0xFFFFFF, present, session.ageS) || !present ||
          !readOptionalUnsigned(item, "pending", 255, present, pending) || !present) {
        rejectionReason = "agent session counters";
        return false;
      }
      session.pending = static_cast<uint8_t>(pending);
      copyText(session.id, sizeof(session.id), id);
      copyText(session.project, sizeof(session.project), project);
      copyText(session.tool, sizeof(session.tool), tool);
      ++index;
    }
    message.agentStatus.count = index;
    message.type = IncomingType::AGENT_STATUS;
    return true;
  }

  if (strcmp(type, "host_status") == 0) {
    for (JsonPair field : root) {
      if (!validHostStatusKey(field.key().c_str())) {
        rejectionReason = "host status key";
        return false;
      }
    }
    uint32_t cpuPercent = 0;
    uint32_t memoryPercent = 0;
    if (!readOptionalUnsigned(root, "cpu_percent", 100,
                              message.hostStatus.hasCpuPercent, cpuPercent) ||
        !readOptionalUnsigned(root, "memory_percent", 100,
                              message.hostStatus.hasMemoryPercent,
                              memoryPercent) ||
        !readOptionalUnsigned(root, "memory_used_mb", UINT32_MAX,
                              message.hostStatus.hasMemoryUsedMb,
                              message.hostStatus.memoryUsedMb) ||
        !readOptionalUnsigned(root, "memory_total_mb", UINT32_MAX,
                              message.hostStatus.hasMemoryTotalMb,
                              message.hostStatus.memoryTotalMb) ||
        !readOptionalUnsigned(root, "uptime_seconds", UINT32_MAX,
                              message.hostStatus.hasUptimeSeconds,
                              message.hostStatus.uptimeSeconds)) {
      rejectionReason = "host status fields";
      return false;
    }
    const bool hasAny = message.hostStatus.hasCpuPercent ||
                        message.hostStatus.hasMemoryPercent ||
                        message.hostStatus.hasMemoryUsedMb ||
                        message.hostStatus.hasMemoryTotalMb ||
                        message.hostStatus.hasUptimeSeconds;
    bool memoryConsistent = true;
    if (message.hostStatus.hasMemoryPercent &&
        message.hostStatus.hasMemoryUsedMb &&
        message.hostStatus.hasMemoryTotalMb &&
        message.hostStatus.memoryTotalMb > 0) {
      const uint32_t calculatedPercent = static_cast<uint32_t>(
          (static_cast<uint64_t>(message.hostStatus.memoryUsedMb) * 100U +
           message.hostStatus.memoryTotalMb / 2U) /
          message.hostStatus.memoryTotalMb);
      const uint32_t difference =
          calculatedPercent > memoryPercent
              ? calculatedPercent - memoryPercent
              : memoryPercent - calculatedPercent;
      memoryConsistent = difference <= 1U;
    }
    if (!hasAny || !memoryConsistent ||
        (message.hostStatus.hasMemoryUsedMb &&
         message.hostStatus.hasMemoryTotalMb &&
         message.hostStatus.memoryUsedMb >
             message.hostStatus.memoryTotalMb) ||
        (message.hostStatus.hasMemoryTotalMb &&
         message.hostStatus.memoryTotalMb == 0)) {
      rejectionReason = "host status bounds";
      return false;
    }
    message.hostStatus.cpuPercent = static_cast<uint8_t>(cpuPercent);
    message.hostStatus.memoryPercent = static_cast<uint8_t>(memoryPercent);
    message.type = IncomingType::HOST_STATUS;
    return true;
  }

  rejectionReason = "unsupported message type";
  return false;
}

String makeHello(const char* firmwareVersion, bool sdAvailable, bool claudePaired, const char* otaNonce) {
  StaticJsonDocument<768> document;
  document["v"] = kVersion;
  document["type"] = "hello";
  document["device"] = "esp32-companion";
  document["width"] = board::kDisplayWidth;
  document["height"] = board::kDisplayHeight;
  document["firmware"] = firmwareVersion;
  JsonArray capabilities = document.createNestedArray("capabilities");
  capabilities.add("video_wallpaper");
  capabilities.add("host_status");
  capabilities.add("interactive_events");
  capabilities.add("events_v2");
  capabilities.add("agent_status");
  capabilities.add("agent_status_v2");
  capabilities.add("ambient");
  capabilities.add("desktop_hub_v1");
  capabilities.add("desktop_hub_batch_v1");
  if (claudePaired && otaNonce) {
    capabilities.add("ota");
    document["ota_nonce"] = otaNonce;
  }
  // Only a device holding the pairing key offers to answer Claude prompts.
  if (claudePaired) capabilities.add("claude_companion");
  if (sdAvailable) capabilities.add("sd_wallpaper");
  String output;
  output.reserve(192);
  serializeJson(document, output);
  return output;
}

String makeAuthResponse(const char* macHex) {
  StaticJsonDocument<160> document;
  document["v"] = kVersion;
  document["type"] = "auth_response";
  document["mac"] = macHex;
  String output;
  serializeJson(document, output);
  return output;
}

String makeHeartbeat(uint32_t uptimeMs) {
  StaticJsonDocument<128> document;
  document["v"] = kVersion;
  document["type"] = "heartbeat";
  document["uptime_ms"] = uptimeMs;
  String output;
  output.reserve(96);
  serializeJson(document, output);
  return output;
}

String makeAction(const char* requestId, const char* actionId) {
  StaticJsonDocument<192> document;
  document["v"] = kVersion;
  document["type"] = "action";
  document["request_id"] = requestId;
  document["action_id"] = actionId;
  String output;
  output.reserve(160);
  serializeJson(document, output);
  return output;
}

String makeWallpaperStatus(const char* wallpaperId, const char* state,
                           uint32_t sequence) {
  StaticJsonDocument<256> document;
  document["v"] = kVersion;
  document["type"] = "wallpaper_status";
  document["wallpaper_id"] = wallpaperId;
  document["state"] = state;
  document["sequence"] = sequence;
  String output;
  output.reserve(192);
  serializeJson(document, output);
  return output;
}

}  // namespace protocol
