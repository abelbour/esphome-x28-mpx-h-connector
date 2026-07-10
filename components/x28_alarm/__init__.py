import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import binary_sensor
from esphome.const import (
    CONF_ID,
    CONF_CODE,
)

DEPENDENCIES = []
AUTO_LOAD = ["alarm_control_panel", "binary_sensor", "button", "text_sensor"]
MULTI_CONF = False

x28_alarm_ns = cg.esphome_ns.namespace("x28_alarm")
X28Alarm = x28_alarm_ns.class_("X28Alarm", cg.Component)
X28ModelEnum = x28_alarm_ns.enum("X28Model", is_class=True)

X28_MODEL_OPTIONS = {
    "AUTO": X28ModelEnum.AUTO,
    "N4-MPXH": X28ModelEnum.N4_MPXH,
    "N8-MPXH": X28ModelEnum.N8_MPXH,
    "N8F-MPXH": X28ModelEnum.N8F_MPXH,
    "N16-MPXH": X28ModelEnum.N16_MPXH,
    "N32-MPXH": X28ModelEnum.N32_MPXH,
    "N32F-MPXH": X28ModelEnum.N32F_MPXH,
    "9002-MPX": X28ModelEnum._9002_MPX,
    "9003-MPX": X28ModelEnum._9003_MPX,
    "9004-MPX": X28ModelEnum._9004_MPX,
}

CONF_RX_PIN = "rx_pin"
CONF_TX_PIN = "tx_pin"
CONF_MODEL = "model"
CONF_INSTALLER_CODE = "installer_code"
CONF_INVERT_RX = "invert_rx"
CONF_INVERT_TX = "invert_tx"
CONF_DEBUG = "debug"
CONF_SNIFFING = "sniffing"
CONF_SNIFFING_ENABLED = "enabled"
CONF_SNIFFING_THROTTLE_MS = "throttle_ms"
CONF_VIRTUAL_ZONES = "virtual_zones"
CONF_ZONE_DEBOUNCE_MS = "zone_debounce_ms"
CONF_ZONE = "zone"

CONF_SENSOR_ID = "sensor_id"
CONF_TRIGGER = "trigger"
CONF_ZONE_TYPE = "zone_type"
CONF_CLEAR_ON_CLOSE = "clear_on_close"
CONF_ZONE_CODES = "zone_codes"

CODE_VALIDATOR = cv.All(
    cv.string_strict,
    cv.Length(min=4, max=6),
    cv.is_number,
)

VIRTUAL_ZONE_SCHEMA = cv.Schema({
    cv.Required(CONF_ZONE): cv.int_range(1, 8),
    cv.Required(CONF_SENSOR_ID): cv.use_id(binary_sensor.BinarySensor),
    cv.Optional(CONF_TRIGGER, default="ON"): cv.one_of("ON", "OPEN", upper=True),
    cv.Optional(CONF_ZONE_TYPE, default="MPXH"): cv.one_of("MPXH", "WIRED", upper=True),
    cv.Optional(CONF_CLEAR_ON_CLOSE, default=True): cv.boolean,
})

SNIFFING_SCHEMA = cv.Schema({
    cv.Optional(CONF_SNIFFING_ENABLED, default=False): cv.boolean,
    cv.Optional(CONF_SNIFFING_THROTTLE_MS, default=1000): cv.positive_int,
})

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(X28Alarm),
    cv.Required(CONF_RX_PIN): pins.gpio_input_pin_schema,
    cv.Required(CONF_TX_PIN): pins.gpio_output_pin_schema,
    cv.Required(CONF_CODE): CODE_VALIDATOR,
    cv.Optional(CONF_INSTALLER_CODE, default="467825"): cv.All(
        cv.string_strict, cv.Length(min=6, max=6), cv.is_number,
    ),
    cv.Optional(CONF_MODEL, default="AUTO"): cv.enum(X28_MODEL_OPTIONS, upper=True),
    cv.Optional(CONF_INVERT_RX, default=True): cv.boolean,
    cv.Optional(CONF_INVERT_TX, default=True): cv.boolean,
    cv.Optional(CONF_DEBUG, default=False): cv.boolean,
    cv.Optional(CONF_SNIFFING, default={}): SNIFFING_SCHEMA,
    cv.Optional(CONF_VIRTUAL_ZONES, default=[]): cv.ensure_list(VIRTUAL_ZONE_SCHEMA),
    cv.Optional(CONF_ZONE_DEBOUNCE_MS, default=500): cv.positive_int,
    cv.Optional(CONF_ZONE_CODES, default={}): cv.Schema({
        cv.int_range(1, 8): cv.hex_uint16_t,
    }),
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    rx_pin = await cg.gpio_pin_expression(config[CONF_RX_PIN])
    cg.add(var.set_rx_pin(rx_pin))

    tx_pin = await cg.gpio_pin_expression(config[CONF_TX_PIN])
    cg.add(var.set_tx_pin(tx_pin))

    cg.add(var.set_code(config[CONF_CODE]))
    cg.add(var.set_installer_code(config[CONF_INSTALLER_CODE]))
    cg.add(var.set_model(config[CONF_MODEL]))
    cg.add(var.set_invert_rx(config[CONF_INVERT_RX]))
    cg.add(var.set_invert_tx(config[CONF_INVERT_TX]))
    cg.add(var.set_debug(config[CONF_DEBUG]))

    sniffing = config[CONF_SNIFFING]
    cg.add(var.set_sniffing_enabled(sniffing[CONF_SNIFFING_ENABLED]))
    cg.add(var.set_sniffing_throttle(sniffing[CONF_SNIFFING_THROTTLE_MS]))

    cg.add(var.set_zone_debounce_ms(config[CONF_ZONE_DEBOUNCE_MS]))

    for zone_num, code in config.get(CONF_ZONE_CODES, {}).items():
        cg.add(var.set_zone_code_override(zone_num, code))

    for vz_conf in config[CONF_VIRTUAL_ZONES]:
        zone = vz_conf[CONF_ZONE]
        sensor = await cg.get_variable(vz_conf[CONF_SENSOR_ID])
        trigger = vz_conf[CONF_TRIGGER]
        zone_type = vz_conf[CONF_ZONE_TYPE]
        clear_on_close = vz_conf[CONF_CLEAR_ON_CLOSE]

        zone_offsets = {
            "MPXH": [0x1615, 0x1623, 0x9630, 0x1640, 0x1655, 0x1663, 0x9670, 0x1680],
            "WIRED": [0xB08A, 0xB045, 0xB026, 0xB010, 0xB008, 0xB1D2, 0xB05C, 0xB134],
        }

        packet_code = zone_offsets[zone_type][zone - 1]

        cg.add(var.add_virtual_zone(zone, packet_code, clear_on_close, sensor))
