#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <esp_system.h>
#include <stddef.h>
#include <initializer_list>
#include <stdlib.h>
#include <string.h>
#include <new>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include "Config334.h"
#include "Control334.h"
#include "Audio342.h"
#include "WebUi342.h"

using namespace rr334;
using namespace rr334cfg;

namespace {
WebServer server(kHttpPort);
Preferences prefs;

// Stable, explicit NVS wire format; never persist the compiler's padded C++ structs.
#pragma pack(push, 1)
struct StoredState {
  uint32_t magic;
  uint16_t schema;
  uint16_t length;
  uint32_t revision;
  uint8_t mode;
  uint8_t credit;
  uint8_t flags; // bit0 positionValid, bit1 rewardInProgress
  uint8_t reserved;
  int32_t positionSteps;
  int32_t pickupSteps;
  int32_t dropoffSteps;
  uint32_t stepIntervalUs;
  uint32_t deliveryDwellMs;
  uint32_t pickupDwellMs;
  uint32_t seekLimitSteps;
  uint32_t seekTimeoutMs;
  uint32_t crc;
};
#pragma pack(pop)
static_assert(sizeof(StoredState) == 52, "StoredState schema size changed");
static constexpr uint32_t kRecordMagic = 0x52523334u; // RR34
static constexpr uint16_t kRecordSchema = 1;

Debouncer hallDebounce, buttonDebounce;
String wifiSsid, wifiPassword, otaPassword;
bool prefsOpen = false, networkReady = false, mdnsReady = false;
bool httpStarted = false;
bool wifiProvisioned = false, otaServiceStarted = false;
volatile bool otaInProgress = false;
bool coilsOn = false, startupSeekActive = false;
SemaphoreHandle_t controlMutex = nullptr;
TaskHandle_t controlTaskHandle = nullptr;
QueueHandle_t stopQueue = nullptr;
volatile bool buttonReleasedReady = false;
portMUX_TYPE eventMux = portMUX_INITIALIZER_UNLOCKED;
bool audioRewardObserved = false, audioDropoffArrived = false;
uint32_t audioRewardCycleId = 0, audioDropoffArrivalMs = 0;
bool serialOverflow = false;
char serialLine[160];
size_t serialLength = 0;
uint8_t phaseIndex = 0;
volatile uint32_t otaWindowStartedMs = 0;
volatile uint32_t otaWindowUntilMs = 0;
uint32_t lastWiFiAttemptMs = 0;
uint32_t lastSerialStatusMs = 0;
char lastEvent[48] = "Power-up check complete; no automatic motion";
static constexpr uint8_t kHalfStepPatterns[8] = {
    0b1000, 0b1100, 0b0100, 0b0110,
    0b0010, 0b0011, 0b0001, 0b1001};
struct StopRequest { uint32_t nonce; TaskHandle_t waiter; Result result; };

uint32_t crc32(const uint8_t* bytes, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xEDB88320u & static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1u))));
  }
  return ~crc;
}

Config defaults() {
  Config c{};
  c.pickupSteps = 0;
  c.dropoffSteps = 0;
  c.stepIntervalUs = 5000;
  c.deliveryDwellMs = kDefaultDwellMs;
  c.pickupDwellMs = kDefaultDwellMs;
  c.seekLimitSteps = kDefaultSeekLimitSteps;
  c.seekTimeoutMs = kDefaultSeekTimeoutMs;
  c.pickupTaught = false;
  c.dropoffTaught = false;
  c.commissioned = false;
  return c;
}

StoredState encode(const Snapshot& s) {
  StoredState r{};
  r.magic = kRecordMagic; r.schema = kRecordSchema; r.length = sizeof(r);
  r.revision = s.revision; r.mode = static_cast<uint8_t>(s.mode);
  r.credit = static_cast<uint8_t>(s.credit);
  r.flags = static_cast<uint8_t>((s.positionValid ? 1 : 0) | (s.rewardInProgress ? 2 : 0));
  r.positionSteps = s.positionSteps;
  r.pickupSteps = s.config.pickupSteps; r.dropoffSteps = s.config.dropoffSteps;
  r.stepIntervalUs = s.config.stepIntervalUs; r.deliveryDwellMs = s.config.deliveryDwellMs;
  r.pickupDwellMs = s.config.pickupDwellMs; r.seekLimitSteps = s.config.seekLimitSteps;
  r.seekTimeoutMs = s.config.seekTimeoutMs;
  r.crc = crc32(reinterpret_cast<const uint8_t*>(&r), offsetof(StoredState, crc));
  return r;
}

// Schema 1 stores taught/commissioned flags in reserved's defined bit field.
bool decodeState(const StoredState& r, Snapshot& s) {
  if (r.magic != kRecordMagic || r.schema != kRecordSchema || r.length != sizeof(r) ||
      r.mode > static_cast<uint8_t>(Mode::Busy) || r.credit > static_cast<uint8_t>(Credit::Consumed) ||
      (r.flags & ~3u) || (r.reserved & ~7u) ||
      r.crc != crc32(reinterpret_cast<const uint8_t*>(&r), offsetof(StoredState, crc))) return false;
  s = {};
  s.revision = r.revision; s.mode = static_cast<Mode>(r.mode); s.credit = static_cast<Credit>(r.credit);
  s.positionValid = (r.flags & 1u) != 0; s.rewardInProgress = (r.flags & 2u) != 0;
  s.positionSteps = r.positionSteps; s.config = defaults();
  s.config.pickupSteps = r.pickupSteps; s.config.dropoffSteps = r.dropoffSteps;
  s.config.stepIntervalUs = r.stepIntervalUs; s.config.deliveryDwellMs = r.deliveryDwellMs;
  s.config.pickupDwellMs = r.pickupDwellMs; s.config.seekLimitSteps = r.seekLimitSteps;
  s.config.seekTimeoutMs = r.seekTimeoutMs;
  s.config.pickupTaught = (r.reserved & 1u) != 0;
  s.config.dropoffTaught = (r.reserved & 2u) != 0;
  s.config.commissioned = (r.reserved & 4u) != 0;
  if (s.config.stepIntervalUs < 3000 || s.config.stepIntervalUs > 20000 ||
      s.config.deliveryDwellMs > 60000 || s.config.pickupDwellMs > 60000 ||
      s.config.seekLimitSteps == 0 || s.config.seekLimitSteps > 50000 ||
      s.config.seekTimeoutMs < 1000 || s.config.seekTimeoutMs > 600000 ||
      (s.config.pickupTaught &&
       (static_cast<int64_t>(s.config.pickupSteps) > s.config.seekLimitSteps ||
        static_cast<int64_t>(s.config.pickupSteps) < -static_cast<int64_t>(s.config.seekLimitSteps))) ||
      (s.config.dropoffTaught &&
       (static_cast<int64_t>(s.config.dropoffSteps) > s.config.seekLimitSteps ||
        static_cast<int64_t>(s.config.dropoffSteps) < -static_cast<int64_t>(s.config.seekLimitSteps))) ||
      (s.config.commissioned && (!s.config.pickupTaught || !s.config.dropoffTaught))) return false;
  return true;
}

