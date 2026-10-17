#include "tuya_lan_sensor.h"
#include "esphome/core/log.h"

namespace esphome {
namespace tuya_lan {

static const char *const TAG = "tuya_lan.sensor";

void TuyaLanSensor::setup() {
  this->parent_->register_dp_listener(this->dp_, [this](const DpValue &v) {
    this->publish_state(v.type == DpValue::BOOL ? (v.b ? 1.0f : 0.0f) : (float) v.i);
  });
}

void TuyaLanSensor::dump_config() { LOG_SENSOR("", "Tuya LAN Sensor", this); }

}  // namespace tuya_lan
}  // namespace esphome
