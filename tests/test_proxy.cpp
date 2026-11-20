// Simulazione su PC del proxy: S-Touch + unità + richieste locali (Home Assistant).
// g++ -std=c++17 -Icomponents tests/test_proxy.cpp -o t && ./t
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include "salda_proxy/proxy_core.h"

using namespace esphome::salda_proxy;
using frame::append_crc;

static int failures = 0;
#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                \
    }                                                            \
  } while (0)

// Valore che l'unità finta restituisce per un registro: dipende da tipo e indirizzo,
// così chi riceve una risposta sbagliata se ne accorge.
static uint16_t reg_value(uint8_t fc, uint16_t addr) { return (uint16_t) (fc * 1000 + addr); }

static std::vector<uint8_t> read_req(uint8_t fc, uint16_t start, uint16_t count) {
  std::vector<uint8_t> f = {0x01, fc, (uint8_t) (start >> 8), (uint8_t) start, (uint8_t) (count >> 8), (uint8_t) count};
  append_crc(f);
  return f;
}

static std::vector<uint8_t> unit_reply(const std::vector<uint8_t> &req) {
  uint8_t fc = req[1];
  uint16_t start = (req[2] << 8) | req[3];
  uint16_t count = (req[4] << 8) | req[5];
  std::vector<uint8_t> r = {req[0], fc};
  if (fc == 0x03 || fc == 0x04) {
    r.push_back((uint8_t) (count * 2));
    for (uint16_t i = 0; i < count; i++) {
      uint16_t v = reg_value(fc, start + i);
      r.push_back(v >> 8);
      r.push_back(v & 0xFF);
    }
  } else if (fc == 0x01 || fc == 0x02) {
    uint8_t nbytes = (count + 7) / 8;
    r.push_back(nbytes);
    for (uint8_t i = 0; i < nbytes; i++)
      r.push_back((uint8_t) ((start + i) & 0xFF));
  } else if (fc == 0x05 || fc == 0x06 || fc == 0x10) {
    r.insert(r.end(), req.begin() + 2, req.begin() + 6);
  } else {
    r = {req[0], (uint8_t) (fc | 0x80), 0x01};
  }
  append_crc(r);
  return r;
}

// Controlla che `resp` sia la risposta giusta a `req`.
static bool valid_reply(const std::vector<uint8_t> &req, const std::vector<uint8_t> &resp) {
  return resp == unit_reply(req);
}

struct Scenario {
  const char *name;
  uint32_t unit_delay_ms;        // tempo di risposta dell'unità
  uint32_t unit_silence_every;   // ogni N richieste l'unità non risponde (0 = mai)
  uint32_t panel_period_ms;      // ogni quanto il pannello interroga
  uint32_t panel_timeout_ms;     // dopo quanto il pannello ritrasmette
  uint32_t local_period_ms;      // ogni quanto arriva una richiesta locale
  uint32_t local_timeout_ms;     // come send_wait_time di modbus
  uint32_t proxy_timeout_ms;     // response_timeout del proxy
};

