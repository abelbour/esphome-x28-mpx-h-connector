import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import CONF_ID

DEPENDENCIES = ["x28_alarm"]

x28_alarm_ns = cg.esphome_ns.namespace("x28_alarm")
X28Alarm = x28_alarm_ns.class_("X28Alarm")
X28ActionButton = x28_alarm_ns.class_("X28ActionButton", button.Button)

CONF_X28_ALARM_ID = "x28_alarm_id"
CONF_ACTION = "action"

PANIC = "PANIC"
PANIC_LONG = "PANIC_LONG"
FIRE = "FIRE"
FIRE_LONG = "FIRE_LONG"

ACTION_CODES = {
    PANIC: 0x80EA,
    PANIC_LONG: (0x80EA, 0x012F),
    FIRE: 0x00F9,
    FIRE_LONG: (0x00F9, 0x0119),
}

CONFIG_SCHEMA = button.BUTTON_SCHEMA.extend({
    cv.GenerateID(): cv.declare_id(X28ActionButton),
    cv.GenerateID(CONF_X28_ALARM_ID): cv.use_id(X28Alarm),
    cv.Required(CONF_ACTION): cv.one_of(*ACTION_CODES.keys(), upper=True),
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_X28_ALARM_ID])
    var = cg.new_Pvariable(config[CONF_ID])
    await button.register_button(var, config)
    cg.add(var.set_parent(parent))
    code = ACTION_CODES[config[CONF_ACTION]]
    if isinstance(code, tuple):
        cg.add(var.set_code(code[0]))
        cg.add(var.set_long_code(code[1]))
    else:
        cg.add(var.set_code(code))