bool persistSnapshot(const Snapshot& next, void*) {
  if (!prefsOpen) return false;
  StoredState r = encode(next);
  r.reserved = static_cast<uint8_t>((next.config.pickupTaught ? 1 : 0) |
      (next.config.dropoffTaught ? 2 : 0) | (next.config.commissioned ? 4 : 0));
  r.crc = crc32(reinterpret_cast<const uint8_t*>(&r), offsetof(StoredState, crc));
  if (prefs.putBytes("state", &r, sizeof(r)) != sizeof(r)) return false;
  StoredState check{};
  if (prefs.getBytes("state", &check, sizeof(check)) != sizeof(check) ||
      memcmp(&r, &check, sizeof(r)) != 0) return false;
  Snapshot verified{};
  return decodeState(check, verified) && verified.revision == next.revision &&
      verified.mode == next.mode && verified.credit == next.credit &&
      verified.positionValid == next.positionValid && verified.positionSteps == next.positionSteps &&
      verified.rewardInProgress == next.rewardInProgress &&
      verified.config.pickupSteps == next.config.pickupSteps && verified.config.dropoffSteps == next.config.dropoffSteps &&
      verified.config.pickupTaught == next.config.pickupTaught && verified.config.dropoffTaught == next.config.dropoffTaught &&
      verified.config.commissioned == next.config.commissioned;
}

// Construction is deferred until Preferences is open, using static aligned storage.
alignas(Controller) uint8_t controllerStorage[sizeof(Controller)];
Controller* control = nullptr;
void constructController() { control = new (controllerStorage) Controller(persistSnapshot, nullptr); }

template <typename F> auto withControl(F&& fn) -> decltype(fn()) {
  xSemaphoreTake(controlMutex, portMAX_DELAY);
  auto result = fn();
  xSemaphoreGive(controlMutex);
  return result;
}
Status controllerStatusSnapshot() { return withControl([] { return control->status(); }); }
void setLastEvent(const char* text) {
  portENTER_CRITICAL(&eventMux);
  strncpy(lastEvent, text, sizeof(lastEvent) - 1);
  lastEvent[sizeof(lastEvent) - 1] = 0;
  portEXIT_CRITICAL(&eventMux);
}
void copyLastEvent(char* target, size_t capacity) {
  portENTER_CRITICAL(&eventMux);
  strncpy(target, lastEvent, capacity - 1);
  target[capacity - 1] = 0;
  portEXIT_CRITICAL(&eventMux);
}

bool hallRaw() { return digitalRead(kHallPin) == LOW; }
bool buttonRaw() { return digitalRead(kButtonPin) == LOW; }
bool buttonReleasedQualified() {
  return !buttonRaw() && !buttonDebounce.stable &&
      static_cast<uint32_t>(millis() - buttonDebounce.changed) >= kDebounceMs;
}
void motorOff() {
  for (uint8_t pin : kMotorPins) digitalWrite(pin, LOW);
  coilsOn = false;
}
void writePhase(int8_t direction) {
  phaseIndex = static_cast<uint8_t>((phaseIndex + (direction > 0 ? 1 : 7)) & 7u);
  const uint8_t pattern = kHalfStepPatterns[phaseIndex];
  for (uint8_t i = 0; i < 4; ++i)
    digitalWrite(kMotorPins[i], (pattern & (1u << (3u - i))) ? HIGH : LOW);
  coilsOn = pattern != 0;
}

const char* modeName(Mode v) {
  switch (v) { case Mode::Locked: return "Locked"; case Mode::Armed: return "Armed";
    case Mode::Busy: return "Busy"; default: return "Recovery"; }
}
const char* creditName(Credit v) {
  switch (v) { case Credit::Pending: return "Pending"; case Credit::Consumed: return "Consumed";
    default: return "None"; }
}
const char* runName(Run v) {
  switch (v) {
    case Run::SeekHall: return "SeekHall"; case Run::HallCheck: return "HallCheck";
    case Run::StartupOffset: return "StartupOffset";
    case Run::JogCounted: return "JogCounted";
    case Run::JogLease: return "JogLease"; case Run::RewardToDelivery: return "RewardToDelivery";
    case Run::DeliveryCheck: return "DeliveryCheck"; case Run::DeliveryOffset: return "DeliveryOffset";
    case Run::DeliveryDwell: return "DeliveryDwell"; case Run::RewardToPickup: return "RewardToPickup";
    case Run::PickupCheck: return "PickupCheck";
    case Run::PickupDwell: return "PickupDwell"; case Run::Fault: return "Fault";
    default: return "Idle";
  }
}
const char* resultName(Result r) {
  switch (r) { case Result::Ok: return "ok"; case Result::NotReady: return "not_ready";
    case Result::Invalid: return "invalid"; case Result::StorageFailure: return "storage_failure";
    case Result::Busy: return "busy"; case Result::Limit: return "limit";
    case Result::HallFault: return "hall_fault"; default: return "conflict"; }
}

