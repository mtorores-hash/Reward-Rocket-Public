#pragma once

// Runtime network credentials are provisioned over the USB serial console and
// stored in NVS. No SSID, Wi-Fi password, or parent password belongs in source.
namespace rr334cfg {
static constexpr uint8_t kMotorPins[4] = {4, 5, 6, 7};
static constexpr uint8_t kHallPin = 15;   // active LOW, INPUT_PULLUP
static constexpr uint8_t kButtonPin = 17; // active LOW, INPUT_PULLUP
static constexpr uint8_t kHttpPort = 80;
static constexpr uint32_t kDebounceMs = 20;
static constexpr uint32_t kSerialBaud = 115200;
static constexpr uint16_t kMaxHttpBody = 1024;
static constexpr uint16_t kMaxWifiSsid = 32;
static constexpr uint16_t kMaxWifiPassword = 63;
static constexpr uint16_t kMaxParentPassword = 64;
static constexpr const char* kHostname = "reward-rocket";
static constexpr const char* kPreferencesNamespace = "rr334";

// Debounce the two active-low inputs on independent clocks. `changed` tracks
// the most recent raw edge, so button arming can require a full stable release.
struct Debouncer {
  bool initialized = false;
  bool raw = false;
  bool stable = false;
  uint32_t changed = 0;

  void begin(bool initial, uint32_t nowMs) {
    initialized = true;
    raw = stable = initial;
    changed = nowMs;
  }
  bool update(bool sample, uint32_t nowMs, uint32_t intervalMs) {
    if (!initialized) { begin(sample, nowMs); return false; }
    if (sample != raw) { raw = sample; changed = nowMs; }
    if (raw != stable && static_cast<uint32_t>(nowMs - changed) >= intervalMs) {
      stable = raw;
      return true;
    }
    return false;
  }
};
}
