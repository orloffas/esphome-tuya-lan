#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "tuya_protocol.h"

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#endif

namespace esphome {
namespace tuya_lan {

// A decoded data-point value. Tuya supports more types, but a LAN smart plug
// only needs booleans (switch) and integers (energy metering). Add more when a
// real device needs them.
struct DpValue {
  enum Type : uint8_t { BOOL, INT } type;
  union {
    bool b;
    int32_t i;
  };
};

// Fixed-size POD passed through the FreeRTOS queue from the network task to the
// main loop (which owns all entity publishing).
struct DpUpdate {
  uint8_t dp;
  uint8_t type;  // DpValue::Type
  int32_t value;
};

struct Command {
  uint8_t dp;
  int32_t value;  // bool encoded as 0/1
};

class TuyaLanDevice : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // --- configuration (set from codegen) ---
  void set_host(const std::string &host) { this->host_ = host; }
  void set_port(uint16_t port) { this->port_ = port; }
  void set_device_id(const std::string &id) { this->device_id_ = id; }
  void set_local_key(const std::string &key) { this->local_key_ = key; }
  void set_version(proto::Version v) { this->version_ = v; }
  void set_poll_interval(uint32_t ms) { this->poll_interval_ = ms; }

  // --- child-entity wiring (called from main loop during setup) ---
  void register_dp_listener(uint8_t dp, std::function<void(const DpValue &)> cb) {
    this->listeners_.push_back({dp, std::move(cb)});
  }
  void add_online_callback(std::function<void(bool)> cb) {
    this->online_callbacks_.push_back(std::move(cb));
  }

  // Called from switch write_state() on the main loop: queue a boolean DP write.
  void set_bool_dp(uint8_t dp, bool value);
  // Drop the stored intent for one DP (or all of them) so the device stops
  // re-asserting it after reconnects; the switch still follows the plug's own
  // reports. Use this when control is handed back to another controller, so a
  // stale desired state cannot flip a plug the other controller just changed.
  void release_dp(uint8_t dp);
  void release_all();

  bool is_online() const { return this->online_.load(); }
  // True while some DP's desired value differs from what the plug last reported.
  bool has_pending_writes() const { return this->pending_writes_.load(); }

 protected:
#ifdef USE_ESP32
  static void task_entry(void *arg);
  void task_loop();
  bool connect_();
  void disconnect_();
  bool negotiate_session_();
  bool send_frame_(const proto::Bytes &frame);
  // Pull whole frames out of the RX accumulator and dispatch them. Returns true
  // if at least one frame authenticated (used to mark the device online).
  bool pump_rx_();
  bool handle_frame_(const proto::Bytes &frame);
  bool send_command_(uint8_t cmd, const std::string &json);
  void publish_dps_(const std::string &json);
  // Move queued write intents into desired_ (task-side, latest value per DP).
  void drain_commands_();
  // Re-send any DP whose last reported value does not match what we want, with
  // throttling; a send failure drops the connection so the state is re-asserted.
  void assert_desired_(uint32_t now);
  bool apply_dp_(uint8_t dp, bool value);
  std::string timestamp_() const;
  // Recompute has_pending_writes() from desired_ vs confirmed_.
  void update_pending_();

  TaskHandle_t task_{nullptr};
  QueueHandle_t rx_queue_{nullptr};   // DpUpdate: task -> main loop
  QueueHandle_t tx_queue_{nullptr};   // Command: main loop -> task
  int sock_{-1};
  std::vector<uint8_t> rx_buf_;       // TCP reassembly buffer (task-only)
  std::unique_ptr<proto::Codec> codec_;  // per-connection; holds 3.4/3.5 session key
  uint32_t seqno_{1};
  bool negotiated_{false};
  // Task-only. desired_ survives reconnects (what we want); confirmed_ is what
  // the plug last reported and is cleared on disconnect (state becomes unknown).
  std::map<uint8_t, bool> desired_;
  std::map<uint8_t, bool> confirmed_;
  uint32_t last_assert_{0};
  // Release requests from the main loop. Atomic (not the bounded command queue)
  // so a handover's "stop controlling this plug" can never be dropped and is
  // applied even while disconnected. release_bits_[dp>>6] bit (dp&63) marks one
  // DP to drop from desired_.
  std::atomic<bool> release_all_pending_{false};
  std::atomic<uint64_t> release_bits_[4]{};
#endif

  struct Listener {
    uint8_t dp;
    std::function<void(const DpValue &)> cb;
  };
  std::vector<Listener> listeners_;
  std::vector<std::function<void(bool)>> online_callbacks_;

  std::string host_;
  uint16_t port_{6668};
  std::string device_id_;
  std::string local_key_;
  proto::Version version_{proto::Version::V33};
  uint32_t poll_interval_{15000};

  std::atomic<bool> online_{false};
  std::atomic<bool> pending_writes_{false};
  bool last_published_online_{false};
};

// Action: tuya_lan.release — drop the stored write intent for a hub, so it stops
// re-asserting a DP after control is handed back to another controller. With a
// datapoint it releases one DP, otherwise all of them.
template<typename... Ts> class TuyaLanReleaseAction : public Action<Ts...> {
 public:
  explicit TuyaLanReleaseAction(TuyaLanDevice *parent) : parent_(parent) {}
  void set_datapoint(uint8_t dp) {
    this->dp_ = dp;
    this->has_dp_ = true;
  }
  void play(Ts... x) override {
    if (this->has_dp_)
      this->parent_->release_dp(this->dp_);
    else
      this->parent_->release_all();
  }

 protected:
  TuyaLanDevice *parent_;
  uint8_t dp_{0};
  bool has_dp_{false};
};

}  // namespace tuya_lan
}  // namespace esphome