// LAN controls and phone audio are password-free; OTA has its own password.
void jsonHeaders() {
  server.sendHeader("Cache-Control", "no-store, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.sendHeader("Referrer-Policy", "no-referrer");
}
void sendJson(int status, const String& body) { jsonHeaders(); server.send(status, "application/json", body); }
String boolJson(bool b) { return b ? "true" : "false"; }
void updateAudioTelemetry() {
  const Status s = control->status();
  const bool active = s.saved.mode == Mode::Busy && s.saved.credit == Credit::Consumed &&
      s.saved.rewardInProgress;
  if (active && !audioRewardObserved) {
    ++audioRewardCycleId;
    if (!audioRewardCycleId) ++audioRewardCycleId;
    audioDropoffArrived = false;
    audioDropoffArrivalMs = 0;
  }
  if (active && !audioDropoffArrived && s.run == Run::DeliveryDwell) {
    audioDropoffArrivalMs = millis();
    audioDropoffArrived = true;
  }
  audioRewardObserved = active;
}
String statusJson() {
  Status s{};
  bool buttonDown = false, dropoffArrived = false, localCoilsOn = false, localStartupSeek = false;
  uint32_t rewardCycleId = 0, dropoffArrivalMs = 0;
  xSemaphoreTake(controlMutex, portMAX_DELAY);
  s = control->status();
  buttonDown = buttonDebounce.stable;
  dropoffArrived = audioDropoffArrived;
  rewardCycleId = audioRewardCycleId;
  dropoffArrivalMs = audioDropoffArrivalMs;
  localCoilsOn = coilsOn;
  localStartupSeek = startupSeekActive;
  xSemaphoreGive(controlMutex);
  char event[sizeof(lastEvent)]; copyLastEvent(event, sizeof(event));
  const Snapshot& x = s.saved;
  const Config& c = x.config;
  String j; j.reserve(920);
  j = "{\"mode\":\""; j += modeName(x.mode); j += "\",\"credit\":\""; j += creditName(x.credit);
  j += "\",\"run\":\""; j += runName(s.run); j += "\",\"revision\":"; j += String(x.revision);
  j += ",\"nonce\":\""; char nonce[9]; snprintf(nonce, sizeof(nonce), "%08lx", static_cast<unsigned long>(s.bootNonce)); j += nonce;
  j += "\",\"commissioned\":"; j += boolJson(c.commissioned);
  j += ",\"position_valid\":"; j += boolJson(x.positionValid); j += ",\"position_steps\":"; j += String(x.positionSteps);
  j += ",\"pickup_steps\":"; j += String(c.pickupSteps); j += ",\"dropoff_steps\":"; j += String(c.dropoffSteps);
  j += ",\"pickup_taught\":"; j += boolJson(c.pickupTaught); j += ",\"dropoff_taught\":"; j += boolJson(c.dropoffTaught);
  j += ",\"delivery_dwell_ms\":"; j += String(c.deliveryDwellMs); j += ",\"pickup_dwell_ms\":"; j += String(c.pickupDwellMs);
  j += ",\"step_interval_us\":"; j += String(c.stepIntervalUs); j += ",\"seek_limit_steps\":"; j += String(c.seekLimitSteps);
  j += ",\"seek_timeout_ms\":"; j += String(c.seekTimeoutMs);
  j += ",\"raw_hall_active\":"; j += boolJson(s.rawHallActive); j += ",\"stable_hall_active\":"; j += boolJson(s.stableHallActive);
  j += ",\"button_down\":"; j += boolJson(buttonDown);
  j += ",\"armed\":"; j += boolJson(s.armed); j += ",\"remaining_steps\":"; j += String(s.remainingSteps);
  j += ",\"direction\":"; j += String(s.direction); j += ",\"storage_good\":"; j += boolJson(s.storageGood);
  // Read-only scheduler telemetry; the persistent record and controller are unchanged.
  j += ",\"reward_in_progress\":"; j += boolJson(x.rewardInProgress);
  j += ",\"server_ms\":"; j += String(millis());
  j += ",\"reward_cycle_id\":"; j += String(rewardCycleId);
  j += ",\"dropoff_arrived\":"; j += boolJson(dropoffArrived);
  j += ",\"dropoff_arrival_ms\":"; j += String(dropoffArrivalMs);
  j += ",\"wifi_connected\":"; j += boolJson(WiFi.status() == WL_CONNECTED); j += ",\"ip\":\"";
  j += WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("");
  j += "\",\"rssi\":"; j += WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : String("0");
  j += ",\"coils_on\":"; j += boolJson(localCoilsOn);
  j += ",\"startup_seek\":"; j += boolJson(localStartupSeek);
  j += ",\"startup_seek_reason\":\"";
  if (localStartupSeek) {
    if (s.run == Run::SeekHall) j += "Hall inactive at boot; bounded negative seek";
    else if (s.run == Run::HallCheck) j += "Hall found; confirming stable sensor state";
    else j += "Hall referenced; applying saved dropoff correction";
  }
  j += "\"";
  j += ",\"ota_window\":"; j += boolJson(otaWindowUntilMs != 0 && static_cast<int32_t>(otaWindowUntilMs - millis()) > 0);
  j += ",\"ota_seconds_left\":"; j += String(otaWindowUntilMs && static_cast<int32_t>(otaWindowUntilMs - millis()) > 0 ? (otaWindowUntilMs - millis()) / 1000 : 0);
  j += ",\"event\":\""; j += event; j += "\"}";
  return j;
}
void statusReply(int code = 200) { sendJson(code, statusJson()); }
bool audioPreloadAllowed() {
  return withControl([] {
    const Status s = control->status();
    return s.saved.mode == Mode::Locked && !s.saved.rewardInProgress &&
        s.run == Run::Idle && !startupSeekActive;
  });
}

void reject(int code, const char* message);
bool parseNonceRevision(uint32_t& nonce, uint32_t& revision, bool allowStaleRevision = false) {
  String n = server.header("X-RR-Nonce");
  String v = server.header("X-RR-Revision");
  if (n.length() != 8 || v.length() == 0 || v.length() > 10) {
    reject(400, "missing_or_malformed_revision_headers"); return false;
  }
  uint32_t nVal = 0, rVal = 0;
  for (size_t i = 0; i < n.length(); ++i) {
    const char ch = n[i];
    uint8_t d = ch >= '0' && ch <= '9' ? ch - '0' : (ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : (ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : 255));
    if (d > 15) { reject(400, "malformed_boot_nonce"); return false; }
    nVal = (nVal << 4) | d;
  }
  for (size_t i = 0; i < v.length(); ++i) {
    if (v[i] < '0' || v[i] > '9') { reject(400, "malformed_revision"); return false; }
    const uint32_t d = static_cast<uint32_t>(v[i] - '0');
    if (rVal > (UINT32_MAX - d) / 10u) { reject(400, "malformed_revision"); return false; }
    rVal = rVal * 10u + d;
  }
  nonce = nVal; revision = rVal;
  const Status s = controllerStatusSnapshot();
  if (nonce != s.bootNonce || (!allowStaleRevision && revision != s.saved.revision)) {
    sendJson(409, "{\"error\":\"stale_state\",\"status\":" + statusJson() + "}");
    return false;
  }
  return true;
}
bool fieldsExactly(std::initializer_list<const char*> names) {
  if (server.args() != names.size()) return false;
  bool seen[10] = {};
  size_t index = 0;
  for (int i = 0; i < server.args(); ++i) {
    size_t match = names.size(); size_t candidate = 0;
    for (const char* n : names) { if (server.argName(i) == n) match = candidate; ++candidate; }
    if (match == names.size() || match >= 10 || seen[match] || server.arg(i).length() > 24) return false;
    seen[match] = true; ++index;
  }
  return index == names.size();
}
bool parseIntArg(const char* name, int32_t min, int32_t max, int32_t& out) {
  String s = server.arg(name);
  if (s.length() == 0 || s.length() > 12) return false;
  size_t i = s[0] == '-' ? 1 : 0;
  if (i == s.length()) return false;
  for (; i < s.length(); ++i) if (s[i] < '0' || s[i] > '9') return false;
  char* end = nullptr; long value = strtol(s.c_str(), &end, 10);
  if (!end || *end || value < min || value > max) return false;
  out = static_cast<int32_t>(value); return true;
}
bool parseUintArg(const char* name, uint32_t min, uint32_t max, uint32_t& out) {
  String s = server.arg(name);
  if (s.length() == 0 || s.length() > 10) return false;
  uint32_t v = 0;
  for (size_t i = 0; i < s.length(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    const uint32_t d = s[i] - '0';
    if (d > max || v > (max - d) / 10u) return false;
    v = v * 10u + d;
  }
  if (v < min || v > max) return false; out = v; return true;
}
void mutationReply(Result r) {
  const int status = r == Result::Ok ? 200 : r == Result::StorageFailure ? 503 : 409;
  String j = statusJson();
  if (r != Result::Ok) {
    j.remove(j.length() - 1);
    j += ",\"error\":\""; j += resultName(r); j += "\"}";
  }
  sendJson(status, j);
}
void reject(int code, const char* message) {
  String j = "{\"error\":\""; j += message; j += "\"}"; sendJson(code, j);
}
bool mutation(uint32_t& n, uint32_t& r, std::initializer_list<const char*> fields,
              bool allowStaleRevision = false) {
  if (!parseNonceRevision(n, r, allowStaleRevision)) return false;
  if (!fieldsExactly(fields)) { reject(400, "invalid_fields"); return false; }
  return true;
}
bool motionPermitted() {
  if (otaWindowUntilMs && static_cast<int32_t>(otaWindowUntilMs - millis()) > 0) {
    reject(409, "ota_maintenance_window"); return false;
  }
  return true;
}

bool otaSafe() {
  return withControl([] {
    const Status s = control->status();
    return s.saved.mode == Mode::Locked && s.saved.credit == Credit::None &&
        !s.saved.rewardInProgress && s.run == Run::Idle && !coilsOn;
  });
}
bool otaTransferSafe() {
  return withControl([] {
    const Status s = control->status();
    return otaWindowUntilMs && static_cast<int32_t>(otaWindowUntilMs - millis()) > 0 &&
        s.saved.mode == Mode::Recovery && s.saved.credit == Credit::None &&
        !s.saved.rewardInProgress && s.run == Run::Idle && !coilsOn;
  });
}
void finishOtaWindow() {
  if (otaServiceStarted) { ArduinoOTA.end(); otaServiceStarted = false; }
  otaWindowUntilMs = otaWindowStartedMs = 0;
}
void startOtaService() {
  if (otaServiceStarted || WiFi.status() != WL_CONNECTED || otaPassword.length() < 12) return;
  ArduinoOTA.setHostname(kHostname).setPassword(otaPassword.c_str()).setRebootOnSuccess(true);
  ArduinoOTA.onStart([] {
    if (!otaTransferSafe()) { Update.abort(); otaInProgress = false; return; }
    const Result stopped = withControl([] {
      const Status s = control->status();
      const Result r = control->stop(s.bootNonce, s.saved.revision);
      motorOff();
      return r;
    });
    if (stopped != Result::Ok) { Update.abort(); otaInProgress = false; return; }
    otaInProgress = true;
    setLastEvent("IDE OTA started; position invalidated");
  });
  ArduinoOTA.onEnd([] { otaInProgress = false; setLastEvent("IDE OTA complete; rebooting into recovery"); });
  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    char event[48]; snprintf(event, sizeof(event), "IDE OTA failed (%u); no reboot", static_cast<unsigned>(error)); setLastEvent(event);
    // The core aborts a failed Update and returns to the sketch without reboot.
  });
  ArduinoOTA.begin(); otaServiceStarted = true;
}