static void run(const Scenario &sc) {
  ProxyCore core;
  core.response_timeout_ms = sc.proxy_timeout_ms;
  core.local_expire_ms = sc.local_timeout_ms - 200;
  srand(12345);
  const uint32_t T = 120000;  // 2 minuti simulati

  // unità
  std::deque<std::pair<uint32_t, std::vector<uint8_t>>> unit_out;  // (quando, byte)
  uint32_t unit_req_count = 0;
  // pannello
  std::vector<uint8_t> panel_req;
  uint32_t panel_sent = 0, panel_next = 50, panel_ok = 0, panel_bad = 0, panel_retry = 0;
  bool panel_waiting = false;
  std::vector<uint8_t> panel_rx;
  // locale
  std::deque<std::vector<uint8_t>> local_wait;
  std::vector<uint8_t> local_req;
  uint32_t local_sent = 0, local_next = 7, local_ok = 0, local_bad = 0, local_lost = 0;
  bool local_waiting = false;
  uint16_t local_addr = 1;
  std::vector<uint8_t> local_rx;

  for (uint32_t now = 0; now < T; now++) {
    // --- pannello: invia / ritrasmette ---
    if (panel_waiting && now - panel_sent >= sc.panel_timeout_ms) {
      panel_retry++;
      panel_waiting = false;
      panel_next = now;  // ritrasmette subito una NUOVA richiesta
    }
    if (!panel_waiting && now >= panel_next) {
      static const uint16_t addrs[] = {1, 18, 30, 55, 200};
      uint16_t a = addrs[rand() % 5];
      panel_req = read_req(rand() % 2 ? 0x03 : 0x04, a, 1 + rand() % 6);
      // spezza la richiesta in due pezzi per esercitare il parser
      core.on_panel_bytes(panel_req.data(), 3, now);
      core.step(now);
      core.on_panel_bytes(panel_req.data() + 3, panel_req.size() - 3, now);
      panel_sent = now;
      panel_waiting = true;
      panel_rx.clear();
      panel_next = now + sc.panel_period_ms;
    }

    // --- locale (modbus_controller) ---
    if (local_waiting && now - local_sent >= sc.local_timeout_ms) {
      local_lost++;
      local_waiting = false;
      core.to_local.clear();
    }
    if (!local_waiting && now >= local_next) {
      local_req = read_req(0x04, local_addr, 1 + (local_addr % 20));
      local_addr = (local_addr % 120) + 1;
      core.queue_local(local_req.data(), local_req.size(), now);
      local_sent = now;
      local_waiting = true;
      local_next = now + sc.local_period_ms;
    }

    // --- unità: consegna risposte programmate (a pezzi) ---
    while (!unit_out.empty() && unit_out.front().first <= now) {
      auto &b = unit_out.front().second;
      size_t half = b.size() / 2;
      core.on_unit_bytes(b.data(), half, now);
      core.step(now);
      core.on_unit_bytes(b.data() + half, b.size() - half, now);
      unit_out.pop_front();
    }

    core.step(now);

    // --- uscite del proxy ---
    if (!core.to_unit.empty()) {
      unit_req_count++;
      bool silent = sc.unit_silence_every && rand() % sc.unit_silence_every == 0;
      if (!silent)
        unit_out.push_back({now + sc.unit_delay_ms, unit_reply(core.to_unit)});
      core.to_unit.clear();
    }
    if (!core.to_panel.empty()) {
      if (panel_waiting) {
        if (valid_reply(panel_req, core.to_panel))
          panel_ok++;
        else
          panel_bad++;
        panel_waiting = false;
      } else {
        panel_bad++;  // risposta non richiesta: il pannello la scambierebbe per altro
      }
      core.to_panel.clear();
    }
    if (!core.to_local.empty()) {
      std::vector<uint8_t> r(core.to_local.begin(), core.to_local.end());
      core.to_local.clear();
      if (local_waiting && valid_reply(local_req, r))
        local_ok++;
      else
        local_bad++;
      local_waiting = false;
    }
  }

  const ProxyStats &s = core.stats;
  std::printf("%-34s pannello ok=%u errate=%u ritrasm=%u  locale ok=%u errate=%u perse=%u  "
              "timeout=%u stale=%u lat.max=%ums\n",
              sc.name, panel_ok, panel_bad, panel_retry, local_ok, local_bad, local_lost, s.timeouts,
              s.stale_responses, s.max_panel_latency_ms);

  // Proprietà fondamentali: nessuno riceve mai una risposta sbagliata.
  CHECK(panel_bad == 0);
  CHECK(local_bad == 0);
  // Se l'unità è più lenta di chi aspetta, nessuna risposta è il comportamento giusto.
  if (sc.unit_delay_ms < sc.panel_timeout_ms)
    CHECK(panel_ok > 0);
  if (sc.unit_delay_ms < sc.proxy_timeout_ms)
    CHECK(local_ok > 0);
  if (sc.unit_silence_every == 0 && sc.unit_delay_ms < sc.proxy_timeout_ms)
    CHECK(s.timeouts == 0);
}

int main() {
  // nome, ritardo unità, silenzio 1 su N, periodo pannello, timeout pannello, periodo locale,
  // timeout locale (send_wait_time), timeout proxy
  run({"unita' veloce, traffico normale", 15, 0, 100, 500, 50, 1500, 500});
  run({"pannello a raffica", 15, 0, 0, 500, 20, 1500, 500});
  run({"unita' lenta (120 ms)", 120, 0, 100, 500, 30, 1500, 500});
  run({"unita' che a volte non risponde", 15, 7, 100, 500, 30, 1500, 500});
  run({"unita' piu' lenta del pannello", 600, 0, 100, 500, 200, 3000, 1000});
  run({"risposte oltre il timeout del proxy", 550, 0, 100, 400, 50, 1500, 500});

  if (failures == 0)
    std::printf("OK: tutti i test superati\n");
  return failures == 0 ? 0 : 1;
}
