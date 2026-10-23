import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import socket
from esphome.const import CONF_ID

DEPENDENCIES = ["api", "network"]
AUTO_LOAD = ["json"]
ns = cg.esphome_ns.namespace("pax_media_bridge")
PaxMediaBridge = ns.class_("PaxMediaBridge", cg.Component)
CONFIG_SCHEMA = cv.All(cv.Schema({
    cv.GenerateID(): cv.declare_id(PaxMediaBridge),
    cv.Required("relay_entity"): cv.entity_id,
    cv.Required("home_assistant_address"): cv.ipv4address,
}).extend(cv.COMPONENT_SCHEMA), cv.only_on_esp32,
    socket.consume_sockets(3, "pax_media_bridge"),
    socket.consume_sockets(1, "pax_media_bridge", socket.SocketType.TCP_LISTEN))

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_relay_entity(config["relay_entity"]))
    cg.add(var.set_home_assistant_address(str(config["home_assistant_address"])))
    for flag in ("USE_API_HOMEASSISTANT_SERVICES", "USE_API_HOMEASSISTANT_ACTION_RESPONSES",
                 "USE_API_HOMEASSISTANT_ACTION_RESPONSES_ERRORS", "USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON"):
        cg.add_define(flag)