void handleStatus() { sendJson(200, statusJson()); }
void handleActivate() {
  uint32_t n, rev; if (!mutation(n, rev, {})) return;
  if (!motionPermitted()) return;
  const Status before = controllerStatusSnapshot();
  if (before.run != Run::Idle || !before.saved.config.commissioned || !before.saved.positionValid ||
      before.saved.positionSteps != before.saved.config.pickupSteps ||
      !before.saved.config.pickupTaught || !before.saved.config.dropoffTaught) {
    reject(409, "setup_required_before_activation"); return;
  }
  const Result credit = withControl([&] { return control->setPendingCredit(n, rev, true); });
  if (credit != Result::Ok) { mutationReply(credit); return; }
  const Status s = controllerStatusSnapshot();
  const Result armed = withControl([&] { return control->arm(n, s.saved.revision, millis(), hallRaw(), buttonReleasedReady); });
  if (armed != Result::Ok) { setLastEvent("Credit saved; activation requires setup"); mutationReply(armed); return; }
  setLastEvent("One reward armed; press physical button to start");
  mutationReply(Result::Ok);
}
void handleArmPending() {
  uint32_t n, rev; if (!mutation(n, rev, {})) return;
  if (!motionPermitted()) return;
  const Result armed = withControl([&] { return control->arm(n, rev, millis(), hallRaw(), buttonReleasedReady); });
  if (armed == Result::Ok) setLastEvent("Preserved reward armed; fresh physical button press required");
  mutationReply(armed);
}
void handleCancel() {
  uint32_t n, rev; if (!mutation(n, rev, {})) return;
  if (!motionPermitted()) return;
  mutationReply(withControl([&] { return control->cancelPending(n, rev); }));
}
void handleReconcile() {
  uint32_t n, rev; if (!mutation(n, rev, {"confirmed"})) return;
  if (!motionPermitted()) return;
  mutationReply(withControl([&] { return control->reconcile(n, rev, server.arg("confirmed") == "hall", hallRaw()); }));
}
void handleStop() {
  uint32_t n, rev; if (!mutation(n, rev, {}, true)) return;
  StopRequest request{n, xTaskGetCurrentTaskHandle(), Result::Busy};
  StopRequest* pending = &request;
  if (!stopQueue || xQueueSend(stopQueue, &pending, 0) != pdTRUE) {
    reject(503, "stop_queue_busy"); return;
  }
  // Wait only for the motion owner to apply STOP. The request loop holds no
  // controller lock here, so it cannot pause a step while doing HTTP work.
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  mutationReply(request.result);
}
void handleConfig() {
  uint32_t n, rev; if (!parseNonceRevision(n, rev)) return;
  const bool baseFields = fieldsExactly({"step_interval_us", "delivery_dwell_ms", "pickup_dwell_ms", "seek_limit_steps", "seek_timeout_ms", "confirmed"});
  const bool fieldsWithAlignment = fieldsExactly({"step_interval_us", "delivery_dwell_ms", "pickup_dwell_ms", "seek_limit_steps", "seek_timeout_ms", "dropoff_steps", "confirmed"});
  if (!baseFields && !fieldsWithAlignment) { reject(400, "invalid_fields"); return; }
  if (!motionPermitted()) return;
  if (server.arg("confirmed") != "save") { reject(400, "confirm_configuration"); return; }
  const Status current = controllerStatusSnapshot();
  if (current.saved.mode != Mode::Locked || current.saved.credit != Credit::None || current.run != Run::Idle) {
    reject(409, "configuration_requires_locked_idle"); return;
  }
  const Config& old = current.saved.config;
  Config c = old;
  uint32_t value;
  if (!parseUintArg("step_interval_us", 3000, 20000, value)) { reject(400, "invalid_step_interval"); return; } c.stepIntervalUs = value;
  if (!parseUintArg("delivery_dwell_ms", 0, 60000, value)) { reject(400, "invalid_delivery_dwell"); return; } c.deliveryDwellMs = value;
  if (!parseUintArg("pickup_dwell_ms", 0, 60000, value)) { reject(400, "invalid_pickup_dwell"); return; } c.pickupDwellMs = value;
  if (!parseUintArg("seek_limit_steps", 1, 50000, value)) { reject(400, "invalid_seek_limit"); return; } c.seekLimitSteps = value;
  if (!parseUintArg("seek_timeout_ms", 1000, 600000, value)) { reject(400, "invalid_seek_timeout"); return; } c.seekTimeoutMs = value;
  if (fieldsWithAlignment) {
    int32_t offset;
    if (!parseIntArg("dropoff_steps", -static_cast<int32_t>(c.seekLimitSteps), static_cast<int32_t>(c.seekLimitSteps), offset)) {
      reject(400, "invalid_dropoff_alignment"); return;
    }
    c.dropoffSteps = offset;
    c.dropoffTaught = true;
  }
  mutationReply(withControl([&] { return control->configure(n, rev, c); }));
}
void handleCountJog() {
  uint32_t n, rev; if (!mutation(n, rev, {"direction", "steps"})) return;
  if (!motionPermitted()) return;
  const String d = server.arg("direction"); uint32_t count;
  if ((d != "+" && d != "-") || !parseUintArg("steps", 1, kMaximumCountedJog, count)) { reject(400, "invalid_jog"); return; }
  mutationReply(withControl([&] { return control->startCountedJog(n, rev, d == "+" ? 1 : -1, count, hallRaw(), micros(), millis()); }));
}
void handleRenewJog() {
  uint32_t n, rev; if (!mutation(n, rev, {"direction", "lease_ms"})) return;
  if (!motionPermitted()) return;
  const String d = server.arg("direction"); uint32_t lease;
  if ((d != "+" && d != "-") || !parseUintArg("lease_ms", 100, kMaximumJogLeaseMs, lease)) { reject(400, "invalid_jog_lease"); return; }
  mutationReply(withControl([&] { return control->renewJog(n, rev, d == "+" ? 1 : -1, lease, millis(), hallRaw()); }));
}
void handleSeek() {
  uint32_t n, rev; if (!mutation(n, rev, {"direction"})) return;
  if (!motionPermitted()) return;
  const String d = server.arg("direction");
  if (d != "+" && d != "-") { reject(400, "invalid_direction"); return; }
  mutationReply(withControl([&] { return control->startSeek(n, rev, d == "+" ? 1 : -1, micros(), millis(), hallRaw()); }));
}
void handleTeachPickup() {
  uint32_t n, rev; if (!mutation(n, rev, {"confirmed"})) return;
  if (!motionPermitted()) return;
  mutationReply(withControl([&] { return control->teachPickup(n, rev, server.arg("confirmed") == "yes", hallRaw()); }));
}
void handleTeachDropoff() {
  uint32_t n, rev; if (!mutation(n, rev, {"confirmed"})) return;
  if (!motionPermitted()) return;
  mutationReply(withControl([&] { return control->teachDropoff(n, rev, server.arg("confirmed") == "yes", hallRaw()); }));
}
void handleGoPickup() {
  uint32_t n, rev; if (!mutation(n, rev, {"confirmed"})) return;
  if (!motionPermitted()) return;
  if (server.arg("confirmed") != "return") { reject(400, "confirm_return_to_pickup"); return; }
  mutationReply(withControl([&] { return control->goPickup(n, rev, micros(), millis(), hallRaw()); }));
}
void handleOtaEnable() {
  uint32_t n, rev; if (!mutation(n, rev, {"confirmed"})) return;
  if (server.arg("confirmed") != "idle") { reject(400, "confirm_idle"); return; }
  if (!otaSafe() || WiFi.status() != WL_CONNECTED || otaPassword.length() < 12) { reject(409, "ota_requires_locked_idle_and_wifi"); return; }
  const Result released = withControl([&] { return control->releaseOutputs(n, rev); });
  if (released != Result::Ok) { mutationReply(released); return; }
  withControl([&] { motorOff(); return 0; });
  otaWindowStartedMs = millis(); otaWindowUntilMs = otaWindowStartedMs + 300000u;
  startOtaService();
  if (!otaServiceStarted) { finishOtaWindow(); reject(503, "ota_service_unavailable"); return; }
  setLastEvent("IDE OTA window open for 5 minutes");
  mutationReply(Result::Ok);
}
void handleOtaDisable() {
  uint32_t n, rev; if (!mutation(n, rev, {})) return;
  if (otaInProgress) { reject(409, "ota_in_progress"); return; }
  const Result stopped = withControl([&] { return control->stop(n, rev); });
  if (stopped != Result::Ok) { mutationReply(stopped); return; }
  withControl([&] { motorOff(); return 0; });
  finishOtaWindow();
  mutationReply(Result::Ok);
}

