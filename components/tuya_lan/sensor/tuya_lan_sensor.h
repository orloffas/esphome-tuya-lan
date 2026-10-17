#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "../tuya_lan.h"

namespace esphome {
namespace tuya_lan {

// A numeric sensor bound to one DP (e.g. current, power, voltage). Raw DP units
// are device-specific; apply `filters: [multiply: ...]` in YAML to scale them.
class TuyaLanSensor : public sensor::Sensor, public Component {
 public:
  void setup() override;
  void dump_config() override;
  void set_parent(TuyaLanDevice *parent) { this->parent_ = parent; }
  void set_datapoint(uint8_t dp) { this->dp_ = dp; }

 protected:
  TuyaLanDevice *parent_{nullptr};
  uint8_t dp_{0};
};

}  // namespace tuya_lan
}  // namespace esphome
