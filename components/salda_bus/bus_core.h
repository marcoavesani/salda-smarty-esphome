#pragma once
// Bus condiviso: l'ESP sta sulla stessa linea RS485 dell'S-Touch (unico altro master).
//
//  - Ascolta tutto il traffico (SnifferCore) e tiene una cache dei valori che il
//    pannello legge già: le richieste di modbus_controller per registri freschi in
//    cache ricevono subito una risposta sintetizzata, senza toccare il bus.
//  - I registri mancanti vengono letti dall'unità in blocchi piccoli, e le scritture
//    inviate, solo in uno "slot": subito dopo che il pannello ha ricevuto una
//    risposta, quando il pannello sta aspettando prima della richiesta successiva
//    (misurato sul campo: 34..100 ms). Ogni transazione nostra deve stare in
//    `budget_us`. Se il pannello tace da `idle_us`, il bus è libero.
//  - Una risposta viene accettata solo se corrisponde alla nostra richiesta.
// Senza dipendenze da ESPHome: testato su PC (tests/test_bus.cpp).

#include <cstdint>
#include <deque>
#include <vector>

#include "../salda_common/frame.h"
#include "../salda_common/sniffer_core.h"

namespace esphome {
namespace salda_bus {

namespace frame = salda_common::frame;
using salda_common::SnifferCore;

struct BusStats {
  uint32_t local_requests{0};     // richieste ricevute da modbus_controller
  uint32_t served_from_cache{0};  // risposte interamente dalla cache
  uint32_t served_from_bus{0};    // risposte che hanno richiesto almeno una transazione
  uint32_t writes{0};             // scritture inviate all'unità
  uint32_t chunks_sent{0};        // nostre transazioni sul bus
  uint32_t chunk_retries{0};      // ritrasmissioni (collisione o nessuna risposta)
  uint32_t local_failed{0};       // richieste abbandonate dopo i tentativi
  uint32_t local_expired{0};      // richieste troppo vecchie (modbus aveva già rinunciato)
  uint32_t exceptions{0};         // eccezioni Modbus ricevute dall'unità
  uint32_t oversize_sent{0};      // transazioni oltre il budget inviate dopo lunga attesa
};

class BusCore {
 public:
  // --- configurazione -------------------------------------------------------
  uint32_t byte_us{573};              // durata di un carattere (11 bit a 19200 baud)
  uint32_t guard_us{2500};            // silenzio minimo dopo l'ultima risposta prima di trasmettere
  uint32_t slot_window_us{12000};     // si trasmette solo entro questo tempo dalla fine della risposta al pannello
  uint32_t budget_us{30000};          // durata massima stimata di una nostra transazione col pannello attivo
  uint32_t max_latency_us{16000};     // latenza massima dell'unità usata nella stima
  uint32_t idle_us{600000};           // pannello in silenzio da tanto: bus libero
  uint32_t response_timeout_us{80000};
  uint32_t max_age_us{10000000};      // età massima di un valore in cache
  uint32_t local_expire_us{4000000};  // < send_wait_time di modbus
  uint32_t oversize_wait_us{2000000}; // dopo questa attesa una transazione troppo lunga parte comunque in uno slot
  uint16_t max_regs{4};               // registri per lettura (FC03/FC04)
  uint16_t max_bits{64};              // bit per lettura (FC01/FC02)
  uint8_t max_retries{3};

  SnifferCore sniffer;
  BusStats stats;

  // --- uscite ------------------------------------------------------------------
  std::vector<uint8_t> to_bus;   // frame da trasmettere adesso
  std::deque<uint8_t> to_local;  // byte da far leggere a modbus_controller

  // --- ingressi --------------------------------------------------------------
  void on_bus_bytes(const uint8_t *data, size_t len, uint32_t now) { sniffer.on_bytes(data, len, now); }

  /// Una richiesta completa da modbus_controller (una write_array = un frame).
  void queue_local(const uint8_t *data, size_t len, uint32_t now) {
    if (len < 8 || !frame::crc_ok(data, len))
      return;
    if (queue_.size() >= 8) {
      queue_.pop_front();
      stats.local_expired++;
    }
    queue_.push_back(Pending{std::vector<uint8_t>(data, data + len), now});
  }

