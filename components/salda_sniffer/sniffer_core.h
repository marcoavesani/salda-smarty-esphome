#pragma once
// Ascolto passivo di un bus Modbus RTU (S-Touch <-> unità). Non trasmette mai.
// Riconosce richieste e risposte, le accoppia e raccoglie le misure che servono
// a capire se un secondo master può inserirsi nelle pause:
//   - frequenza e contenuto delle interrogazioni del pannello
//   - pausa tra la fine di una risposta e la richiesta successiva
//   - latenza di risposta dell'unità e occupazione del bus
//   - valori letti/scritti (cache dei registri)
// Senza dipendenze da ESPHome: testato su PC (tests/test_sniffer.cpp).

#include <cstdint>
#include <map>
#include <vector>

#include "../salda_common/frame.h"

namespace esphome {
namespace salda_sniffer {

namespace frame = salda_common::frame;

/// Minimo/massimo/media di una durata in microsecondi.
struct Span {
  uint32_t count{0};
  uint32_t min_us{0xFFFFFFFF};
  uint32_t max_us{0};
  uint64_t sum_us{0};
  void add(uint32_t us) {
    count++;
    sum_us += us;
    if (us < min_us)
      min_us = us;
    if (us > max_us)
      max_us = us;
  }
  uint32_t avg_us() const { return count ? (uint32_t) (sum_us / count) : 0; }
  void reset() { *this = Span(); }
};

/// Istogramma delle pause tra una transazione e la successiva (ms).
static const uint32_t GAP_BUCKETS_MS[] = {5, 10, 20, 50, 100, 200, 500};
static const size_t GAP_BUCKETS = sizeof(GAP_BUCKETS_MS) / sizeof(GAP_BUCKETS_MS[0]) + 1;

struct WindowStats {
  uint32_t start_us{0};
  bool started{false};
  uint32_t transactions{0};
  uint32_t no_response{0};
  uint32_t errors{0};          // byte scartati + risposte orfane + frame sconosciuti
  uint64_t busy_us{0};         // somma delle durate richiesta->fine risposta
  Span gap;                    // fine risposta -> richiesta successiva
  Span latency;                // fine richiesta -> inizio risposta
  uint32_t gap_hist[GAP_BUCKETS]{};
  void reset(uint32_t now) {
    *this = WindowStats();
    start_us = now;
    started = true;
  }
};

struct PollKey {
  uint8_t addr, fc;
  uint16_t start, qty;
  bool operator==(const PollKey &o) const {
    return addr == o.addr && fc == o.fc && start == o.start && qty == o.qty;
  }
};

struct PollStat {
  PollKey key;
  uint32_t count{0};
  uint32_t no_response{0};
  uint32_t exceptions{0};
  uint32_t last_us{0};
  Span interval;
};

struct WriteEvent {
  uint32_t time_us;
  uint8_t addr, fc;
  uint16_t start, qty;
  std::vector<uint16_t> values;  // FC05: 1 = ON; FC06: valore; FC16: valori; FC15: bit (0/1)
  bool confirmed;                // l'unità ha risposto senza eccezione
};

class SnifferCore {
 public:
  // --- configurazione -------------------------------------------------------
  uint32_t gap_us{20000};               // silenzio che chiude/invalida un frame
  uint32_t response_timeout_us{500000}; // richiesta senza risposta oltre questo tempo
  size_t max_polls{64};
  size_t max_registers{1024};
  size_t max_write_events{32};

  // --- risultati -------------------------------------------------------------
  uint32_t total_transactions{0};
  uint32_t total_no_response{0};
  uint32_t total_orphan_responses{0};
  uint32_t total_exceptions{0};
  uint32_t total_unknown_frames{0};
  uint32_t total_dropped_bytes{0};
  uint32_t last_frame_us{0};
  bool seen_traffic{false};
  WindowStats window;
  std::vector<PollStat> polls;
  /// chiave: (tipo << 16) | indirizzo; tipo 1 coil, 2 discrete input, 3 holding, 4 input
  std::map<uint32_t, uint16_t> registers;
  std::vector<WriteEvent> writes;  // svuotato dall'adattatore dopo averlo loggato

  static uint32_t reg_key(uint8_t type, uint16_t addr) { return ((uint32_t) type << 16) | addr; }

  // --- ingressi --------------------------------------------------------------
  void on_bytes(const uint8_t *data, size_t len, uint32_t now) {
    for (size_t i = 0; i < len; i++) {
      buf_.push_back(data[i]);
      times_.push_back(now);
    }
    last_rx_ = now;
    if (!window.started)
      window.reset(now);
    if (buf_.size() > 4 * frame::MAX_FRAME) {
      this->drop_(buf_.size());
    }
  }

  void step(uint32_t now) {
    if (!window.started)
      window.reset(now);
    this->extract_(now);
    if (pending_ && now - pending_end_ > response_timeout_us) {
      this->count_no_response_();
      pending_ = false;
    }
  }

 protected:
  enum class Kind { NEED_MORE, INVALID, FRAME };

