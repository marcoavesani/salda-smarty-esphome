#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"

#include "../salda_common/sniffer_core.h"

namespace esphome {
namespace salda_sniffer {

using salda_common::SnifferCore;
using salda_common::WindowStats;
using salda_common::GAP_BUCKETS;
using salda_common::GAP_BUCKETS_MS;

class SaldaSniffer : public PollingComponent, public uart::UARTDevice {
 public:
  void set_frame_gap(uint32_t ms) { this->core_.gap_us = ms * 1000; }
  void set_response_timeout(uint32_t ms) { this->core_.response_timeout_us = ms * 1000; }
  void set_log_registers(bool v) { this->log_registers_ = v; }

  void set_transactions_sensor(sensor::Sensor *s) { this->transactions_ = s; }
  void set_gap_min_sensor(sensor::Sensor *s) { this->gap_min_ = s; }
  void set_gap_avg_sensor(sensor::Sensor *s) { this->gap_avg_ = s; }
  void set_latency_max_sensor(sensor::Sensor *s) { this->latency_max_ = s; }
  void set_bus_load_sensor(sensor::Sensor *s) { this->bus_load_ = s; }
  void set_no_response_sensor(sensor::Sensor *s) { this->no_response_ = s; }
  void set_errors_sensor(sensor::Sensor *s) { this->errors_ = s; }
  void set_traffic_sensor(binary_sensor::BinarySensor *s) { this->traffic_ = s; }

  /// Scrive nel log l'elenco delle richieste viste (cosa legge il pannello e ogni quanto).
  void dump_polls();
  /// Scrive nel log tutti i valori di registri visti passare.
  void dump_registers();
  /// Azzera le statistiche cumulative.
  void reset_stats();

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  void log_writes_();

  SnifferCore core_;
  HighFrequencyLoopRequester high_freq_;
  bool log_registers_{false};
  uint32_t gap_min_since_boot_us_{0xFFFFFFFF};
  uint32_t latency_max_since_boot_us_{0};

  sensor::Sensor *transactions_{nullptr};
  sensor::Sensor *gap_min_{nullptr};
  sensor::Sensor *gap_avg_{nullptr};
  sensor::Sensor *latency_max_{nullptr};
  sensor::Sensor *bus_load_{nullptr};
  sensor::Sensor *no_response_{nullptr};
  sensor::Sensor *errors_{nullptr};
  binary_sensor::BinarySensor *traffic_{nullptr};
};

}  // namespace salda_sniffer
}  // namespace esphome
