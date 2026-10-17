#pragma once

#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "../tuya_lan.h"

namespace esphome {
namespace tuya_lan {

// Reports whether the plug is reachable on the LAN. Build alarms on this: if it
// goes off, the thermostat has lost control of the heater.
class TuyaLanBinarySensor : public binary_sensor::BinarySensor, public Component {
 public:
  void setup() override;
  void dump_config() override;
  void set_parent(TuyaLanDevice *parent) { this->parent_ = parent; }

 protected:
  TuyaLanDevice *parent_{nullptr};
};

}  // namespace tuya_lan
}  // namespace esphome