void registerRoutes() {
  const char* headers[] = {"X-RR-Nonce", "X-RR-Revision"}; server.collectHeaders(headers, 2);
  server.on("/", HTTP_GET, [] { server.sendHeader("Cache-Control", "no-store"); server.send(200, "text/html; charset=utf-8", PAGE342); });
  server.on("/api/status", HTTP_GET, handleStatus);
  for (size_t i = 0; i < AUDIO342_ASSET_COUNT; ++i) {
    const AudioAsset342* asset = &AUDIO342_ASSETS[i];
    String route = "/audio342/"; route += asset->name; route += ".wav";
    server.on(route.c_str(), HTTP_GET, [asset] {
      if (!audioPreloadAllowed()) { sendJson(409, "{\"error\":\"audio_preload_only_while_idle\"}"); return; }
      jsonHeaders();
      server.sendHeader("Cache-Control", "private, max-age=3600");
      server.send_P(200, "audio/wav", reinterpret_cast<PGM_P>(asset->data), asset->size);
    });
  }
  server.on("/api/activate", HTTP_POST, handleActivate);
  server.on("/api/arm-pending", HTTP_POST, handleArmPending);
  server.on("/api/cancel", HTTP_POST, handleCancel);
  server.on("/api/reconcile", HTTP_POST, handleReconcile);
  server.on("/api/stop", HTTP_POST, handleStop);
  server.on("/api/config", HTTP_POST, handleConfig);
  server.on("/api/jog/count", HTTP_POST, handleCountJog);
  server.on("/api/jog/renew", HTTP_POST, handleRenewJog);
  server.on("/api/hall/seek", HTTP_POST, handleSeek);
  server.on("/api/teach/pickup", HTTP_POST, handleTeachPickup);
  server.on("/api/teach/dropoff", HTTP_POST, handleTeachDropoff);
  server.on("/api/go-pickup", HTTP_POST, handleGoPickup);
  server.on("/api/ota/enable", HTTP_POST, handleOtaEnable);
  server.on("/api/ota/disable", HTTP_POST, handleOtaDisable);
  server.onNotFound([] { reject(404, "not_found"); });
}

