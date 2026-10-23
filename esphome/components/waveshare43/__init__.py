import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import i2c
from esphome.const import CONF_ID

DEPENDENCIES = ["i2c"]
waveshare43_ns = cg.esphome_ns.namespace("waveshare43")
Control = waveshare43_ns.class_("Control", cg.Component, i2c.I2CDevice)
Finalizer = waveshare43_ns.class_("Finalizer", cg.Component)
CONFIG_SCHEMA = cv.Schema({cv.GenerateID(): cv.declare_id(Control), cv.GenerateID("finalizer_id"): cv.declare_id(Finalizer)}).extend(cv.COMPONENT_SCHEMA).extend(i2c.i2c_device_schema(0x45))
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    finalizer = cg.new_Pvariable(config["finalizer_id"], var)
    await cg.register_component(finalizer, {})
