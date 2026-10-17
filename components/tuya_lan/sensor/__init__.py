import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import CONF_ID

from .. import CONF_TUYA_LAN_ID, TuyaLanDevice, tuya_lan_ns

DEPENDENCIES = ["tuya_lan"]

CONF_DATAPOINT = "datapoint"

TuyaLanSensor = tuya_lan_ns.class_("TuyaLanSensor", sensor.Sensor, cg.Component)

CONFIG_SCHEMA = (
    sensor.sensor_schema(TuyaLanSensor)
    .extend(
        {
            cv.GenerateID(CONF_TUYA_LAN_ID): cv.use_id(TuyaLanDevice),
            cv.Required(CONF_DATAPOINT): cv.uint8_t,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await sensor.register_sensor(var, config)
    parent = await cg.get_variable(config[CONF_TUYA_LAN_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_datapoint(config[CONF_DATAPOINT]))