bool saveCredential(const char* key, const String& value) {
  if (!prefsOpen || !value.length()) return false;
  if (prefs.putString(key, value) != value.length()) return false;
  return prefs.getString(key, "") == value;
}
void beginStation() {
  if (wifiSsid.isEmpty() || wifiPassword.length() < 8) {
    wifiProvisioned = false; networkReady = false;
    setLastEvent("Provision Wi-Fi over USB serial");
    return;
  }
  wifiProvisioned = true; WiFi.mode(WIFI_STA); WiFi.setHostname(kHostname); WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str()); lastWiFiAttemptMs = millis();
}
void provisioningHelp() {
  Serial.println("Reward Rocket 393 local setup commands (values are never echoed):");
  Serial.println("  WIFI SSID <network name>");
  Serial.println("  WIFI PASSWORD <network password>");
  Serial.println("  OTA PASSWORD <upload password, at least 12 chars; optional for LAN>");
  Serial.println("  WIFI SAVE | WIFI CLEAR | STATUS | HELP");
  Serial.println("  NVS RESET CONFIRM (clears motion/config state; preserves network credentials)");
  Serial.println("Station Wi-Fi only; no access point or cloud service is created.");
}
void serialStatus() {
  const Status s = controllerStatusSnapshot();
  Serial.printf("%s credit=%s run=%s revision=%lu position_valid=%d position=%ld storage=%d WiFi=%s ip=%s ota_window=%d\n",
      modeName(s.saved.mode), creditName(s.saved.credit), runName(s.run), (unsigned long)s.saved.revision,
      s.saved.positionValid, (long)s.saved.positionSteps, s.storageGood,
      WiFi.status() == WL_CONNECTED ? "connected" : (wifiProvisioned ? "connecting" : "not provisioned"),
      WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-",
      otaWindowUntilMs && static_cast<int32_t>(otaWindowUntilMs - millis()) > 0);
}
void handleSerialLine(char* line) {
  String s(line); s.trim(); s.toUpperCase();
  if (s == "HELP") { provisioningHelp(); return; }
  if (s == "STATUS") { serialStatus(); return; }
  if (s == "NVS RESET CONFIRM") {
    xSemaphoreTake(controlMutex, portMAX_DELAY);
    const Status beforeReset = control->status();
    if (beforeReset.saved.mode == Mode::Busy || beforeReset.run != Run::Idle) {
      xSemaphoreGive(controlMutex);
      Serial.println("NVS reset refused while motion is active; stop and wait for Idle first.");
      return;
    }
    motorOff();
    Snapshot seed{}; seed.revision = 1; seed.mode = Mode::Recovery; seed.credit = Credit::None;
    seed.config = defaults(); seed.positionValid = false; seed.positionSteps = 0; seed.rewardInProgress = false;
    StoredState r = encode(seed);
    r.reserved = 0; r.crc = crc32(reinterpret_cast<const uint8_t*>(&r), offsetof(StoredState, crc));
    const bool ok = prefsOpen && prefs.putBytes("state", &r, sizeof(r)) == sizeof(r);
    StoredState check{}; Snapshot verified{};
    const bool readback = ok && prefs.getBytes("state", &check, sizeof(check)) == sizeof(check) &&
        memcmp(&r, &check, sizeof(r)) == 0 && decodeState(check, verified);
    if (!readback) { xSemaphoreGive(controlMutex); Serial.println("NVS reset/readback failed; controller stays in recovery."); return; }
    uint32_t nonce = esp_random(); if (!nonce) nonce = 1;
    control->boot(true, verified, nonce, hallDebounce.stable, buttonDebounce.stable, millis());
    startupSeekActive = control->status().run == Run::SeekHall ||
        control->status().run == Run::StartupOffset;
    setLastEvent("NVS motion/config state reset; network credentials retained");
    xSemaphoreGive(controlMutex);
    Serial.println("Motion/config state reset and verified. Position unknown; network credentials retained.");
    return;
  }
  if (s == "WIFI SAVE") { beginStation(); Serial.println(wifiProvisioned ? "Credentials saved and station connection started." : "Credentials incomplete; values not displayed."); return; }
  if (s == "WIFI CLEAR") {
    finishOtaWindow(); prefs.remove("ssid"); prefs.remove("wifipw"); prefs.remove("parentpw");
    wifiSsid = wifiPassword = otaPassword = ""; WiFi.disconnect(true, true); wifiProvisioned = false; networkReady = false;
    Serial.println("Stored network credentials removed."); return;
  }
  const char* key = nullptr; const char* prefix = nullptr; size_t prefixLen = 0; size_t maxLen = 0; size_t minLen = 0;
  if (strncmp(line, "WIFI SSID ", 10) == 0) { key = "ssid"; prefix = "WIFI SSID "; prefixLen = 10; maxLen = kMaxWifiSsid; minLen = 1; }
  else if (strncmp(line, "WIFI PASSWORD ", 14) == 0) { key = "wifipw"; prefix = "WIFI PASSWORD "; prefixLen = 14; maxLen = kMaxWifiPassword; minLen = 8; }
  else if (strncmp(line, "OTA PASSWORD ", 13) == 0) { key = "parentpw"; prefix = "OTA PASSWORD "; prefixLen = 13; maxLen = kMaxParentPassword; minLen = 12; }
  // Legacy provisioning command and NVS key remain compatible; password is OTA-only.
  else if (strncmp(line, "PARENT PASSWORD ", 16) == 0) { key = "parentpw"; prefix = "PARENT PASSWORD "; prefixLen = 16; maxLen = kMaxParentPassword; minLen = 12; }
  if (!key || !prefix) { Serial.println("Unknown or malformed setup command; type HELP."); return; }
  (void)prefix;
  const size_t len = strlen(line + prefixLen);
  if (len < minLen || len > maxLen) { Serial.println("Value length rejected; nothing was saved."); return; }
  String value(line + prefixLen);
  finishOtaWindow();
  if (!saveCredential(key, value)) { Serial.println("Credential save/readback failed; value not displayed."); return; }
  if (strcmp(key, "ssid") == 0) wifiSsid = value;
  else if (strcmp(key, "wifipw") == 0) wifiPassword = value;
  else otaPassword = value;
  Serial.println("Credential saved to device NVS; value not echoed.");
}
void pollSerial() {
  uint8_t consumed = 0;
  while (Serial.available() > 0 && consumed++ < 64) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\r') continue;
    if (ch == '\n') {
      if (!serialOverflow) { serialLine[serialLength] = 0; handleSerialLine(serialLine); }
      else Serial.println("Setup line too long; discarded.");
      serialLength = 0; serialOverflow = false; continue;
    }
    if (serialOverflow) continue;
    if (serialLength + 1 >= sizeof(serialLine)) { serialOverflow = true; serialLength = 0; continue; }
    serialLine[serialLength++] = ch;
  }
}

