import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv

from .. import CONF_TUYA_LAN_ID, TuyaLanDevice, tuya_lan_ns

DEPENDENCIES = ["tuya_lan"]

TuyaLanBinarySensor = tuya_lan_ns.class_(
    "TuyaLanBinarySensor", binary_sensor.BinarySensor, cg.Component
)

CONFIG_SCHEMA = binary_sensor.binary_sensor_schema(
    TuyaLanBinarySensor,
    device_class="connectivity",
).extend(
    {
        cv.GenerateID(CONF_TUYA_LAN_ID): cv.use_id(TuyaLanDevice),
    }
)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_TUYA_LAN_ID])
    cg.add(var.set_parent(parent))
