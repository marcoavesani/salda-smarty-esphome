#pragma once

#include <utility>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/uart/uart_component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"

#include "proxy_core.h"

namespace esphome {
namespace salda_proxy {

/// Proxy Modbus RTU S-Touch <-> unità, e UART virtuale per il componente `modbus`.
class SaldaProxy : public PollingComponent, public uart::UARTComponent {
 public:
  void set_panel_uart(uart::UARTComponent *u) { this->panel_ = u; }
  void set_unit_uart(uart::UARTComponent *u) { this->unit_ = u; }
  void set_response_timeout(uint32_t ms) { this->core_.response_timeout_ms = ms; }
  void set_local_expire(uint32_t ms) { this->core_.local_expire_ms = ms; }
  void set_frame_gap(uint32_t ms) { this->core_.gap_ms = ms; }

  void set_panel_requests_sensor(sensor::Sensor *s) { this->panel_requests_ = s; }
  void set_local_requests_sensor(sensor::Sensor *s) { this->local_requests_ = s; }
  void set_timeouts_sensor(sensor::Sensor *s) { this->timeouts_ = s; }
  void set_errors_sensor(sensor::Sensor *s) { this->errors_ = s; }
  void set_panel_latency_sensor(sensor::Sensor *s) { this->panel_latency_ = s; }
  void set_panel_connected_sensor(binary_sensor::BinarySensor *s) { this->panel_connected_ = s; }
  void set_unit_connected_sensor(binary_sensor::BinarySensor *s) { this->unit_connected_ = s; }

  const ProxyStats &get_stats() const { return this->core_.stats; }

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
  // I tipi di ritorno sono cambiati tra le versioni di ESPHome (int/void fino al
  // 2025.x, size_t/UARTFlushResult dal 2026.x): li ricaviamo dalla classe base.
  using AvailableT = decltype(std::declval<uart::UARTComponent &>().available());
  using FlushT = decltype(std::declval<uart::UARTComponent &>().flush());
  AvailableT available() override;
  // Niente da attendere: i byte passano subito al core. FlushT() vale void oppure
  // UART_FLUSH_RESULT_SUCCESS (primo valore dell'enum).
  FlushT flush() override { return FlushT(); }
#if defined(USE_ESP8266) || defined(USE_ESP32)
  void load_settings(bool dump_config) override {}
#endif

 protected:
  void check_logger_conflict() override {}
  void pump_rx_(uart::UARTComponent *port, bool from_panel, uint32_t now);
  void transmit_(uart::UARTComponent *port, std::vector<uint8_t> &frame);

  uart::UARTComponent *panel_{nullptr};
  uart::UARTComponent *unit_{nullptr};
  ProxyCore core_;
  HighFrequencyLoopRequester high_freq_;

  sensor::Sensor *panel_requests_{nullptr};
  sensor::Sensor *local_requests_{nullptr};
  sensor::Sensor *timeouts_{nullptr};
  sensor::Sensor *errors_{nullptr};
  sensor::Sensor *panel_latency_{nullptr};
  binary_sensor::BinarySensor *panel_connected_{nullptr};
  binary_sensor::BinarySensor *unit_connected_{nullptr};
};

}  // namespace salda_proxy
}  // namespace esphome
