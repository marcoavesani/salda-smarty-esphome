#pragma once
// Stampa nel log ESPHome di ciò che SnifferCore ha visto (usato da salda_sniffer e salda_bus).

#include <cstdio>
#include <string>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "sniffer_core.h"

namespace esphome {
namespace salda_common {

inline const char *table_name(uint8_t fc) {
  switch (fc) {
    case 0x01: case 0x05: case 0x0F: return "COIL";
    case 0x02: return "DI";
    case 0x03: case 0x06: case 0x10: return "HR";
    case 0x04: return "IR";
    default: return "?";
  }
}

inline const char *type_name(uint8_t type) {
  switch (type) {
    case 1: return "COIL";
    case 2: return "DI";
    case 3: return "HR";
    case 4: return "IR";
    default: return "?";
  }
}

/// Le scritture del pannello viste dall'ultima chiamata (poi la lista viene svuotata).
inline void log_writes(SnifferCore &core, const char *tag) {
  for (const auto &w : core.writes) {
    std::string vals;
    for (size_t i = 0; i < w.values.size() && i < 16; i++) {
      if (i)
        vals += ",";
      vals += to_string(w.values[i]);
    }
    ESP_LOGI(tag, "Scrittura del pannello: FC%02X %s %u%s = %s (%s)", w.fc, table_name(w.fc), w.start,
             w.qty > 1 ? ("+" + to_string(w.qty - 1)).c_str() : "", vals.c_str(),
             w.confirmed ? "confermata dall'unità" : "RIFIUTATA (eccezione)");
  }
  core.writes.clear();
}

/// Elenco delle richieste del pannello: cosa legge e ogni quanto.
inline void log_polls(const SnifferCore &core, const char *tag) {
  ESP_LOGI(tag, "Richieste del pannello (%u tipi diversi, transazioni %u, senza risposta %u, eccezioni %u):",
           (unsigned) core.polls.size(), (unsigned) core.total_transactions, (unsigned) core.total_no_response,
           (unsigned) core.total_exceptions);
  for (const auto &p : core.polls) {
    const auto &k = p.key;
    if (p.interval.count) {
      ESP_LOGI(tag, "  slave %u FC%02X %-4s %4u..%-4u (%3u)  x%-6u ogni %.0f ms (min %.0f, max %.0f)  "
                    "senza risp. %u, eccezioni %u",
               k.addr, k.fc, table_name(k.fc), k.start, k.start + (k.qty ? k.qty - 1 : 0), k.qty,
               (unsigned) p.count, p.interval.avg_us() / 1000.0f, p.interval.min_us / 1000.0f,
               p.interval.max_us / 1000.0f, (unsigned) p.no_response, (unsigned) p.exceptions);
    } else {
      ESP_LOGI(tag, "  slave %u FC%02X %-4s %4u..%-4u (%3u)  x%-6u  senza risp. %u, eccezioni %u", k.addr, k.fc,
               table_name(k.fc), k.start, k.start + (k.qty ? k.qty - 1 : 0), k.qty, (unsigned) p.count,
               (unsigned) p.no_response, (unsigned) p.exceptions);
    }
    delay(0);
  }
}

/// Tutti i valori in cache, 8 per riga; per i registri anche la lettura con segno.
inline void log_registers(const SnifferCore &core, const char *tag, uint32_t now) {
  ESP_LOGI(tag, "Valori visti (%u):", (unsigned) core.registers.size());
  std::string line;
  int line_type = -1, line_start = -1, last_addr = -2, count = 0;
  uint32_t oldest_s = 0;
  auto flush = [&]() {
    if (!line.empty())
      ESP_LOGI(tag, "  %-4s %4d: %s (età max %us)", type_name((uint8_t) line_type), line_start, line.c_str(),
               (unsigned) oldest_s);
    line.clear();
    oldest_s = 0;
  };
  for (const auto &kv : core.registers) {
    int type = (int) (kv.first >> 16);
    int addr = (int) (kv.first & 0xFFFF);
    if (type != line_type || addr != last_addr + 1 || count >= 8) {
      flush();
      line_type = type;
      line_start = addr;
      count = 0;
    }
    char part[24];
    if (type == 3 || type == 4)
      snprintf(part, sizeof(part), "%u(%d) ", kv.second.value, (int16_t) kv.second.value);
    else
      snprintf(part, sizeof(part), "%u ", kv.second.value);
    line += part;
    uint32_t age_s = (now - kv.second.time_us) / 1000000;
    if (age_s > oldest_s)
      oldest_s = age_s;
    last_addr = addr;
    count++;
    delay(0);
  }
  flush();
}

}  // namespace salda_common
}  // namespace esphome
