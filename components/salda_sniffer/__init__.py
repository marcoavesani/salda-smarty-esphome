"""Ascolto passivo del bus Modbus RTU tra S-Touch e unità Salda.

Non trasmette mai. Misura frequenza e contenuto delle interrogazioni del
pannello, le pause sul bus e la latenza dell'unità, e decodifica i valori
letti/scritti. Serve a decidere se un secondo master può inserirsi nelle pause.
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
    UNIT_PERCENT,
)

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "binary_sensor", "salda_common"]

salda_sniffer_ns = cg.esphome_ns.namespace("salda_sniffer")
SaldaSniffer = salda_sniffer_ns.class_(
    "SaldaSniffer", cg.PollingComponent, uart.UARTDevice
)

CONF_FRAME_GAP = "frame_gap"
CONF_RESPONSE_TIMEOUT = "response_timeout"
CONF_LOG_REGISTERS = "log_registers"
CONF_TRANSACTIONS = "transactions_per_minute"
CONF_GAP_MIN = "gap_min"
CONF_GAP_AVG = "gap_avg"
CONF_LATENCY_MAX = "latency_max"
CONF_BUS_LOAD = "bus_load"
CONF_NO_RESPONSE = "no_response"
CONF_ERRORS = "errors"
CONF_TRAFFIC = "traffic"


def _ms_sensor(icon):
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_MILLISECOND,
        accuracy_decimals=1,
        state_class=STATE_CLASS_MEASUREMENT,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        icon=icon,
    )


_counter = sensor.sensor_schema(
    accuracy_decimals=0,
    state_class=STATE_CLASS_TOTAL_INCREASING,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SaldaSniffer),
            cv.Optional(CONF_FRAME_GAP, default="20ms"): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_RESPONSE_TIMEOUT, default="500ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_LOG_REGISTERS, default=False): cv.boolean,
            cv.Optional(CONF_TRANSACTIONS): sensor.sensor_schema(
                unit_of_measurement="/min",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:swap-horizontal",
            ),
            cv.Optional(CONF_GAP_MIN): _ms_sensor("mdi:timer-sand-empty"),
            cv.Optional(CONF_GAP_AVG): _ms_sensor("mdi:timer-sand"),
            cv.Optional(CONF_LATENCY_MAX): _ms_sensor("mdi:timer-outline"),
            cv.Optional(CONF_BUS_LOAD): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                accuracy_decimals=1,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:gauge",
            ),
            cv.Optional(CONF_NO_RESPONSE): _counter,
            cv.Optional(CONF_ERRORS): _counter,
            cv.Optional(CONF_TRAFFIC): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_CONNECTIVITY,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    )
    .extend(cv.polling_component_schema("60s"))
    .extend(uart.UART_DEVICE_SCHEMA)
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema("salda_sniffer", require_rx=True)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_frame_gap(config[CONF_FRAME_GAP]))
    cg.add(var.set_response_timeout(config[CONF_RESPONSE_TIMEOUT]))
    cg.add(var.set_log_registers(config[CONF_LOG_REGISTERS]))

    for key, setter in (
        (CONF_TRANSACTIONS, "set_transactions_sensor"),
        (CONF_GAP_MIN, "set_gap_min_sensor"),
        (CONF_GAP_AVG, "set_gap_avg_sensor"),
        (CONF_LATENCY_MAX, "set_latency_max_sensor"),
        (CONF_BUS_LOAD, "set_bus_load_sensor"),
        (CONF_NO_RESPONSE, "set_no_response_sensor"),
        (CONF_ERRORS, "set_errors_sensor"),
    ):
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(s))

    if CONF_TRAFFIC in config:
        b = await binary_sensor.new_binary_sensor(config[CONF_TRAFFIC])
        cg.add(var.set_traffic_sensor(b))
