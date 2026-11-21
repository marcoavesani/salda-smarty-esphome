// Simulazione su PC del bus condiviso: S-Touch + unità + ESP (BusCore) sulla stessa linea.
// g++ -std=c++17 -O2 -Icomponents tests/test_bus.cpp -o t && ./t
//
// Il bus è modellato a livello di byte: se due trasmissioni si sovrappongono nel
// tempo, entrambe arrivano corrotte a tutti. Il pannello riproduce il traffico
// misurato sul campo (23 richieste a ciclo, pause 34..100 ms), l'unità risponde
// dopo 5..15 ms. "modbus_controller" fa le letture e scritture dei pacchetti ESPHome.
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include "salda_bus/bus_core.h"

using namespace esphome::salda_bus;
using esphome::salda_common::frame::append_crc;
using esphome::salda_common::frame::crc_ok;
using esphome::salda_common::frame::matches;

static int failures = 0;
#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                \
    }                                                            \
  } while (0)

static const uint32_t BYTE = 573;  // µs per carattere a 19200 8E1

// ---------------------------------------------------------------- modello unità
struct Model {
  std::map<uint16_t, uint16_t> hr, coil;
  uint16_t get(uint8_t fc, uint16_t a) {
    switch (fc) {
      case 1: return coil.count(a) ? coil[a] : (a % 2);
      case 2: return a % 3 == 0;
      case 3: return hr.count(a) ? hr[a] : (uint16_t) (3000 + a);
      default: return (uint16_t) (4000 + a);
    }
  }
};

static std::vector<uint8_t> req(uint8_t fc, uint16_t s, uint16_t q) {
  std::vector<uint8_t> f = {1, fc, (uint8_t) (s >> 8), (uint8_t) s, (uint8_t) (q >> 8), (uint8_t) q};
  append_crc(f);
  return f;
}

static std::vector<uint8_t> reply(Model &m, const std::vector<uint8_t> &q) {
  uint8_t fc = q[1];
  uint16_t s = (q[2] << 8) | q[3], n = (q[4] << 8) | q[5];
  std::vector<uint8_t> r = {q[0], fc};
  if (fc == 3 || fc == 4) {
    r.push_back((uint8_t) (2 * n));
    for (uint16_t i = 0; i < n; i++) {
      uint16_t v = m.get(fc, s + i);
      r.push_back(v >> 8);
      r.push_back(v & 0xFF);
    }
  } else if (fc == 1 || fc == 2) {
    uint16_t nb = (n + 7) / 8;
    r.push_back((uint8_t) nb);
    r.resize(3 + nb, 0);
    for (uint16_t i = 0; i < n; i++)
      if (m.get(fc, s + i))
        r[3 + i / 8] |= 1 << (i % 8);
  } else if (fc == 5) {
    m.coil[s] = q[4] == 0xFF;
    r.insert(r.end(), q.begin() + 2, q.begin() + 6);
  } else if (fc == 6) {
    m.hr[s] = (q[4] << 8) | q[5];
    r.insert(r.end(), q.begin() + 2, q.begin() + 6);
  } else {
    r = {q[0], (uint8_t) (fc | 0x80), 1};
  }
  append_crc(r);
  return r;
}

// ------------------------------------------------------------------- bus fisico
enum Who { PANEL = 0, UNIT = 1, ESP = 2 };
struct Tx {
  Who who;
  uint32_t start;
  std::vector<uint8_t> bytes;
  bool collided = false, delivered = false;
  uint32_t end() const { return start + (uint32_t) bytes.size() * BYTE; }
};

struct Scenario {
  const char *name;
  uint32_t gap_min_us, gap_max_us;  // pausa del pannello dopo una risposta
  double short_gap_prob;            // probabilità di una pausa corta (34..40 ms)
  uint32_t lat_min_us, lat_max_us;  // latenza dell'unità
  uint32_t loop_min_us, loop_max_us;
  bool echo;
  bool panel_present;
  bool expect_no_collisions;
};

