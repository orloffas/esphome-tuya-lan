// Pure, FreeRTOS- and ESPHome-independent logic for the hub's data-point state
// machine, split out of tuya_lan.cpp so it can be unit-tested on a host. The
// socket, the task and the queues stay in tuya_lan.cpp and are covered by
// `esphome compile` and live testing instead.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

namespace esphome {
namespace tuya_lan {

using BoolDpMap = std::map<uint8_t, bool>;

// Which boolean DPs must be (re)sent: those the plug has not yet confirmed at
// the value we want. `confirmed` is cleared on disconnect, so after a reconnect
// every desired DP is returned and re-asserted — this is what stops a commanded
// OFF from being lost across a dropped connection.
inline std::vector<uint8_t> dps_needing_assert(const BoolDpMap &desired, const BoolDpMap &confirmed) {
  std::vector<uint8_t> out;
  for (const auto &kv : desired) {
    auto it = confirmed.find(kv.first);
    if (it == confirmed.end() || it->second != kv.second)
      out.push_back(kv.first);
  }
  return out;
}

// Tuya data-point ids run 1..255; anything else in a status payload is junk.
inline bool valid_dp_id(long id) { return id >= 1 && id <= 255; }

}  // namespace tuya_lan
}  // namespace esphome