  void step(uint32_t now) {
    sniffer.response_timeout_us = response_timeout_us;
    sniffer.step(now);

    // scarta le richieste che modbus_controller ha già dato per perse
    while (!queue_.empty() && now - queue_.front().since > local_expire_us) {
      queue_.pop_front();
      stats.local_expired++;
    }
    if (active_ && !waiting_ && now - cur_.since > local_expire_us) {
      active_ = false;
      stats.local_expired++;
    }

    if (!active_ && !queue_.empty()) {
      this->activate_(queue_.front(), now);
      queue_.pop_front();
    }
    if (!active_)
      return;

    if (waiting_) {
      this->check_outcome_(now);
      if (waiting_ || !active_)
        return;
    }
    if (next_ < chunks_.size() && this->can_send_(chunks_[next_], now))
      this->send_(now);
  }

  bool busy() const { return active_; }
  size_t queue_size() const { return queue_.size(); }

  /// Stima della durata di una transazione (richiesta + latenza massima + risposta).
  uint32_t estimate_us(const std::vector<uint8_t> &req) const {
    return (uint32_t) (req.size() + response_len_(req)) * byte_us + max_latency_us;
  }

 protected:
  struct Pending {
    std::vector<uint8_t> frame;
    uint32_t since;
  };

  static size_t response_len_(const std::vector<uint8_t> &req) {
    const uint8_t fc = req[1];
    const uint16_t qty = (uint16_t) ((req[4] << 8) | req[5]);
    switch (fc) {
      case 0x03: case 0x04: return 5 + 2 * qty;
      case 0x01: case 0x02: return 5 + (qty + 7) / 8;
      default: return 8;
    }
  }

  static std::vector<uint8_t> read_req_(uint8_t addr, uint8_t fc, uint16_t start, uint16_t qty) {
    std::vector<uint8_t> f = {addr, fc, (uint8_t) (start >> 8), (uint8_t) start, (uint8_t) (qty >> 8), (uint8_t) qty};
    frame::append_crc(f);
    return f;
  }

  void activate_(const Pending &p, uint32_t now) {
    cur_ = p;
    active_ = true;
    waiting_ = false;
    retries_ = 0;
    next_ = 0;
    chunks_.clear();
    stats.local_requests++;
    const std::vector<uint8_t> &f = cur_.frame;
    addr_ = f[0];
    fc_ = f[1];
    start_ = (uint16_t) ((f[2] << 8) | f[3]);
    qty_ = (uint16_t) ((f[4] << 8) | f[5]);
    is_read_ = fc_ >= 0x01 && fc_ <= 0x04;

    if (!is_read_) {  // scrittura (o function code non gestito): così com'è
      chunks_.push_back(f);
      stats.writes++;
      return;
    }
    const uint8_t type = SnifferCore::type_of_fc(fc_);
    const uint16_t step = (fc_ == 0x03 || fc_ == 0x04) ? max_regs : max_bits;
    for (uint32_t s = start_; s < (uint32_t) start_ + qty_; s += step) {
      const uint16_t n = (uint16_t) std::min<uint32_t>(step, (uint32_t) start_ + qty_ - s);
      bool stale = false;
      for (uint16_t i = 0; i < n && !stale; i++)
        stale = !sniffer.fresh(type, (uint16_t) (s + i), now, max_age_us);
      if (stale)
        chunks_.push_back(read_req_(addr_, fc_, (uint16_t) s, n));
    }
    if (chunks_.empty()) {
      stats.served_from_cache++;
      this->answer_from_cache_(now);
    }
  }