// Motion timing has its own high-priority service task. The Arduino loop may
// block in WebServer's synchronous request parser/writer; that must not delay
// Hall/button sampling, the controller's gap monitor, or a GPIO phase write.
// No network or Serial I/O is performed here. Control state is locked only for
// short controller/GPIO operations and copied before any event formatting.
void controlServiceTask(void*) {
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(1));
    const uint32_t nowMs = millis();
    const bool otaWindowOpen = otaWindowUntilMs &&
        static_cast<int32_t>(otaWindowUntilMs - nowMs) > 0;
    bool freshPress = false;
    Motion action = Motion::None;
    xSemaphoreTake(controlMutex, portMAX_DELAY);
    hallDebounce.update(hallRaw(), nowMs, kDebounceMs);
    buttonDebounce.update(buttonRaw(), nowMs, kDebounceMs);
    StopRequest* stopRequest = nullptr;
    if (stopQueue && xQueueReceive(stopQueue, &stopRequest, 0) == pdTRUE && stopRequest) {
      // Drop coil drive before persistence. A slow flash write can delay the
      // response, but cannot leave the motor energized or block WebServer's
      // core tick because the owner task is the only executor of STOP.
      motorOff();
      const Status current = control->status();
      stopRequest->result = control->stop(stopRequest->nonce, current.saved.revision);
      if (stopRequest->result == Result::Ok)
        setLastEvent("Stopped; position invalid; recovery required");
    }
    buttonReleasedReady = buttonReleasedQualified();
    freshPress = control->updateButton(buttonDebounce.stable,
                                      buttonReleasedReady, nowMs);
    if (!otaInProgress && !otaWindowOpen && freshPress) {
      const Status st = control->status();
      if (st.armed) {
        const Result started = control->startReward(
            st.bootNonce, st.saved.revision, micros(), nowMs, hallRaw(), true);
        if (started == Result::Ok) setLastEvent("Reward started from fresh physical button press");
        else setLastEvent("Reward start blocked; inspect controller state");
      }
    }
    if (!otaInProgress && !otaWindowOpen) {
      action = control->tick(micros(), nowMs, hallRaw(), hallDebounce.stable);
      if (action == Motion::StepPositive || action == Motion::StepNegative) {
        writePhase(action == Motion::StepPositive ? 1 : -1);
        control->reportStepWritten();
      } else if (action == Motion::ReleaseCoils || action == Motion::RewardComplete || action == Motion::Fault) {
        motorOff();
        if (action == Motion::ReleaseCoils && startupSeekActive) {
          startupSeekActive = false;
          setLastEvent("Explicit Hall reference complete; no reward started");
        } else if (action == Motion::RewardComplete) {
          setLastEvent("Reward cycle returned to taught pickup");
        } else if (action == Motion::Fault) {
          startupSeekActive = false;
          setLastEvent("Motion fault; inspect and reconcile before reuse");
        }
      }
    }
    updateAudioTelemetry();
    xSemaphoreGive(controlMutex);
    if (stopRequest) xTaskNotifyGive(stopRequest->waiter);
  }
}
} // namespace

