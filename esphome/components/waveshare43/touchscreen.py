import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import touchscreen, i2c
from esphome.const import CONF_ID
from . import waveshare43_ns
Touch = waveshare43_ns.class_("Touch", touchscreen.Touchscreen, i2c.I2CDevice)
CONFIG_SCHEMA = touchscreen.TOUCHSCREEN_SCHEMA.extend({cv.GenerateID(): cv.declare_id(Touch)}).extend(i2c.i2c_device_schema(0x38))
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await touchscreen.register_touchscreen(var, config)
    await i2c.register_i2c_device(var, config)
