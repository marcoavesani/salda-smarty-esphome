#include "salda_bus.h"

#include <algorithm>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "../salda_common/sniffer_log.h"

namespace esphome {
namespace salda_bus {

static const char *const TAG = "salda_bus";
static const uint32_t PANEL_WINDOW_US = 10000000;

void SaldaBus::setup() {
  // La UART virtuale "eredita" i parametri della linea.
  this->set_baud_rate(this->bus_->get_baud_rate());
  this->set_data_bits(this->bus_->get_data_bits());
  this->set_stop_bits(this->bus_->get_stop_bits());
  this->set_parity(this->bus_->get_parity());
  this->set_rx_buffer_size(512);
  // Durata di un carattere: start + dati + parità + stop.
  uint32_t bits = 1 + this->bus_->get_data_bits() + this->bus_->get_stop_bits() +
                  (this->bus_->get_parity() == uart::UART_CONFIG_PARITY_NONE ? 0 : 1);
  uint32_t baud = std::max<uint32_t>(this->bus_->get_baud_rate(), 1);
  this->core_.byte_us = (bits * 1000000UL + baud - 1) / baud;
  // Gli slot durano pochi millisecondi: il loop deve girare di continuo.
  this->high_freq_.start();
}

void SaldaBus::loop() {
  uint8_t buf[64];
  int avail;
  while ((avail = this->bus_->available()) > 0) {
    size_t n = std::min<size_t>((size_t) avail, sizeof(buf));
    if (!this->bus_->read_array(buf, n))
      break;
    this->core_.on_bus_bytes(buf, n, micros());
  }
  this->core_.step(micros());
  if (!this->core_.to_bus.empty()) {
    ESP_LOGV(TAG, "-> %s", format_hex_pretty(this->core_.to_bus).c_str());
    this->bus_->write_array(this->core_.to_bus);
    this->bus_->flush();  // attende la fine della trasmissione
    this->core_.to_bus.clear();
  }
  if (!this->core_.sniffer.writes.empty())
    salda_common::log_writes(this->core_.sniffer, TAG);
}

// ---- UART virtuale ----------------------------------------------------------

void SaldaBus::write_array(const uint8_t *data, size_t len) {
  // Il componente modbus scrive ogni richiesta con una sola write_array.
  this->core_.queue_local(data, len, micros());
}

bool SaldaBus::peek_byte(uint8_t *data) {
  if (this->core_.to_local.empty())
    return false;
  *data = this->core_.to_local.front();
  return true;
}

bool SaldaBus::read_array(uint8_t *data, size_t len) {
  if (this->core_.to_local.size() < len)
    return false;
  for (size_t i = 0; i < len; i++) {
    data[i] = this->core_.to_local.front();
    this->core_.to_local.pop_front();
  }
  return true;
}

int SaldaBus::available() { return (int) this->core_.to_local.size(); }

// ---- Diagnostica ------------------------------------------------------------

void SaldaBus::update() {
  const uint32_t now = micros();
  auto &sn = this->core_.sniffer;
  auto &w = sn.window;
  const BusStats &s = this->core_.stats;
  uint32_t elapsed = std::max<uint32_t>(now - w.start_us, 1);
  float per_min = w.transactions * 60e6f / elapsed;

  if (this->panel_connected_ != nullptr)
    this->panel_connected_->publish_state(sn.seen_panel && now - sn.last_panel_us < PANEL_WINDOW_US);
  if (this->panel_transactions_ != nullptr)
    this->panel_transactions_->publish_state(per_min);
  if (this->gap_min_ != nullptr)
    this->gap_min_->publish_state(w.gap.count ? w.gap.min_us / 1000.0f : NAN);
  if (this->cache_hits_ != nullptr)
    this->cache_hits_->publish_state(s.served_from_cache);
  if (this->own_transactions_ != nullptr)
    this->own_transactions_->publish_state(s.chunks_sent);
  if (this->retries_ != nullptr)
    this->retries_->publish_state(s.chunk_retries);
  if (this->failures_ != nullptr)
    this->failures_->publish_state(s.local_failed + s.local_expired);

  ESP_LOGI(TAG, "Ultimi %.0f s: pannello %u transazioni (pausa min %.1f ms), nostre %u | richieste HA: %u dalla cache, "
                "%u dal bus | ritrasmissioni %u, fallite %u, scadute %u, eccezioni %u, eco %u",
           elapsed / 1e6f, (unsigned) w.transactions, w.gap.count ? w.gap.min_us / 1000.0f : 0.0f,
           (unsigned) w.own_transactions, (unsigned) s.served_from_cache, (unsigned) s.served_from_bus,
           (unsigned) s.chunk_retries, (unsigned) s.local_failed, (unsigned) s.local_expired,
           (unsigned) s.exceptions, (unsigned) sn.total_echoes);
  if (this->log_polls_)
    this->dump_polls();
  w.reset(now);
}

void SaldaBus::dump_polls() { salda_common::log_polls(this->core_.sniffer, TAG); }

void SaldaBus::dump_registers() { salda_common::log_registers(this->core_.sniffer, TAG, micros()); }

void SaldaBus::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Salda bus condiviso:\n"
                "  Carattere: %u us\n"
                "  Durata max transazione col pannello attivo: %u us (finestra slot %u us)\n"
                "  Pannello fermo dopo: %u ms\n"
                "  Cache valida per: %u ms\n"
                "  Registri per lettura: %u",
                (unsigned) this->core_.byte_us, (unsigned) this->core_.budget_us,
                (unsigned) this->core_.slot_window_us, (unsigned) (this->core_.idle_us / 1000),
                (unsigned) (this->core_.max_age_us / 1000), (unsigned) this->core_.max_regs);
}

}  // namespace salda_bus
}  // namespace esphome