  bool can_send_(const std::vector<uint8_t> &req, uint32_t now) const {
    if (!sniffer.buffer_empty() || sniffer.request_pending())
      return false;  // c'è traffico in corso o il pannello aspetta una risposta
    const uint32_t since_rx = now - sniffer.last_rx_us();
    if (since_rx < guard_us)
      return false;
    if (!sniffer.seen_panel || now - sniffer.last_panel_us >= idle_us)
      return true;  // pannello assente o fermo (es. nei menu): bus libero
    // Slot: l'ultima cosa sul bus è la risposta dell'unità al pannello, appena finita.
    const bool slot = sniffer.transaction_seq > 0 && !sniffer.last_transaction_ours &&
                      sniffer.last_transaction_end_us == sniffer.last_rx_us() && since_rx <= slot_window_us;
    if (!slot)
      return false;
    if (this->estimate_us(req) <= budget_us)
      return true;
    return now - cur_.since >= oversize_wait_us;
  }

  void send_(uint32_t now) {
    const std::vector<uint8_t> &req = chunks_[next_];
    if (this->estimate_us(req) > budget_us && sniffer.seen_panel && now - sniffer.last_panel_us < idle_us)
      stats.oversize_sent++;
    to_bus = req;
    sent_at_ = now;
    seq_at_send_ = sniffer.transaction_seq;
    failed_at_send_ = sniffer.ours_failed_seq;
    sniffer.inject_request(req, now, now + (uint32_t) req.size() * byte_us);
    waiting_ = true;
    stats.chunks_sent++;
  }

  void check_outcome_(uint32_t now) {
    if (sniffer.transaction_seq != seq_at_send_ && sniffer.last_transaction_ours) {
      waiting_ = false;
      const std::vector<uint8_t> &resp = sniffer.last_response;
      if (resp[1] & 0x80) {  // eccezione: la giriamo a modbus_controller
        stats.exceptions++;
        std::vector<uint8_t> r = {addr_, (uint8_t) (fc_ | 0x80), resp[2]};
        frame::append_crc(r);
        this->deliver_(r);
        return;
      }
      if (!is_read_) {
        this->deliver_(resp);
        return;
      }
      retries_ = 0;
      next_++;
      if (next_ >= chunks_.size()) {
        stats.served_from_bus++;
        this->answer_from_cache_(now);
      }
      return;
    }
    const bool failed = sniffer.ours_failed_seq != failed_at_send_;
    if (!failed && now - sent_at_ <= response_timeout_us + 20000)
      return;  // ancora in attesa
    waiting_ = false;
    stats.chunk_retries++;
    if (++retries_ > max_retries) {
      active_ = false;
      stats.local_failed++;
    }
  }

  void answer_from_cache_(uint32_t now) {
    const uint8_t type = SnifferCore::type_of_fc(fc_);
    std::vector<uint8_t> r = {addr_, fc_};
    if (fc_ == 0x03 || fc_ == 0x04) {
      r.push_back((uint8_t) (2 * qty_));
      for (uint16_t i = 0; i < qty_; i++) {
        uint16_t v = 0;
        // dopo le letture i valori sono freschi; uno mancante (improbabile) vale 0
        sniffer.fresh(type, (uint16_t) (start_ + i), now, 0xFFFFFFFF, &v);
        r.push_back(v >> 8);
        r.push_back(v & 0xFF);
      }
    } else {
      const uint16_t nbytes = (qty_ + 7) / 8;
      r.push_back((uint8_t) nbytes);
      r.resize(3 + nbytes, 0);
      for (uint16_t i = 0; i < qty_; i++) {
        uint16_t v = 0;
        sniffer.fresh(type, (uint16_t) (start_ + i), now, 0xFFFFFFFF, &v);
        if (v)
          r[3 + i / 8] |= (uint8_t) (1 << (i % 8));
      }
    }
    frame::append_crc(r);
    this->deliver_(r);
  }

  void deliver_(const std::vector<uint8_t> &r) {
    to_local.insert(to_local.end(), r.begin(), r.end());
    active_ = false;
  }

  std::deque<Pending> queue_;
  Pending cur_{};
  bool active_{false};
  bool waiting_{false};
  bool is_read_{false};
  uint8_t addr_{0}, fc_{0};
  uint16_t start_{0}, qty_{0};
  std::vector<std::vector<uint8_t>> chunks_;
  size_t next_{0};
  uint8_t retries_{0};
  uint32_t sent_at_{0}, seq_at_send_{0}, failed_at_send_{0};
};

}  // namespace salda_bus
}  // namespace esphome
