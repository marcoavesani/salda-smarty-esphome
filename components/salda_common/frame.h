#pragma once
// Riconoscimento dei frame Modbus RTU su un flusso di byte.
// Header-only e senza dipendenze da ESPHome: è testato su PC (tests/test_frame.cpp).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome {
namespace salda_common {
namespace frame {

static const size_t MAX_FRAME = 256;

inline uint16_t crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 1) ? (uint16_t) ((crc >> 1) ^ 0xA001) : (uint16_t) (crc >> 1);
  }
  return crc;
}

inline bool crc_ok(const uint8_t *data, size_t len) {
  if (len < 4)
    return false;
  uint16_t remote = (uint16_t) data[len - 2] | ((uint16_t) data[len - 1] << 8);
  return crc16(data, len - 2) == remote;
}

inline void append_crc(std::vector<uint8_t> &f) {
  uint16_t crc = crc16(f.data(), f.size());
  f.push_back(crc & 0xFF);
  f.push_back(crc >> 8);
}

/// Lunghezza totale attesa di una RICHIESTA (master -> slave).
/// >0 lunghezza nota, 0 servono altri byte, -1 function code sconosciuto.
inline int request_length(const uint8_t *b, size_t n) {
  if (n < 2)
    return 0;
  switch (b[1]) {
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x08:
      return 8;
    case 0x0F: case 0x10:  // addr fc start(2) qty(2) bytecount data crc(2)
      return n < 7 ? 0 : 9 + b[6];
    case 0x17:  // read/write multiple registers
      return n < 11 ? 0 : 13 + b[10];
    default:
      return -1;
  }
}

/// Lunghezza totale attesa di una RISPOSTA (slave -> master).
inline int response_length(const uint8_t *b, size_t n) {
  if (n < 2)
    return 0;
  if (b[1] & 0x80)
    return 5;  // eccezione: addr fc code crc(2)
  switch (b[1]) {
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x17:
      return n < 3 ? 0 : 5 + b[2];
    case 0x05: case 0x06: case 0x08: case 0x0F: case 0x10:
      return 8;
    default:
      return -1;
  }
}

enum class Result { NEED_MORE, FRAME };

/// Estrae il primo frame valido (CRC corretto) dall'inizio di `buf`.
/// - `idle_ms`: ms trascorsi dall'ultimo byte ricevuto.
/// - `gap_ms`: silenzio oltre il quale un frame incompleto è considerato perso
///   (e oltre il quale un frame di function code sconosciuto è considerato finito).
/// I byte che non formano un frame valido vengono scartati uno alla volta
/// (risincronizzazione); `dropped` conta i byte scartati.
inline Result extract(std::vector<uint8_t> &buf, bool is_request, uint32_t idle_ms, uint32_t gap_ms,
                      std::vector<uint8_t> &out, size_t &dropped) {
  while (!buf.empty()) {
    const bool silent = idle_ms >= gap_ms;
    int need = is_request ? request_length(buf.data(), buf.size()) : response_length(buf.data(), buf.size());
    if (need == 0) {
      if (!silent)
        return Result::NEED_MORE;
      buf.erase(buf.begin());
      dropped++;
      continue;
    }
    if (need < 0) {
      // Function code sconosciuto. Se più avanti c'è già un frame noto e valido,
      // quello che lo precede è spazzatura: scartalo subito.
      bool resynced = false;
      for (size_t k = 1; k + 4 <= buf.size(); k++) {
        const uint8_t *p = buf.data() + k;
        size_t rem = buf.size() - k;
        int n = is_request ? request_length(p, rem) : response_length(p, rem);
        if (n > 0 && (size_t) n <= rem && crc_ok(p, n)) {
          buf.erase(buf.begin(), buf.begin() + k);
          dropped += k;
          resynced = true;
          break;
        }
      }
      if (resynced)
        continue;
      // Altrimenti il frame finisce col silenzio, valido se il CRC torna.
      if (!silent)
        return Result::NEED_MORE;
      if (buf.size() <= MAX_FRAME && crc_ok(buf.data(), buf.size())) {
        out.assign(buf.begin(), buf.end());
        buf.clear();
        return Result::FRAME;
      }
      buf.erase(buf.begin());
      dropped++;
      continue;
    }
    if ((size_t) need > MAX_FRAME) {
      buf.erase(buf.begin());
      dropped++;
      continue;
    }
    if (buf.size() < (size_t) need) {
      if (!silent)
        return Result::NEED_MORE;
      buf.erase(buf.begin());
      dropped++;
      continue;
    }
    if (crc_ok(buf.data(), need)) {
      out.assign(buf.begin(), buf.begin() + need);
      buf.erase(buf.begin(), buf.begin() + need);
      return Result::FRAME;
    }
    buf.erase(buf.begin());
    dropped++;
  }
  return Result::NEED_MORE;
}

/// La risposta `resp` è plausibile per la richiesta `req`? Controlla indirizzo,
/// function code e, dove possibile, quantità/indirizzo riportati nella risposta.
inline bool matches(const std::vector<uint8_t> &req, const std::vector<uint8_t> &resp) {
  if (req.size() < 2 || resp.size() < 2)
    return false;
  if (resp[0] != req[0] || (resp[1] & 0x7F) != req[1])
    return false;
  if (resp[1] & 0x80)
    return true;  // eccezione
  const uint8_t fc = req[1];
  if (req.size() >= 6 && resp.size() >= 3) {
    const uint16_t qty = (uint16_t) ((req[4] << 8) | req[5]);
    switch (fc) {
      case 0x03: case 0x04:
        return resp[2] == (uint8_t) (qty * 2);
      case 0x01: case 0x02:
        return resp[2] == (uint8_t) ((qty + 7) / 8);
      case 0x05: case 0x06: case 0x0F: case 0x10:
        // la risposta ripete indirizzo (e valore o quantità)
        return resp.size() >= 6 && resp[2] == req[2] && resp[3] == req[3] && resp[4] == req[4] &&
               resp[5] == req[5];
      default:
        break;
    }
  }
  return true;
}

}  // namespace frame
}  // namespace salda_common
}  // namespace esphome
