"""Proxy Modbus RTU tra pannello S-Touch e unità Salda (MCB/miniMCB).

L'ESP sta in mezzo al cavo del pannello con due porte RS485:
- porta "panel": l'S-Touch (master) vede l'ESP come se fosse l'unità;
- porta "unit": l'ESP è l'unico master verso l'unità.
Il componente è anche una UART virtuale: un blocco `modbus:` con `uart_id`
uguale all'id del proxy fa passare le richieste di modbus_controller (Home
Assistant) negli spazi lasciati liberi dal pannello.
"""

import esphome.codegen as cg
from esphome.components import binary_sensor, sensor, uart
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_CONNECTIVITY,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_MILLISECOND,
)

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "binary_sensor", "salda_common"]
MULTI_CONF = False

salda_proxy_ns = cg.esphome_ns.namespace("salda_proxy")
SaldaProxy = salda_proxy_ns.class_(
    "SaldaProxy", cg.PollingComponent, uart.UARTComponent
)

CONF_PANEL_UART_ID = "panel_uart_id"
CONF_UNIT_UART_ID = "unit_uart_id"
CONF_RESPONSE_TIMEOUT = "response_timeout"
CONF_LOCAL_EXPIRE = "local_expire"
CONF_FRAME_GAP = "frame_gap"
CONF_PANEL_REQUESTS = "panel_requests"
CONF_LOCAL_REQUESTS = "local_requests"
CONF_TIMEOUTS = "timeouts"
CONF_ERRORS = "errors"
CONF_PANEL_LATENCY = "panel_latency"
CONF_PANEL_CONNECTED = "panel_connected"
CONF_UNIT_CONNECTED = "unit_connected"

_counter = sensor.sensor_schema(
    accuracy_decimals=0,
    state_class=STATE_CLASS_TOTAL_INCREASING,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
)
_connectivity = binary_sensor.binary_sensor_schema(
    device_class=DEVICE_CLASS_CONNECTIVITY,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SaldaProxy),
        cv.Required(CONF_PANEL_UART_ID): cv.use_id(uart.UARTComponent),
        cv.Required(CONF_UNIT_UART_ID): cv.use_id(uart.UARTComponent),
        cv.Optional(
            CONF_RESPONSE_TIMEOUT, default="500ms"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(
            CONF_LOCAL_EXPIRE, default="1200ms"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_FRAME_GAP, default="20ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_PANEL_REQUESTS): _counter,
        cv.Optional(CONF_LOCAL_REQUESTS): _counter,
        cv.Optional(CONF_TIMEOUTS): _counter,
        cv.Optional(CONF_ERRORS): _counter,
        cv.Optional(CONF_PANEL_LATENCY): sensor.sensor_schema(
            unit_of_measurement=UNIT_MILLISECOND,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:timer-outline",
        ),
        cv.Optional(CONF_PANEL_CONNECTED): _connectivity,
        cv.Optional(CONF_UNIT_CONNECTED): _connectivity,
    }
).extend(cv.polling_component_schema("30s"))


def _validate_ports(config):
    if config[CONF_PANEL_UART_ID] == config[CONF_UNIT_UART_ID]:
        raise cv.Invalid("panel_uart_id e unit_uart_id devono essere due UART diverse")
    return config


CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, _validate_ports)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    panel = await cg.get_variable(config[CONF_PANEL_UART_ID])
    unit = await cg.get_variable(config[CONF_UNIT_UART_ID])
    cg.add(var.set_panel_uart(panel))
    cg.add(var.set_unit_uart(unit))
    cg.add(var.set_response_timeout(config[CONF_RESPONSE_TIMEOUT]))
    cg.add(var.set_local_expire(config[CONF_LOCAL_EXPIRE]))
    cg.add(var.set_frame_gap(config[CONF_FRAME_GAP]))

    for key, setter in (
        (CONF_PANEL_REQUESTS, "set_panel_requests_sensor"),
        (CONF_LOCAL_REQUESTS, "set_local_requests_sensor"),
        (CONF_TIMEOUTS, "set_timeouts_sensor"),
        (CONF_ERRORS, "set_errors_sensor"),
        (CONF_PANEL_LATENCY, "set_panel_latency_sensor"),
    ):
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(s))

    for key, setter in (
        (CONF_PANEL_CONNECTED, "set_panel_connected_sensor"),
        (CONF_UNIT_CONNECTED, "set_unit_connected_sensor"),
    ):
        if key in config:
            b = await binary_sensor.new_binary_sensor(config[key])
            cg.add(getattr(var, setter)(b))
