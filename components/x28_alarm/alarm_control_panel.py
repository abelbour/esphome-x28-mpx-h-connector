import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import alarm_control_panel
from esphome.const import CONF_ID

DEPENDENCIES = ["x28_alarm"]

x28_alarm_ns = cg.esphome_ns.namespace("x28_alarm")
X28Alarm = x28_alarm_ns.class_("X28Alarm")
X28AlarmControlPanel = x28_alarm_ns.class_(
    "X28AlarmControlPanel", alarm_control_panel.AlarmControlPanel
)

CONF_X28_ALARM_ID = "x28_alarm_id"

CONFIG_SCHEMA = alarm_control_panel.ALARM_CONTROL_PANEL_SCHEMA.extend({
    cv.GenerateID(): cv.declare_id(X28AlarmControlPanel),
    cv.GenerateID(CONF_X28_ALARM_ID): cv.use_id(X28Alarm),
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_X28_ALARM_ID])
    var = cg.new_Pvariable(config[CONF_ID])
    await alarm_control_panel.register_alarm_control_panel(var, config)
    cg.add(var.set_parent(parent))
    cg.add(parent.set_alarm_control_panel(var))
