// Test su PC dello sniffer: g++ -std=c++17 -Icomponents tests/test_sniffer.cpp -o t && ./t
// Simula il traffico S-Touch <-> unità a 19200 baud, con i byte che arrivano
// a pezzi come da una UART letta nel loop, e verifica misure e valori decodificati.
#include <cstdio>
#include <cstdlib>
#include "salda_sniffer/sniffer_core.h"

using namespace esphome::salda_sniffer;
using esphome::salda_common::frame::append_crc;

static int failures = 0;
#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                \
    }                                                            \
  } while (0)

static const uint32_t BYTE_US = 573;  // 11 bit a 19200 baud

static uint16_t reg_value(uint8_t fc, uint16_t a) { return (uint16_t) (fc * 1000 + a); }

static std::vector<uint8_t> req(uint8_t fc, uint16_t start, uint16_t qty_or_value) {
  std::vector<uint8_t> f = {0x01, fc, (uint8_t) (start >> 8), (uint8_t) start, (uint8_t) (qty_or_value >> 8),
                            (uint8_t) qty_or_value};
  append_crc(f);
  return f;
}

static std::vector<uint8_t> reply(const std::vector<uint8_t> &q) {
  uint8_t fc = q[1];
  uint16_t start = (q[2] << 8) | q[3], qty = (q[4] << 8) | q[5];
  std::vector<uint8_t> r = {q[0], fc};
  if (fc == 3 || fc == 4) {
    r.push_back((uint8_t) (qty * 2));
    for (uint16_t i = 0; i < qty; i++) {
      uint16_t v = reg_value(fc, start + i);
      r.push_back(v >> 8);
      r.push_back(v & 0xFF);
    }
  } else if (fc == 1 || fc == 2) {
    uint8_t n = (qty + 7) / 8;
    r.push_back(n);
    for (uint8_t i = 0; i < n; i++)
      r.push_back(0xA5);  // bit alternati
  } else {
    r.insert(r.end(), q.begin() + 2, q.begin() + 6);  // eco per FC05/06
  }
  append_crc(r);
  return r;
}

// Bus simulato: accoda byte con il loro istante di arrivo; il "loop" li legge a pezzi.
struct Bus {
  std::vector<std::pair<uint32_t, uint8_t>> bytes;
  uint32_t t{0};
  void send(const std::vector<uint8_t> &f) {
    for (uint8_t b : f) {
      t += BYTE_US;
      bytes.push_back({t, b});
    }
  }
  void wait_us(uint32_t us) { t += us; }
};

static void run_loop(SnifferCore &s, const Bus &bus, uint32_t loop_us) {
  size_t i = 0;
  uint32_t end = bus.t + 1000000;
  std::vector<uint8_t> chunk;
  for (uint32_t now = 0; now < end; now += loop_us + (rand() % 3) * 1000) {  // loop irregolare
    chunk.clear();
    while (i < bus.bytes.size() && bus.bytes[i].first <= now)
      chunk.push_back(bus.bytes[i++].second);
    if (!chunk.empty())
      s.on_bytes(chunk.data(), chunk.size(), now);
    s.step(now);
  }
}

int main() {
  srand(42);
  Bus bus;
  // Ciclo tipico ipotetico di un pannello: 3 letture ogni secondo, pausa 40 ms tra
  // una transazione e l'altra, unità che risponde dopo 8 ms. Per 30 secondi.
  const auto ir = req(0x04, 1, 32);
  const auto hr = req(0x03, 1, 37);
  const auto co = req(0x01, 1, 9);
  bus.wait_us(3000);
  bus.send({0x13, 0x00});  // spazzatura iniziale (avvio a metà frame)
  bus.wait_us(50000);
  for (int s = 0; s < 30; s++) {
    uint32_t cycle_start = bus.t;
    for (const auto *q : {&ir, &hr, &co}) {
      bus.send(*q);
      bus.wait_us(8000);
      bus.send(reply(*q));
      bus.wait_us(40000);
    }
    if (s == 10) {  // l'utente cambia il setpoint dal pannello
      auto w = req(0x06, 2, 215);
      bus.send(w);
      bus.wait_us(8000);
      bus.send(reply(w));
      bus.wait_us(40000);
    }
    if (s == 20) {  // una richiesta rimane senza risposta
      bus.send(req(0x04, 55, 2));
      bus.wait_us(700000);
    }
    bus.t = cycle_start + 1000000 > bus.t ? cycle_start + 1000000 : bus.t;
  }

  for (uint32_t loop_us : {1000u, 5000u, 16000u}) {
    SnifferCore s;
    run_loop(s, bus, loop_us);
    const WindowStats &w = s.window;
    std::printf("loop %2u ms: transazioni=%u senza_risposta=%u orfane=%u scartati=%u  gap min/avg=%u/%u ms  "
                "latenza avg=%u ms  occupazione=%.1f%%  richieste diverse=%u\n",
                loop_us / 1000, s.total_transactions, s.total_no_response, s.total_orphan_responses,
                s.total_dropped_bytes, w.gap.min_us / 1000, w.gap.avg_us() / 1000, w.latency.avg_us() / 1000,
                100.0 * w.busy_us / (bus.t - w.start_us), (unsigned) s.polls.size());

    CHECK(s.total_transactions == 30 * 3 + 1);
    CHECK(s.total_no_response == 1);
    CHECK(s.polls.size() == 5);  // IR, HR, coil, scrittura HR2, IR55
    // La pausa reale è 40 ms; con un loop lento la misura è meno precisa.
    CHECK(w.gap.min_us >= 40000 - 2 * (loop_us + 2000) && w.gap.min_us <= 40000 + 2 * (loop_us + 2000));
    CHECK(w.latency.avg_us() <= 8000 + 2 * (loop_us + 2000));
    // Valori decodificati
    CHECK(s.registers[SnifferCore::reg_key(4, 18)] == reg_value(4, 18));
    CHECK(s.registers[SnifferCore::reg_key(3, 37)] == reg_value(3, 37));
    CHECK(s.registers[SnifferCore::reg_key(3, 2)] == reg_value(3, 2));  // riletto dopo la scrittura
    CHECK(s.registers[SnifferCore::reg_key(1, 1)] == 1 && s.registers[SnifferCore::reg_key(1, 2)] == 0);
    // Scrittura vista
    CHECK(s.writes.size() == 1 && s.writes[0].fc == 6 && s.writes[0].start == 2 && s.writes[0].values[0] == 215 &&
          s.writes[0].confirmed);
    // Intervallo tra due letture uguali ~1 s
    for (const auto &p : s.polls)
      if (p.key.fc == 4 && p.key.start == 1)
        CHECK(p.count == 30 && p.interval.avg_us() > 950000 && p.interval.avg_us() < 1100000);
  }

  if (failures == 0)
    std::printf("OK: tutti i test superati\n");
  return failures == 0 ? 0 : 1;
}
