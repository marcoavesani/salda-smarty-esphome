// Test su PC del parser dei frame: g++ -std=c++17 -I../components tests/test_frame.cpp -o t && ./t
#include <cstdio>
#include <string>
#include "salda_common/frame.h"

using namespace esphome::salda_common::frame;

static int failures = 0;
#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                \
    }                                                            \
  } while (0)

static std::vector<uint8_t> F(std::vector<uint8_t> v) {
  append_crc(v);
  return v;
}

static std::vector<uint8_t> cat(std::vector<uint8_t> a, const std::vector<uint8_t> &b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

int main() {
  std::vector<uint8_t> out;
  size_t dropped = 0;

  // CRC noto: 01 03 00 00 00 01 -> 84 0A
  {
    auto f = F({0x01, 0x03, 0x00, 0x00, 0x00, 0x01});
    CHECK(f[6] == 0x84 && f[7] == 0x0A);
    CHECK(crc_ok(f.data(), f.size()));
  }

  // Richiesta FC04 completa
  {
    auto req = F({0x01, 0x04, 0x00, 0x12, 0x00, 0x04});
    std::vector<uint8_t> buf = req;
    CHECK(extract(buf, true, 0, 20, out, dropped) == Result::FRAME);
    CHECK(out == req && buf.empty());
  }

  // Richiesta spezzata in due letture
  {
    auto req = F({0x01, 0x03, 0x00, 0x01, 0x00, 0x25});
    std::vector<uint8_t> buf(req.begin(), req.begin() + 5);
    CHECK(extract(buf, true, 1, 20, out, dropped) == Result::NEED_MORE);
    buf.insert(buf.end(), req.begin() + 5, req.end());
    CHECK(extract(buf, true, 0, 20, out, dropped) == Result::FRAME);
    CHECK(out == req);
  }

  // Due richieste attaccate
  {
    auto a = F({0x01, 0x05, 0x00, 0x05, 0xFF, 0x00});
    auto b = F({0x01, 0x06, 0x00, 0x02, 0x00, 0xD2});
    std::vector<uint8_t> buf = cat(a, b);
    CHECK(extract(buf, true, 0, 20, out, dropped) == Result::FRAME && out == a);
    CHECK(extract(buf, true, 0, 20, out, dropped) == Result::FRAME && out == b);
    CHECK(buf.empty());
  }

  // FC16 a lunghezza variabile (orologio: 7 registri)
  {
    auto req = F({0x01, 0x10, 0x00, 0x64, 0x00, 0x07, 0x0E, 0, 26, 0, 11, 0, 10, 0, 0, 0, 12, 0, 30, 0, 0});
    std::vector<uint8_t> buf = req;
    CHECK(request_length(buf.data(), 6) == 0);
    CHECK(request_length(buf.data(), buf.size()) == (int) req.size());
    CHECK(extract(buf, true, 0, 20, out, dropped) == Result::FRAME && out == req);
  }

  // Risposta FC03 con 2 registri, poi eccezione
  {
    auto r = F({0x01, 0x03, 0x04, 0x00, 0xD2, 0xFF, 0x38});
    auto e = F({0x01, 0x83, 0x02});
    std::vector<uint8_t> buf = cat(r, e);
    CHECK(extract(buf, false, 0, 20, out, dropped) == Result::FRAME && out == r);
    CHECK(extract(buf, false, 0, 20, out, dropped) == Result::FRAME && out == e);
    CHECK(matches(F({0x01, 0x03, 0, 1, 0, 2}), e));
    CHECK(!matches(F({0x02, 0x03, 0, 1, 0, 2}), e));
  }

  // Spazzatura davanti: risincronizza scartando byte
  {
    auto req = F({0x01, 0x04, 0x00, 0x37, 0x00, 0x02});
    std::vector<uint8_t> buf = cat({0x00, 0xFF, 0x13}, req);
    dropped = 0;
    Result r = extract(buf, true, 0, 20, out, dropped);
    CHECK(r == Result::FRAME && out == req);
    CHECK(dropped == 3);
  }

  // CRC sbagliato e poi silenzio: tutto scartato, nessun frame
  {
    auto req = F({0x01, 0x04, 0x00, 0x37, 0x00, 0x02});
    req[7] ^= 0x55;
    std::vector<uint8_t> buf = req;
    CHECK(extract(buf, true, 50, 20, out, dropped) == Result::NEED_MORE);
    CHECK(buf.empty());
  }

  // Frame troncato: si attende, poi col silenzio si butta
  {
    auto req = F({0x01, 0x03, 0x00, 0x01, 0x00, 0x01});
    std::vector<uint8_t> buf(req.begin(), req.begin() + 6);
    CHECK(extract(buf, true, 5, 20, out, dropped) == Result::NEED_MORE && buf.size() == 6);
    CHECK(extract(buf, true, 25, 20, out, dropped) == Result::NEED_MORE && buf.empty());
  }

  // Function code sconosciuto (0x2B): accettato solo dopo il silenzio e con CRC valido
  {
    auto req = F({0x01, 0x2B, 0x0E, 0x01, 0x00});
    std::vector<uint8_t> buf = req;
    CHECK(extract(buf, true, 5, 20, out, dropped) == Result::NEED_MORE);
    CHECK(extract(buf, true, 25, 20, out, dropped) == Result::FRAME && out == req);
  }

  // Risposta FC06 (eco della richiesta) riconosciuta come risposta
  {
    auto req = F({0x01, 0x06, 0x00, 0x01, 0x00, 0x03});
    std::vector<uint8_t> buf = req;
    CHECK(extract(buf, false, 0, 20, out, dropped) == Result::FRAME && matches(req, out));
  }

  if (failures == 0)
    std::printf("OK: tutti i test superati\n");
  return failures == 0 ? 0 : 1;
}
