"""Desktop-only adapter for shared PAX LVGL launch callbacks."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.types import ConfigType

namespace = cg.esphome_ns.namespace("pax_desktop")
Runtime = namespace.class_("Runtime", cg.Component)
CONFIG_SCHEMA = cv.All(
    cv.Schema({cv.GenerateID(): cv.declare_id(Runtime)}).extend(cv.COMPONENT_SCHEMA),
    cv.only_on("host"),
)

async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
