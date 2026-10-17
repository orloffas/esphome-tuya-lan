#include "tuya_lan_switch.h"
#include "esphome/core/log.h"

namespace esphome {
namespace tuya_lan {

static const char *const TAG = "tuya_lan.switch";

void TuyaLanSwitch::setup() {
  this->parent_->register_dp_listener(this->dp_, [this](const DpValue &v) {
    this->publish_state(v.type == DpValue::BOOL ? v.b : v.i != 0);
  });
}

void TuyaLanSwitch::write_state(bool state) {
  // Do not publish here: the hub publishes once the plug reports the new state.
  this->parent_->set_bool_dp(this->dp_, state);
}

void TuyaLanSwitch::dump_config() { LOG_SWITCH("", "Tuya LAN Switch", this); }

}  // namespace tuya_lan
}  // namespace esphome
