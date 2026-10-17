#include "tuya_lan_binary_sensor.h"
#include "esphome/core/log.h"

namespace esphome {
namespace tuya_lan {

static const char *const TAG = "tuya_lan.binary_sensor";

void TuyaLanBinarySensor::setup() {
  this->publish_initial_state(this->parent_->is_online());
  this->parent_->add_online_callback([this](bool online) { this->publish_state(online); });
}

void TuyaLanBinarySensor::dump_config() { LOG_BINARY_SENSOR("", "Tuya LAN Online", this); }

}  // namespace tuya_lan
}  // namespace esphome
