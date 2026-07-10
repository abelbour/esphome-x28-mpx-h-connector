import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_ID

DEPENDENCIES = ["x28_alarm"]

x28_alarm_ns = cg.esphome_ns.namespace("x28_alarm")
X28Alarm = x28_alarm_ns.class_("X28Alarm")
X28SnifferSwitch = x28_alarm_ns.class_("X28SnifferSwitch", switch.Switch, cg.Component)

CONF_X28_ALARM_ID = "x28_alarm_id"

CONFIG_SCHEMA = switch.SWITCH_SCHEMA.extend({
    cv.GenerateID(): cv.declare_id(X28SnifferSwitch),
    cv.GenerateID(CONF_X28_ALARM_ID): cv.use_id(X28Alarm),
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_X28_ALARM_ID])
    var = cg.new_Pvariable(config[CONF_ID])
    await switch.register_switch(var, config)
    await cg.register_component(var, config)
    cg.add(var.set_parent(parent))
    cg.add(parent.set_sniffer_switch(var))
