import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32, pax_loader, socket
from esphome.const import CONF_ID, CONF_PORT
DEPENDENCIES = ["pax_loader", "network"]
ns = cg.esphome_ns.namespace("pax_web")
PaxWeb = ns.class_("PaxWeb", cg.Component)
CONFIG_SCHEMA = cv.All(cv.Schema({
    cv.GenerateID(): cv.declare_id(PaxWeb),
    cv.Required("pax_loader_id"): cv.use_id(pax_loader.PaxLoader),
    cv.Optional(CONF_PORT, default=8080): cv.port,
}).extend(cv.COMPONENT_SCHEMA), cv.only_on_esp32,
    socket.consume_sockets(4, "pax_web"),
    socket.consume_sockets(1, "pax_web", socket.SocketType.TCP_LISTEN))
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_loader(await cg.get_variable(config["pax_loader_id"])))
    cg.add(var.set_port(config[CONF_PORT]))
    esp32.include_builtin_idf_component("esp_http_server")