static void run(const Scenario &sc) {
  std::mt19937 rng(7);
  auto rnd = [&](uint32_t a, uint32_t b) { return a + (uint32_t) (rng() % (b - a + 1)); };
  Model model;
  BusCore core;
  std::vector<Tx> txs;
  uint32_t collisions = 0;

  auto transmit = [&](Who who, uint32_t now, const std::vector<uint8_t> &b) {
    Tx t{who, now, b};
    for (auto &o : txs)
      if (!o.delivered && o.end() > now) {
        if (!o.collided)
          collisions++;
        o.collided = t.collided = true;
      }
    txs.push_back(t);
  };

  // unità
  bool unit_busy = false;
  uint32_t unit_at = 0;
  std::vector<uint8_t> unit_resp;
  // pannello
  static const struct { uint8_t fc; uint16_t s, q; } CYCLE[] = {
      {3, 202, 1}, {3, 3, 1},    {3, 1, 1},   {3, 2, 1},   {3, 4, 1},   {3, 6, 1},    {1, 117, 1}, {1, 5, 1},
      {3, 107, 1}, {3, 14, 1},   {1, 1, 1},   {4, 30, 1},  {4, 18, 1},  {1, 125, 1},  {3, 661, 1}, {3, 662, 1},
      {1, 51, 1},  {1, 53, 1},   {1, 8, 1},   {3, 21, 1},  {3, 22, 1},  {3, 23, 1},   {2, 1, 339}};
  const size_t NCYCLE = sizeof(CYCLE) / sizeof(CYCLE[0]);
  size_t p_idx = 0;
  bool p_waiting = false;
  uint32_t p_next = 50000, p_sent_end = 0;
  std::vector<uint8_t> p_req;
  uint32_t p_ok = 0, p_bad = 0, p_timeout = 0;
  // "modbus_controller"
  struct Cmd { std::vector<uint8_t> frame; };
  std::vector<Cmd> cmds;
  auto R = [&](uint8_t fc, uint16_t s, uint16_t q) { cmds.push_back({req(fc, s, q)}); };
  R(4, 1, 30); R(4, 55, 10); R(4, 93, 1); R(4, 104, 1); R(4, 125, 1);
  R(3, 1, 4); R(3, 6, 1); R(3, 14, 2); R(3, 21, 2); R(3, 24, 1);
  R(1, 3, 3); R(1, 8, 1);
  R(2, 3, 3); R(2, 8, 4); R(2, 14, 6); R(2, 24, 4); R(2, 34, 2); R(2, 38, 2); R(2, 44, 1); R(2, 47, 1);
  R(2, 49, 2); R(2, 55, 2); R(2, 58, 1); R(2, 188, 2); R(2, 207, 1); R(2, 209, 1);
  size_t h_idx = 0;
  bool h_waiting = false;
  uint32_t h_next = 1000000, h_sent = 0, h_cycle_start = 1000000;
  std::vector<uint8_t> h_req, h_rx;
  uint32_t h_ok = 0, h_bad = 0, h_timeout = 0, h_cycles = 0;
  bool wrote_hr2 = false, wrote_coil5 = false;
  // ESP
  std::vector<std::pair<uint32_t, uint8_t>> esp_rx;
  size_t esp_rd = 0;
  uint32_t esp_next = 0;

  const uint32_t T = 120000000;  // 2 minuti
  for (uint32_t now = 0; now < T; now += 100) {
    // consegna delle trasmissioni terminate
    for (auto &t : txs) {
      if (t.delivered || t.end() > now)
        continue;
      t.delivered = true;
      std::vector<uint8_t> b = t.bytes;
      if (t.collided)
        for (auto &x : b)
          x ^= (uint8_t) (0x5A + (rng() & 0x0F));
      // all'ESP (anche le proprie se il transceiver fa eco)
      if (t.who != ESP || sc.echo)
        for (size_t i = 0; i < b.size(); i++)
          esp_rx.push_back({t.start + (uint32_t) (i + 1) * BYTE, b[i]});
      // all'unità: richieste pulite
      if (t.who != UNIT && !unit_busy && crc_ok(b.data(), b.size()) && b[0] == 1) {
        unit_busy = true;
        unit_resp = reply(model, b);
        unit_at = t.end() + rnd(sc.lat_min_us, sc.lat_max_us);
      }
      // al pannello: risposta dell'unità mentre aspetta
      if (t.who == UNIT && p_waiting && crc_ok(b.data(), b.size()) && matches(p_req, b)) {
        std::vector<uint8_t> expect = reply(model, p_req);
        if (b == expect)
          p_ok++;
        else
          p_bad++;
        p_waiting = false;
        uint32_t gap = (rng() % 1000) < sc.short_gap_prob * 1000 ? rnd(34000, 40000)
                                                                  : rnd(sc.gap_min_us, sc.gap_max_us);
        p_next = now + gap;
      }
    }
    while (txs.size() > 64 && txs.front().delivered)
      txs.erase(txs.begin());

    // unità
    if (unit_busy && now >= unit_at) {
      transmit(UNIT, now, unit_resp);
      unit_busy = false;
    }
    // pannello
    if (sc.panel_present) {
      if (p_waiting && (int32_t) (now - p_sent_end) > 300000) {
        p_timeout++;
        p_waiting = false;
        p_next = now + rnd(sc.gap_min_us, sc.gap_max_us);
      }
      if (!p_waiting && now >= p_next) {
        auto &c = CYCLE[p_idx++ % NCYCLE];
        p_req = req(c.fc, c.s, c.q);
        transmit(PANEL, now, p_req);
        p_sent_end = now + (uint32_t) p_req.size() * BYTE;
        p_waiting = true;
      }
    }
    // modbus_controller
    if (!h_waiting && now >= h_next) {
      if (!wrote_hr2 && now > 20000000) {
        h_req = {1, 6, 0, 2, 0, 215};
        append_crc(h_req);
        wrote_hr2 = true;
      } else if (!wrote_coil5 && now > 40000000) {
        h_req = {1, 5, 0, 5, 0xFF, 0};
        append_crc(h_req);
        wrote_coil5 = true;
      } else {
        h_req = cmds[h_idx].frame;
        h_idx = (h_idx + 1) % cmds.size();
        if (h_idx == 0) {
          h_cycles++;
          h_next = h_cycle_start + 10000000;  // update_interval 10 s
          h_cycle_start = h_next;
        }
      }
      if (now >= h_next) {
        core.queue_local(h_req.data(), h_req.size(), now);
        h_waiting = true;
        h_sent = now;
        h_rx.clear();
      }
    }
    // ESP loop
    if (now >= esp_next) {
      std::vector<uint8_t> chunk;
      while (esp_rd < esp_rx.size() && esp_rx[esp_rd].first <= now)
        chunk.push_back(esp_rx[esp_rd++].second);
      if (!chunk.empty())
        core.on_bus_bytes(chunk.data(), chunk.size(), now);
      core.step(now);
      if (!core.to_bus.empty()) {
        transmit(ESP, now, core.to_bus);
        core.to_bus.clear();
      }
      while (!core.to_local.empty()) {
        h_rx.push_back(core.to_local.front());
        core.to_local.pop_front();
      }
      esp_next = now + rnd(sc.loop_min_us, sc.loop_max_us);
    }
    // risposta a modbus_controller
    if (h_waiting && !h_rx.empty()) {
      if (crc_ok(h_rx.data(), h_rx.size()) && h_rx == reply(model, h_req))
        h_ok++;
      else
        h_bad++;
      h_waiting = false;
      h_next = std::max(h_next, now + 50000);  // command_throttle
    } else if (h_waiting && now - h_sent > 5000000) {
      h_timeout++;
      h_waiting = false;
      h_next = std::max(h_next, now + 50000);
    }
  }

  const BusStats &s = core.stats;
  std::printf("%-32s pannello ok=%u errati=%u timeout=%u | HA ok=%u errati=%u timeout=%u cicli=%u | "
              "cache=%u bus=%u tx=%u ritrasm=%u collisioni=%u eco=%u\n",
              sc.name, p_ok, p_bad, p_timeout, h_ok, h_bad, h_timeout, h_cycles, s.served_from_cache,
              s.served_from_bus, s.chunks_sent, s.chunk_retries, collisions, core.sniffer.total_echoes);

  CHECK(p_bad == 0);  // il pannello non riceve mai dati sbagliati
  CHECK(h_bad == 0);  // Home Assistant non riceve mai dati sbagliati
  CHECK(h_ok > 0);
  CHECK(model.get(3, 2) == 215);  // la scrittura è arrivata
  CHECK(model.get(1, 5) == 1);
  if (sc.expect_no_collisions) {
    CHECK(collisions == 0);
    CHECK(h_timeout == 0);
  }
  if (sc.panel_present)
    CHECK(p_ok > 0);
}

int main() {
  // nome, pausa min/max, prob. pausa corta, latenza min/max, loop ESP min/max, eco, pannello, nessuna collisione attesa
  run({"come misurato (pause 55-95 ms)", 55000, 95000, 0.01, 5000, 15000, 300, 2500, false, true, true});
  run({"come misurato, con eco", 55000, 95000, 0.01, 5000, 15000, 300, 2500, true, true, true});
  run({"pannello assente", 55000, 95000, 0.0, 5000, 15000, 300, 2500, false, false, true});
  run({"loop ESP lento (5-12 ms)", 55000, 95000, 0.01, 5000, 15000, 5000, 12000, false, true, false});
  run({"pause strette (20-40 ms)", 20000, 40000, 0.0, 5000, 15000, 300, 2500, false, true, false});
  if (failures == 0)
    std::printf("OK: tutti i test superati\n");
  return failures == 0 ? 0 : 1;
}
