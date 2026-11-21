"""Bus condiviso con il pannello S-Touch: una sola interfaccia RS485.

L'ESP ascolta il traffico tra pannello e unità, risponde a modbus_controller
dalla cache quando i valori sono freschi e, per il resto, trasmette solo nelle
pause del pannello (subito dopo una risposta dell'unità al pannello) con
transazioni brevi. Il componente è anche una UART virtuale: un blocco `modbus:`
con `uart_id` uguale all'id di salda_bus.
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

salda_bus_ns = cg.esphome_ns.namespace("salda_bus")
SaldaBus = salda_bus_ns.class_("SaldaBus", cg.PollingComponent, uart.UARTComponent)

CONF_BUS_UART_ID = "bus_uart_id"
CONF_CACHE_MAX_AGE = "cache_max_age"
CONF_BUDGET = "budget"
CONF_SLOT_WINDOW = "slot_window"
CONF_IDLE_TIME = "idle_time"
CONF_RESPONSE_TIMEOUT = "response_timeout"
CONF_LOCAL_EXPIRE = "local_expire"
CONF_MAX_REGISTERS = "max_registers"
CONF_LOG_POLLS = "log_polls"
CONF_PANEL_CONNECTED = "panel_connected"
CONF_PANEL_TRANSACTIONS = "panel_transactions"
CONF_GAP_MIN = "gap_min"
CONF_CACHE_HITS = "cache_hits"
CONF_OWN_TRANSACTIONS = "own_transactions"
CONF_RETRIES = "retries"
CONF_FAILURES = "failures"

_counter = sensor.sensor_schema(
    accuracy_decimals=0,
    state_class=STATE_CLASS_TOTAL_INCREASING,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SaldaBus),
        cv.Required(CONF_BUS_UART_ID): cv.use_id(uart.UARTComponent),
        cv.Optional(CONF_CACHE_MAX_AGE, default="10s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_BUDGET, default="30ms"): cv.positive_time_period_microseconds,
        cv.Optional(CONF_SLOT_WINDOW, default="12ms"): cv.positive_time_period_microseconds,
        cv.Optional(CONF_IDLE_TIME, default="600ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_RESPONSE_TIMEOUT, default="80ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_LOCAL_EXPIRE, default="4s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_MAX_REGISTERS, default=4): cv.int_range(min=1, max=32),
        cv.Optional(CONF_LOG_POLLS, default=False): cv.boolean,
        cv.Optional(CONF_PANEL_CONNECTED): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_PANEL_TRANSACTIONS): sensor.sensor_schema(
            unit_of_measurement="/min",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:swap-horizontal",
        ),
        cv.Optional(CONF_GAP_MIN): sensor.sensor_schema(
            unit_of_measurement=UNIT_MILLISECOND,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:timer-sand-empty",
        ),
        cv.Optional(CONF_CACHE_HITS): _counter,
        cv.Optional(CONF_OWN_TRANSACTIONS): _counter,
        cv.Optional(CONF_RETRIES): _counter,
        cv.Optional(CONF_FAILURES): _counter,
    }
).extend(cv.polling_component_schema("60s"))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    bus = await cg.get_variable(config[CONF_BUS_UART_ID])
    cg.add(var.set_bus_uart(bus))
    cg.add(var.set_cache_max_age(config[CONF_CACHE_MAX_AGE]))
    cg.add(var.set_budget(config[CONF_BUDGET]))
    cg.add(var.set_slot_window(config[CONF_SLOT_WINDOW]))
    cg.add(var.set_idle_time(config[CONF_IDLE_TIME]))
    cg.add(var.set_response_timeout(config[CONF_RESPONSE_TIMEOUT]))
    cg.add(var.set_local_expire(config[CONF_LOCAL_EXPIRE]))
    cg.add(var.set_max_registers(config[CONF_MAX_REGISTERS]))
    cg.add(var.set_log_polls(config[CONF_LOG_POLLS]))

    for key, setter in (
        (CONF_PANEL_TRANSACTIONS, "set_panel_transactions_sensor"),
        (CONF_GAP_MIN, "set_gap_min_sensor"),
        (CONF_CACHE_HITS, "set_cache_hits_sensor"),
        (CONF_OWN_TRANSACTIONS, "set_own_transactions_sensor"),
        (CONF_RETRIES, "set_retries_sensor"),
        (CONF_FAILURES, "set_failures_sensor"),
    ):
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(s))

    if CONF_PANEL_CONNECTED in config:
        b = await binary_sensor.new_binary_sensor(config[CONF_PANEL_CONNECTED])
        cg.add(var.set_panel_connected_sensor(b))
