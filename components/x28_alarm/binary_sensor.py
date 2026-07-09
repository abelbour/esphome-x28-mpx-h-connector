import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import (
    CONF_ID,
    CONF_NAME,
    CONF_ZONE,
)

DEPENDENCIES = ["x28_alarm"]

x28_alarm_ns = cg.esphome_ns.namespace("x28_alarm")
X28Alarm = x28_alarm_ns.class_("X28Alarm")

CONF_X28_ALARM_ID = "x28_alarm_id"

ZONE_ESTOY = "ESTOY"

CONFIG_SCHEMA = binary_sensor.BINARY_SENSOR_SCHEMA.extend({
    cv.GenerateID(): cv.declare_id(binary_sensor.BinarySensor),
    cv.GenerateID(CONF_X28_ALARM_ID): cv.use_id(X28Alarm),
    cv.Required(CONF_ZONE): cv.Any(
        cv.one_of(ZONE_ESTOY, upper=True),
        cv.int_range(1, 32),
    ),
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_X28_ALARM_ID])
    var = cg.new_Pvariable(config[CONF_ID])
    await binary_sensor.register_binary_sensor(var, config)

    zone = config[CONF_ZONE]
    if zone == ZONE_ESTOY:
        cg.add(parent.set_estoy_sensor(var))
    else:
        cg.add(parent.set_zone_sensor(zone, var))