  /// Prova a leggere all'inizio del buffer un frame del tipo indicato.
  Kind try_kind_(bool request, size_t &len) const {
    int n = request ? frame::request_length(buf_.data(), buf_.size())
                    : frame::response_length(buf_.data(), buf_.size());
    if (n == 0)
      return Kind::NEED_MORE;
    if (n < 0 || (size_t) n > frame::MAX_FRAME)
      return Kind::INVALID;
    if (buf_.size() < (size_t) n)
      return Kind::NEED_MORE;
    if (!frame::crc_ok(buf_.data(), n))
      return Kind::INVALID;
    len = (size_t) n;
    return Kind::FRAME;
  }

  void extract_(uint32_t now) {
    while (!buf_.empty()) {
      const bool silent = now - last_rx_ >= gap_us;
      size_t len = 0;
      Kind resp = Kind::INVALID, req = Kind::INVALID;

      if (pending_) {
        resp = this->try_kind_(false, len);
        if (resp == Kind::FRAME) {
          if (frame::matches(pending_req_, this->peek_(len))) {
            this->on_response_(len);
          } else {
            total_orphan_responses++;
            window.errors++;
            this->consume_(len);
          }
          continue;
        }
        req = this->try_kind_(true, len);
        if (req == Kind::FRAME) {  // nuova richiesta: la precedente è rimasta senza risposta
          this->count_no_response_();
          this->on_request_(len);
          continue;
        }
      } else {
        req = this->try_kind_(true, len);
        if (req == Kind::FRAME) {
          this->on_request_(len);
          continue;
        }
        resp = this->try_kind_(false, len);
        if (resp == Kind::FRAME) {  // risposta senza richiesta vista (es. avvio a metà)
          total_orphan_responses++;
          window.errors++;
          this->consume_(len);
          continue;
        }
      }

      if (!silent && (req == Kind::NEED_MORE || resp == Kind::NEED_MORE))
        return;  // aspetta altri byte
      if (silent && frame::crc_ok(buf_.data(), buf_.size()) && buf_.size() <= frame::MAX_FRAME) {
        total_unknown_frames++;  // function code non gestito ma CRC valido
        window.errors++;
        this->consume_(buf_.size());
        continue;
      }
      if (!silent && req == Kind::INVALID && resp == Kind::INVALID && !this->valid_frame_ahead_()) {
        // inizio sporco ma potrebbe completarsi: aspetta il silenzio
        if (buf_.size() < frame::MAX_FRAME)
          return;
      }
      this->drop_(1);
    }
  }

  /// C'è un frame valido (richiesta o risposta) che inizia più avanti nel buffer?
  bool valid_frame_ahead_() const {
    for (size_t k = 1; k + 4 <= buf_.size(); k++) {
      const uint8_t *p = buf_.data() + k;
      size_t rem = buf_.size() - k;
      int a = frame::request_length(p, rem);
      int b = frame::response_length(p, rem);
      if ((a > 0 && (size_t) a <= rem && frame::crc_ok(p, a)) || (b > 0 && (size_t) b <= rem && frame::crc_ok(p, b)))
        return true;
    }
    return false;
  }

  std::vector<uint8_t> peek_(size_t len) const { return std::vector<uint8_t>(buf_.begin(), buf_.begin() + len); }

  void consume_(size_t len) {
    last_frame_us = times_[len - 1];
    seen_traffic = true;
    buf_.erase(buf_.begin(), buf_.begin() + len);
    times_.erase(times_.begin(), times_.begin() + len);
  }

  void drop_(size_t n) {
    if (n > buf_.size())
      n = buf_.size();
    total_dropped_bytes += n;
    window.errors += n;
    buf_.erase(buf_.begin(), buf_.begin() + n);
    times_.erase(times_.begin(), times_.begin() + n);
  }

  void on_request_(size_t len) {
    uint32_t start = times_[0], end = times_[len - 1];
    if (have_last_end_)
      this->add_gap_(start - last_end_);
    pending_req_ = this->peek_(len);
    pending_start_ = start;
    pending_end_ = end;
    pending_ = true;
    this->consume_(len);
    // il gap si misura solo dopo una transazione completa
    have_last_end_ = false;
  }

  void on_response_(size_t len) {
    uint32_t start = times_[0], end = times_[len - 1];
    std::vector<uint8_t> resp = this->peek_(len);
    this->consume_(len);
    pending_ = false;
    total_transactions++;
    window.transactions++;
    window.latency.add(start - pending_end_);
    window.busy_us += end - pending_start_;
    last_end_ = end;
    have_last_end_ = true;

    PollStat &ps = this->poll_for_(pending_req_, pending_start_);
    if (resp[1] & 0x80) {
      ps.exceptions++;
      total_exceptions++;
      this->record_write_(pending_req_, pending_start_, false);
      return;
    }
    this->record_values_(pending_req_, resp);
    this->record_write_(pending_req_, pending_start_, true);
  }

  void count_no_response_() {
    total_no_response++;
    window.no_response++;
    PollStat &ps = this->poll_for_(pending_req_, pending_start_);
    ps.no_response++;
    have_last_end_ = false;
  }

