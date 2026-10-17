#pragma once

#include "esphome/core/component.h"
#include "esphome/components/switch/switch.h"
#include "../tuya_lan.h"

namespace esphome {
namespace tuya_lan {

// A switch bound to one boolean DP. State is published only when the plug
// confirms it (never optimistically), so the reported state always matches the
// hardware — a hard requirement for driving a thermostat from it.
class TuyaLanSwitch : public switch_::Switch, public Component {
 public:
  void setup() override;
  void dump_config() override;
  void set_parent(TuyaLanDevice *parent) { this->parent_ = parent; }
  void set_datapoint(uint8_t dp) { this->dp_ = dp; }

 protected:
  void write_state(bool state) override;

  TuyaLanDevice *parent_{nullptr};
  uint8_t dp_{0};
};

}  // namespace tuya_lan
}  // namespace esphome
