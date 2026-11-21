#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/uart/uart_component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"

#include "bus_core.h"

namespace esphome {
namespace salda_bus {

/// Bus condiviso con l'S-Touch e UART virtuale per il componente `modbus`.
class SaldaBus : public PollingComponent, public uart::UARTComponent {
 public:
  void set_bus_uart(uart::UARTComponent *u) { this->bus_ = u; }
  void set_cache_max_age(uint32_t ms) { this->core_.max_age_us = ms * 1000; }
  void set_budget(uint32_t us) { this->core_.budget_us = us; }
  void set_slot_window(uint32_t us) { this->core_.slot_window_us = us; }
  void set_idle_time(uint32_t ms) { this->core_.idle_us = ms * 1000; }
  void set_response_timeout(uint32_t ms) { this->core_.response_timeout_us = ms * 1000; }
  void set_local_expire(uint32_t ms) { this->core_.local_expire_us = ms * 1000; }
  void set_max_registers(uint16_t n) { this->core_.max_regs = n; }
  void set_log_polls(bool v) { this->log_polls_ = v; }

  void set_panel_connected_sensor(binary_sensor::BinarySensor *s) { this->panel_connected_ = s; }
  void set_panel_transactions_sensor(sensor::Sensor *s) { this->panel_transactions_ = s; }
  void set_gap_min_sensor(sensor::Sensor *s) { this->gap_min_ = s; }
  void set_cache_hits_sensor(sensor::Sensor *s) { this->cache_hits_ = s; }
  void set_own_transactions_sensor(sensor::Sensor *s) { this->own_transactions_ = s; }
  void set_retries_sensor(sensor::Sensor *s) { this->retries_ = s; }
  void set_failures_sensor(sensor::Sensor *s) { this->failures_ = s; }

  /// Scrive nel log l'elenco delle richieste del pannello.
  void dump_polls();
  /// Scrive nel log tutti i valori in cache.
  void dump_registers();

  // Component
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  // Dopo le UART reali (BUS), prima di modbus (BUS - 1).
  float get_setup_priority() const override { return setup_priority::BUS - 0.5f; }

  // UARTComponent virtuale (lato modbus_controller)
  void write_array(const uint8_t *data, size_t len) override;
  bool peek_byte(uint8_t *data) override;
  bool read_array(uint8_t *data, size_t len) override;
  int available() override;
  void flush() override {}

 protected:
  void check_logger_conflict() override {}

  uart::UARTComponent *bus_{nullptr};
  BusCore core_;
  HighFrequencyLoopRequester high_freq_;
  bool log_polls_{false};

  binary_sensor::BinarySensor *panel_connected_{nullptr};
  sensor::Sensor *panel_transactions_{nullptr};
  sensor::Sensor *gap_min_{nullptr};
  sensor::Sensor *cache_hits_{nullptr};
  sensor::Sensor *own_transactions_{nullptr};
  sensor::Sensor *retries_{nullptr};
  sensor::Sensor *failures_{nullptr};
};

}  // namespace salda_bus
}  // namespace esphome
