import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import (
    CONF_ID,
    CONF_NAME,
)

DEPENDENCIES = ["x28_alarm"]

x28_alarm_ns = cg.esphome_ns.namespace("x28_alarm")
X28Alarm = x28_alarm_ns.class_("X28Alarm")

CONF_X28_ALARM_ID = "x28_alarm_id"

CONFIG_SCHEMA = text_sensor.TEXT_SENSOR_SCHEMA.extend({
    cv.GenerateID(): cv.declare_id(text_sensor.TextSensor),
    cv.GenerateID(CONF_X28_ALARM_ID): cv.use_id(X28Alarm),
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_X28_ALARM_ID])
    var = cg.new_Pvariable(config[CONF_ID])
    await text_sensor.register_text_sensor(var, config)
    cg.add(parent.set_sniffer_text_sensor(var))