  void add_gap_(uint32_t us) {
    window.gap.add(us);
    uint32_t ms = us / 1000;
    size_t i = 0;
    while (i < GAP_BUCKETS - 1 && ms >= GAP_BUCKETS_MS[i])
      i++;
    window.gap_hist[i]++;
  }

  static PollKey key_of_(const std::vector<uint8_t> &req) {
    PollKey k{req[0], req[1], 0, 0};
    if (req.size() >= 6) {
      k.start = (uint16_t) ((req[2] << 8) | req[3]);
      k.qty = (uint16_t) ((req[4] << 8) | req[5]);
    }
    if (k.fc == 0x05 || k.fc == 0x06)
      k.qty = 1;  // per le scritture singole i byte 4-5 sono il valore, non la quantità
    return k;
  }

  /// Statistiche per tipo di richiesta. Registra l'occorrenza solo alla prima chiamata per richiesta.
  PollStat &poll_for_(const std::vector<uint8_t> &req, uint32_t when) {
    PollKey k = key_of_(req);
    for (auto &p : polls) {
      if (p.key == k) {
        if (p.last_us != when) {
          p.interval.add(when - p.last_us);
          p.last_us = when;
          p.count++;
        }
        return p;
      }
    }
    if (polls.size() >= max_polls) {
      static PollStat overflow;
      overflow = PollStat();
      overflow.key = k;
      return overflow;
    }
    PollStat p;
    p.key = k;
    p.count = 1;
    p.last_us = when;
    polls.push_back(p);
    return polls.back();
  }

  void set_reg_(uint8_t type, uint16_t addr, uint16_t value) {
    uint32_t key = reg_key(type, addr);
    if (registers.size() >= max_registers && registers.find(key) == registers.end())
      return;
    registers[key] = value;
  }

  void record_values_(const std::vector<uint8_t> &req, const std::vector<uint8_t> &resp) {
    PollKey k = key_of_(req);
    switch (k.fc) {
      case 0x03:
      case 0x04: {
        uint8_t type = k.fc == 0x03 ? 3 : 4;
        size_t n = resp[2] / 2;
        for (size_t i = 0; i < n && 3 + 2 * i + 1 < resp.size(); i++)
          this->set_reg_(type, k.start + i, (uint16_t) ((resp[3 + 2 * i] << 8) | resp[4 + 2 * i]));
        break;
      }
      case 0x01:
      case 0x02: {
        uint8_t type = k.fc == 0x01 ? 1 : 2;
        for (uint16_t i = 0; i < k.qty; i++) {
          size_t byte = 3 + i / 8;
          if (byte >= resp.size() - 2)
            break;
          this->set_reg_(type, k.start + i, (resp[byte] >> (i % 8)) & 1);
        }
        break;
      }
      case 0x05:
        this->set_reg_(1, k.start, req[4] == 0xFF ? 1 : 0);
        break;
      case 0x06:
        this->set_reg_(3, k.start, (uint16_t) ((req[4] << 8) | req[5]));
        break;
      case 0x10: {
        for (uint16_t i = 0; i < k.qty && (size_t) (7 + 2 * i + 1) < req.size() - 2; i++)
          this->set_reg_(3, k.start + i, (uint16_t) ((req[7 + 2 * i] << 8) | req[8 + 2 * i]));
        break;
      }
      default:
        break;
    }
  }

  void record_write_(const std::vector<uint8_t> &req, uint32_t when, bool confirmed) {
    uint8_t fc = req[1];
    if (fc != 0x05 && fc != 0x06 && fc != 0x0F && fc != 0x10)
      return;
    WriteEvent w;
    PollKey k = key_of_(req);
    w.time_us = when;
    w.addr = k.addr;
    w.fc = fc;
    w.start = k.start;
    w.qty = k.qty;
    w.confirmed = confirmed;
    if (fc == 0x05) {
      w.values.push_back(req[4] == 0xFF ? 1 : 0);
    } else if (fc == 0x06) {
      w.values.push_back((uint16_t) ((req[4] << 8) | req[5]));
    } else if (fc == 0x10) {
      for (uint16_t i = 0; i < k.qty && (size_t) (7 + 2 * i + 1) < req.size() - 2; i++)
        w.values.push_back((uint16_t) ((req[7 + 2 * i] << 8) | req[8 + 2 * i]));
    } else {
      for (uint16_t i = 0; i < k.qty && (size_t) (7 + i / 8) < req.size() - 2; i++)
        w.values.push_back((req[7 + i / 8] >> (i % 8)) & 1);
    }
    if (writes.size() >= max_write_events)
      writes.erase(writes.begin());
    writes.push_back(w);
  }

  std::vector<uint8_t> buf_;
  std::vector<uint32_t> times_;
  uint32_t last_rx_{0};
  bool pending_{false};
  std::vector<uint8_t> pending_req_;
  uint32_t pending_start_{0}, pending_end_{0};
  bool have_last_end_{false};
  uint32_t last_end_{0};
};

}  // namespace salda_sniffer
}  // namespace esphome
