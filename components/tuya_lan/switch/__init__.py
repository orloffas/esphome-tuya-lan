import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv

from .. import CONF_TUYA_LAN_ID, TuyaLanDevice, tuya_lan_ns

DEPENDENCIES = ["tuya_lan"]

CONF_DATAPOINT = "datapoint"

TuyaLanSwitch = tuya_lan_ns.class_("TuyaLanSwitch", switch.Switch, cg.Component)

CONFIG_SCHEMA = (
    switch.switch_schema(TuyaLanSwitch)
    .extend(
        {
            cv.GenerateID(CONF_TUYA_LAN_ID): cv.use_id(TuyaLanDevice),
            cv.Required(CONF_DATAPOINT): cv.uint8_t,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_TUYA_LAN_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_datapoint(config[CONF_DATAPOINT]))
