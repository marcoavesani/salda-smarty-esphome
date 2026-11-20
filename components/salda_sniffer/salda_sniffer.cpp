#include "salda_sniffer.h"

#include <algorithm>
#include <string>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace salda_sniffer {

static const char *const TAG = "salda_sniffer";

static const char *table_name(uint8_t fc) {
  switch (fc) {
    case 0x01: case 0x05: case 0x0F: return "COIL";
    case 0x02: return "DI";
    case 0x03: case 0x06: case 0x10: return "HR";
    case 0x04: return "IR";
    default: return "?";
  }
}

static const char *type_name(uint8_t type) {
  switch (type) {
    case 1: return "COIL";
    case 2: return "DI";
    case 3: return "HR";
    case 4: return "IR";
    default: return "?";
  }
}

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

void SaldaSniffer::log_writes_() {
  for (const auto &w : this->core_.writes) {
    std::string vals;
    for (size_t i = 0; i < w.values.size() && i < 16; i++) {
      if (i)
        vals += ",";
      vals += to_string(w.values[i]);
    }
    ESP_LOGI(TAG, "Scrittura vista: FC%02X %s %u%s = %s (%s)", w.fc, table_name(w.fc), w.start,
             w.qty > 1 ? ("+" + to_string(w.qty - 1)).c_str() : "", vals.c_str(),
             w.confirmed ? "confermata dall'unità" : "RIFIUTATA (eccezione)");
  }
  this->core_.writes.clear();
}

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

void SaldaSniffer::dump_polls() {
  ESP_LOGI(TAG, "Richieste viste (%u tipi diversi, totale transazioni %u, senza risposta %u, eccezioni %u):",
           (unsigned) this->core_.polls.size(), (unsigned) this->core_.total_transactions,
           (unsigned) this->core_.total_no_response, (unsigned) this->core_.total_exceptions);
  for (const auto &p : this->core_.polls) {
    const auto &k = p.key;
    if (p.interval.count) {
      ESP_LOGI(TAG, "  slave %u FC%02X %-4s %4u..%-4u (%3u)  x%-6u ogni %.0f ms (min %.0f, max %.0f)  "
                    "senza risp. %u, eccezioni %u",
               k.addr, k.fc, table_name(k.fc), k.start, k.start + (k.qty ? k.qty - 1 : 0), k.qty,
               (unsigned) p.count, p.interval.avg_us() / 1000.0f, p.interval.min_us / 1000.0f,
               p.interval.max_us / 1000.0f, (unsigned) p.no_response, (unsigned) p.exceptions);
    } else {
      ESP_LOGI(TAG, "  slave %u FC%02X %-4s %4u..%-4u (%3u)  x%-6u  senza risp. %u, eccezioni %u", k.addr, k.fc,
               table_name(k.fc), k.start, k.start + (k.qty ? k.qty - 1 : 0), k.qty, (unsigned) p.count,
               (unsigned) p.no_response, (unsigned) p.exceptions);
    }
  }
}

void SaldaSniffer::dump_registers() {
  ESP_LOGI(TAG, "Valori visti (%u):", (unsigned) this->core_.registers.size());
  // Una riga per gruppo di 8 registri consecutivi dello stesso tipo.
  std::string line;
  int line_type = -1;
  int line_start = -1;
  int last_addr = -2;
  auto flush = [&]() {
    if (!line.empty())
      ESP_LOGI(TAG, "  %-4s %4d: %s", type_name((uint8_t) line_type), line_start, line.c_str());
    line.clear();
  };
  int count = 0;
  for (const auto &kv : this->core_.registers) {
    int type = (int) (kv.first >> 16);
    int addr = (int) (kv.first & 0xFFFF);
    if (type != line_type || addr != last_addr + 1 || count >= 8) {
      flush();
      line_type = type;
      line_start = addr;
      count = 0;
    }
    char part[24];
    // valore grezzo e, per i registri, interpretazione con segno (utile per temperature x0,1)
    if (type == 3 || type == 4)
      snprintf(part, sizeof(part), "%u(%d) ", kv.second, (int16_t) kv.second);
    else
      snprintf(part, sizeof(part), "%u ", kv.second);
    line += part;
    last_addr = addr;
    count++;
    delay(0);
  }
  flush();
}

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
