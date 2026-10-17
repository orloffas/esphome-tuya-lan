import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PORT, CONF_VERSION

CODEOWNERS = ["@orloffas"]
DEPENDENCIES = ["network"]
AUTO_LOAD = ["json"]
MULTI_CONF = True

CONF_HOST = "host"
CONF_DEVICE_ID = "device_id"
CONF_LOCAL_KEY = "local_key"
CONF_POLL_INTERVAL = "poll_interval"
CONF_TUYA_LAN_ID = "tuya_lan_id"

tuya_lan_ns = cg.esphome_ns.namespace("tuya_lan")
TuyaLanDevice = tuya_lan_ns.class_("TuyaLanDevice", cg.Component)

Version = tuya_lan_ns.namespace("proto").enum("Version", is_class=True)
VERSIONS = {
    "3.3": Version.V33,
    "3.4": Version.V34,
    "3.5": Version.V35,
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(TuyaLanDevice),
        cv.Required(CONF_HOST): cv.string,
        cv.Required(CONF_DEVICE_ID): cv.string,
        cv.Required(CONF_LOCAL_KEY): cv.All(cv.string, cv.Length(min=16, max=16)),
        cv.Optional(CONF_VERSION, default="3.3"): cv.one_of(*VERSIONS),
        cv.Optional(CONF_PORT, default=6668): cv.port,
        cv.Optional(
            CONF_POLL_INTERVAL, default="15s"
        ): cv.positive_time_period_milliseconds,
    }
).extend(cv.COMPONENT_SCHEMA)

# mbedtls GCM and the FreeRTOS network task are ESP32-only; fail config
# validation with a clear message rather than a cryptic build error elsewhere.
CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, cv.only_on_esp32)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_host(config[CONF_HOST]))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_device_id(config[CONF_DEVICE_ID]))
    cg.add(var.set_local_key(config[CONF_LOCAL_KEY]))
    cg.add(var.set_version(VERSIONS[config[CONF_VERSION]]))
    cg.add(var.set_poll_interval(config[CONF_POLL_INTERVAL]))