void setup() {
  for (uint8_t pin : kMotorPins) { digitalWrite(pin, LOW); pinMode(pin, OUTPUT); }
  pinMode(kHallPin, INPUT_PULLUP); pinMode(kButtonPin, INPUT_PULLUP);
  Serial.begin(kSerialBaud);
  Serial.println("Reward Rocket 393 boot: serial ready; initializing controller.");
  controlMutex = xSemaphoreCreateMutex();
  if (!controlMutex) { Serial.println("Controller mutex creation failed; motor remains off."); while (true) delay(1000); }
  stopQueue = xQueueCreate(4, sizeof(StopRequest*));
  if (!stopQueue) { Serial.println("STOP queue creation failed; motor remains off."); while (true) delay(1000); }
  const uint32_t sensorStart = millis();
  hallDebounce.begin(hallRaw(), sensorStart); buttonDebounce.begin(buttonRaw(), sensorStart);
  // Require a full stable interval before interpreting the single Hall at boot.
  while (static_cast<uint32_t>(millis() - sensorStart) < kDebounceMs) {
    const uint32_t sampleAt = millis();
    hallDebounce.update(hallRaw(), sampleAt, kDebounceMs);
    buttonDebounce.update(buttonRaw(), sampleAt, kDebounceMs);
    delay(1);
  }
  const uint32_t now = millis();
  motorOff();
  prefsOpen = prefs.begin(kPreferencesNamespace, false);
  wifiSsid = prefs.getString("ssid", ""); wifiPassword = prefs.getString("wifipw", ""); otaPassword = prefs.getString("parentpw", "");
  constructController();
  StoredState stored{}; Snapshot initial{}; bool recordValid = false;
  if (prefsOpen && prefs.getBytesLength("state") == 0) {
    initial = {}; initial.revision = 1; initial.mode = Mode::Recovery; initial.credit = Credit::None;
    initial.config = defaults(); initial.positionValid = false; initial.rewardInProgress = false;
    stored = encode(initial); stored.reserved = 0;
    stored.crc = crc32(reinterpret_cast<const uint8_t*>(&stored), offsetof(StoredState, crc));
    if (prefs.putBytes("state", &stored, sizeof(stored)) == sizeof(stored)) {
      StoredState check{};
      if (prefs.getBytes("state", &check, sizeof(check)) == sizeof(check) &&
          memcmp(&stored, &check, sizeof(stored)) == 0) recordValid = decodeState(check, initial);
    }
  } else if (prefsOpen && prefs.getBytesLength("state") == sizeof(stored) &&
             prefs.getBytes("state", &stored, sizeof(stored)) == sizeof(stored)) recordValid = decodeState(stored, initial);
  uint32_t nonce = esp_random(); if (!nonce) nonce = 1;
  // Pass the Hall value after stable input qualification. Boot restores only
  // a clean saved position; unknown/interrupted states stay still in Recovery.
  control->boot(recordValid, initial, nonce, hallDebounce.stable, buttonDebounce.stable, now);
  startupSeekActive = control->status().run == Run::SeekHall ||
      control->status().run == Run::StartupOffset;
  if (startupSeekActive) setLastEvent("Startup position restored or left for explicit recovery");
  registerRoutes(); // No sockets before Wi-Fi initializes the network stack.
  beginStation();
  const BaseType_t taskCreated = xTaskCreatePinnedToCore(controlServiceTask, "rrMotion", 4096, nullptr, 5, &controlTaskHandle, 1);
  if (taskCreated != pdPASS) { Serial.println("Motion service task creation failed; motor remains off."); while (true) delay(1000); }
  Serial.println("Reward Rocket 393 ready; independent motion service active; coils LOW; no motor motion at boot; no reward starts from Wi-Fi alone.");
  provisioningHelp(); serialStatus();
}

void loop() {
  pollSerial();
  const uint32_t now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (!httpStarted) { server.begin(); httpStarted = true; }
    if (!networkReady) {
      networkReady = true;
      if (MDNS.begin(kHostname)) { MDNS.addService("http", "tcp", kHttpPort); mdnsReady = true; }
      setLastEvent("LAN ready at reward-rocket.local");
      Serial.printf("LAN connected; parent controls at http://%s/ (LAN access without login).\n", WiFi.localIP().toString().c_str());
    }
  } else {
    networkReady = false; mdnsReady = false;
    if (wifiProvisioned && static_cast<uint32_t>(now - lastWiFiAttemptMs) > 30000) {
      lastWiFiAttemptMs = now; WiFi.reconnect();
    }
  }

  if (httpStarted && WiFi.status() == WL_CONNECTED) server.handleClient();
  if (otaWindowUntilMs && static_cast<int32_t>(now - otaWindowUntilMs) >= 0 && !otaInProgress) {
    withControl([] {
      const Status st = control->status();
      const Result result = control->stop(st.bootNonce, st.saved.revision);
      motorOff(); return result;
    });
    finishOtaWindow(); setLastEvent("IDE OTA window expired; position requires recovery");
  }
  if (otaWindowUntilMs && static_cast<int32_t>(otaWindowUntilMs - now) > 0) ArduinoOTA.handle();
  if (now - lastSerialStatusMs > 10000) { lastSerialStatusMs = now; serialStatus(); }
  delay(1);
}
