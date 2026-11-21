#include "salda_sniffer.h"

#include <algorithm>
#include <string>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "../salda_common/sniffer_log.h"

namespace esphome {
namespace salda_sniffer {

static const char *const TAG = "salda_sniffer";

void SaldaSniffer::setup() {
  // Il riconoscimento dei frame e la misura delle pause richiedono un loop frequente.
  this->high_freq_.start();
}

void SaldaSniffer::loop() {
  uint8_t buf[64];
  int avail;
  while ((avail = this->available()) > 0) {
    size_t n = std::min<size_t>((size_t) avail, sizeof(buf));
    if (!this->read_array(buf, n))
      break;
    this->core_.on_bytes(buf, n, micros());
  }
  this->core_.step(micros());
  if (!this->core_.writes.empty())
    this->log_writes_();
}

void SaldaSniffer::log_writes_() { salda_common::log_writes(this->core_, TAG); }

void SaldaSniffer::update() {
  uint32_t now = micros();
  WindowStats &w = this->core_.window;
  uint32_t elapsed = now - w.start_us;
  if (elapsed == 0)
    elapsed = 1;

  float per_min = w.transactions * 60e6f / elapsed;
  float load = 100.0f * (float) w.busy_us / elapsed;
  if (w.gap.count && w.gap.min_us < this->gap_min_since_boot_us_)
    this->gap_min_since_boot_us_ = w.gap.min_us;
  if (w.latency.max_us > this->latency_max_since_boot_us_)
    this->latency_max_since_boot_us_ = w.latency.max_us;

  if (this->transactions_ != nullptr)
    this->transactions_->publish_state(per_min);
  if (this->gap_min_ != nullptr)
    this->gap_min_->publish_state(w.gap.count ? w.gap.min_us / 1000.0f : NAN);
  if (this->gap_avg_ != nullptr)
    this->gap_avg_->publish_state(w.gap.count ? w.gap.avg_us() / 1000.0f : NAN);
  if (this->latency_max_ != nullptr)
    this->latency_max_->publish_state(w.latency.count ? w.latency.max_us / 1000.0f : NAN);
  if (this->bus_load_ != nullptr)
    this->bus_load_->publish_state(load);
  if (this->no_response_ != nullptr)
    this->no_response_->publish_state(this->core_.total_no_response);
  if (this->errors_ != nullptr)
    this->errors_->publish_state(this->core_.total_dropped_bytes + this->core_.total_orphan_responses +
                                 this->core_.total_unknown_frames);
  if (this->traffic_ != nullptr)
    this->traffic_->publish_state(this->core_.seen_traffic && now - this->core_.last_frame_us < 10000000);

  ESP_LOGI(TAG, "Ultimi %.0f s: %u transazioni (%.0f/min), %u senza risposta, %u errori, bus occupato %.1f%%",
           elapsed / 1e6f, (unsigned) w.transactions, per_min, (unsigned) w.no_response, (unsigned) w.errors, load);
  if (w.gap.count) {
    ESP_LOGI(TAG, "  Pausa dopo una transazione: min %.1f ms, media %.1f ms, max %.1f ms (min da avvio %.1f ms)",
             w.gap.min_us / 1000.0f, w.gap.avg_us() / 1000.0f, w.gap.max_us / 1000.0f,
             this->gap_min_since_boot_us_ / 1000.0f);
    std::string hist;
    for (size_t i = 0; i < GAP_BUCKETS; i++) {
      char part[40];
      if (i < GAP_BUCKETS - 1)
        snprintf(part, sizeof(part), "<%ums:%u ", (unsigned) GAP_BUCKETS_MS[i], (unsigned) w.gap_hist[i]);
      else
        snprintf(part, sizeof(part), ">=%ums:%u", (unsigned) GAP_BUCKETS_MS[i - 1], (unsigned) w.gap_hist[i]);
      hist += part;
    }
    ESP_LOGI(TAG, "  Istogramma pause: %s", hist.c_str());
  }
  if (w.latency.count) {
    ESP_LOGI(TAG, "  Latenza unità: min %.1f ms, media %.1f ms, max %.1f ms (max da avvio %.1f ms)",
             w.latency.min_us / 1000.0f, w.latency.avg_us() / 1000.0f, w.latency.max_us / 1000.0f,
             this->latency_max_since_boot_us_ / 1000.0f);
  }
  if (!this->core_.seen_traffic)
    ESP_LOGW(TAG, "Nessun frame valido ricevuto: controlla A/B, baud e parità (default 19200 8E1)");

  this->dump_polls();
  if (this->log_registers_)
    this->dump_registers();
  w.reset(now);
}

void SaldaSniffer::dump_polls() { salda_common::log_polls(this->core_, TAG); }

void SaldaSniffer::dump_registers() { salda_common::log_registers(this->core_, TAG, micros()); }

void SaldaSniffer::reset_stats() {
  SnifferCore fresh;
  fresh.gap_us = this->core_.gap_us;
  fresh.response_timeout_us = this->core_.response_timeout_us;
  this->core_ = fresh;
  this->gap_min_since_boot_us_ = 0xFFFFFFFF;
  this->latency_max_since_boot_us_ = 0;
  ESP_LOGI(TAG, "Statistiche azzerate");
}

void SaldaSniffer::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Salda sniffer (solo ascolto, non trasmette):\n"
                "  Silenzio fine frame: %u ms\n"
                "  Timeout risposta: %u ms\n"
                "  Log registri a ogni intervallo: %s",
                (unsigned) (this->core_.gap_us / 1000), (unsigned) (this->core_.response_timeout_us / 1000),
                YESNO(this->log_registers_));
}

}  // namespace salda_sniffer
}  // namespace esphome
