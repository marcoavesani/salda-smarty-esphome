#pragma once
// Logica del proxy Modbus RTU, senza dipendenze da ESPHome (testata su PC).
//
//   S-Touch (master) --[porta pannello]-- ESP --[porta unità]-- MCB (slave)
//                                          ^
//                        richieste "locali" di modbus_controller (Home Assistant)
//
// L'ESP è l'unico master sul segmento dell'unità, quindi non ci sono collisioni:
// le richieste del pannello vengono inoltrate e le risposte riportate indietro,
// le richieste locali vengono infilate negli spazi liberi.

#include <cstdint>
#include <deque>
#include <vector>

#include "../salda_common/frame.h"

namespace esphome {
namespace salda_proxy {

namespace frame = salda_common::frame;

struct ProxyStats {
  uint32_t panel_requests{0};     // richieste del pannello ricevute
  uint32_t panel_responses{0};    // risposte consegnate al pannello
  uint32_t panel_replaced{0};     // richieste del pannello sostituite da una più recente prima dell'invio
  uint32_t stale_responses{0};    // risposte scartate perché il pannello aveva già ritrasmesso
  uint32_t local_requests{0};     // richieste locali (HA) inviate all'unità
  uint32_t local_responses{0};    // risposte consegnate a modbus_controller
  uint32_t local_dropped{0};      // richieste locali scartate (coda piena)
  uint32_t local_expired{0};      // richieste/risposte locali troppo vecchie (modbus aveva già rinunciato)
  uint32_t late_bytes{0};         // byte arrivati dall'unità fuori tempo (dopo un timeout)
  uint32_t timeouts{0};           // nessuna risposta dall'unità entro il timeout
  uint32_t dropped_bytes{0};      // byte scartati per CRC errato / risincronizzazione
  uint32_t max_panel_latency_ms{0};
  uint32_t last_panel_rx_ms{0};   // ultimo frame valido dal pannello
  uint32_t last_unit_rx_ms{0};    // ultima risposta valida dall'unità
  bool seen_panel{false};
  bool seen_unit{false};
};

class ProxyCore {
 public:
  enum class Origin : uint8_t { NONE, PANEL, LOCAL };

  // --- configurazione -------------------------------------------------------
  uint32_t response_timeout_ms{500};  // attesa massima della risposta dell'unità
  uint32_t gap_ms{20};                // silenzio che chiude/invalida un frame parziale
  uint32_t turnaround_ms{2};          // pausa minima tra fine risposta e richiesta successiva
  uint32_t broadcast_delay_ms{100};   // pausa dopo un broadcast (indirizzo 0)
  uint32_t panel_stale_ms{1000};      // richiesta del pannello troppo vecchia: non inoltrarla
  uint32_t local_max_wait_ms{300};    // dopo questa attesa una richiesta locale passa davanti al pannello
  uint32_t local_expire_ms{1000};     // < send_wait_time di modbus: oltre, modbus ha già rinunciato
  uint32_t late_guard_ms{100};        // dopo un timeout: silenzio richiesto prima di trasmettere ancora
  size_t local_queue_max{8};

  // --- uscite (svuotate dall'adattatore) -------------------------------------
  std::vector<uint8_t> to_panel;  // un frame da scrivere sulla porta pannello
  std::vector<uint8_t> to_unit;   // un frame da scrivere sulla porta unità
  std::deque<uint8_t> to_local;   // byte da far leggere a modbus_controller

  ProxyStats stats;

  // --- ingressi --------------------------------------------------------------
  void on_panel_bytes(const uint8_t *data, size_t len, uint32_t now) {
    panel_buf_.insert(panel_buf_.end(), data, data + len);
    panel_last_rx_ = now;
    if (panel_buf_.size() > 4 * frame::MAX_FRAME) {  // flusso impazzito
      stats.dropped_bytes += panel_buf_.size();
      panel_buf_.clear();
    }
  }

  void on_unit_bytes(const uint8_t *data, size_t len, uint32_t now) {
    if (state_ != State::WAITING) {  // nessuno ha chiesto niente: risposta tardiva o rumore
      stats.late_bytes += len;
      if (state_ == State::GUARD)
        guard_until_ = now + late_guard_ms;  // la linea non è ancora tranquilla
      return;
    }
    unit_buf_.insert(unit_buf_.end(), data, data + len);
    unit_last_rx_ = now;
  }

  /// Una richiesta completa da modbus_controller (una write_array = un frame).
  void queue_local(const uint8_t *data, size_t len, uint32_t now) {
    if (len < 4)
      return;
    if (local_queue_.size() >= local_queue_max) {
      local_queue_.pop_front();
      stats.local_dropped++;
    }
    local_queue_.push_back(Pending{std::vector<uint8_t>(data, data + len), now});
  }

  // --- passo della macchina a stati -------------------------------------------
  void step(uint32_t now) {
    this->parse_panel_(now);

    switch (state_) {
      case State::WAITING:
        this->check_response_(now);
        break;
      case State::BROADCAST:
        if (now - sent_at_ >= broadcast_delay_ms) {
          state_ = State::IDLE;
          idle_since_ = now;
        }
        break;
      case State::GUARD:
        if ((int32_t) (now - guard_until_) >= 0) {
          state_ = State::IDLE;
          idle_since_ = now;
        }
        break;
      case State::IDLE:
        break;
    }

    if (state_ == State::IDLE && to_unit.empty() && now - idle_since_ >= turnaround_ms)
      this->send_next_(now);
  }

  bool busy() const { return state_ != State::IDLE; }
  bool has_panel_pending() const { return panel_pending_; }
  size_t local_queue_size() const { return local_queue_.size(); }

 protected:
  enum class State : uint8_t { IDLE, WAITING, BROADCAST, GUARD };
  struct Pending {
    std::vector<uint8_t> frame;
    uint32_t since;
  };

  void parse_panel_(uint32_t now) {
    std::vector<uint8_t> f;
    size_t dropped = 0;
    while (frame::extract(panel_buf_, true, now - panel_last_rx_, gap_ms, f, dropped) == frame::Result::FRAME) {
      stats.panel_requests++;
      stats.last_panel_rx_ms = now;
      stats.seen_panel = true;
      if (panel_pending_)
        stats.panel_replaced++;
      panel_req_ = Pending{f, now};
      panel_pending_ = true;
      panel_seq_++;
    }
    stats.dropped_bytes += dropped;
  }

  void check_response_(uint32_t now) {
    std::vector<uint8_t> resp;
    size_t dropped = 0;
    while (frame::extract(unit_buf_, false, now - unit_last_rx_, gap_ms, resp, dropped) == frame::Result::FRAME) {
      if (!frame::matches(in_flight_, resp)) {
        stats.dropped_bytes += resp.size();
        continue;
      }
      stats.last_unit_rx_ms = now;
      stats.seen_unit = true;
      if (origin_ == Origin::PANEL) {
        if (in_flight_seq_ == panel_seq_ && !panel_pending_) {
          to_panel = resp;
          stats.panel_responses++;
          uint32_t lat = now - in_flight_since_;
          if (lat > stats.max_panel_latency_ms)
            stats.max_panel_latency_ms = lat;
        } else {
          stats.stale_responses++;  // il pannello ha già ritrasmesso: questa risposta lo confonderebbe
        }
      } else if (now - in_flight_since_ > local_expire_ms) {
        stats.local_expired++;  // modbus è già passato oltre: la risposta finirebbe al comando sbagliato
      } else {
        to_local.insert(to_local.end(), resp.begin(), resp.end());
        stats.local_responses++;
      }
      this->finish_(now);
      stats.dropped_bytes += dropped;
      return;
    }
    stats.dropped_bytes += dropped;
    if (now - sent_at_ >= response_timeout_ms) {
      stats.timeouts++;
      stats.dropped_bytes += unit_buf_.size();
      this->finish_(now);
      // la risposta potrebbe ancora arrivare: aspetta che la linea sia tranquilla
      state_ = State::GUARD;
      guard_until_ = now + late_guard_ms;
    }
  }

  void finish_(uint32_t now) {
    state_ = State::IDLE;
    origin_ = Origin::NONE;
    unit_buf_.clear();
    idle_since_ = now;
  }

  void send_next_(uint32_t now) {
    if (panel_pending_ && now - panel_req_.since > panel_stale_ms) {
      panel_pending_ = false;  // il pannello avrà già rinunciato
      stats.panel_replaced++;
    }
    while (!local_queue_.empty() && now - local_queue_.front().since > local_expire_ms) {
      local_queue_.pop_front();  // modbus ha già rinunciato
      stats.local_expired++;
    }
    bool local_ready = !local_queue_.empty();
    bool local_starving = local_ready && (now - local_queue_.front().since >= local_max_wait_ms) &&
                          last_origin_ == Origin::PANEL;
    if (panel_pending_ && !local_starving) {
      in_flight_ = panel_req_.frame;
      in_flight_since_ = panel_req_.since;
      in_flight_seq_ = panel_seq_;
      panel_pending_ = false;
      origin_ = Origin::PANEL;
    } else if (local_ready) {
      in_flight_ = local_queue_.front().frame;
      in_flight_since_ = local_queue_.front().since;
      local_queue_.pop_front();
      origin_ = Origin::LOCAL;
      stats.local_requests++;
    } else {
      return;
    }
    last_origin_ = origin_;
    to_unit = in_flight_;
    unit_buf_.clear();
    sent_at_ = now;
    if (in_flight_[0] == 0x00) {  // broadcast: nessuna risposta
      state_ = State::BROADCAST;
      origin_ = Origin::NONE;
    } else {
      state_ = State::WAITING;
    }
  }

  State state_{State::IDLE};
  Origin origin_{Origin::NONE};
  Origin last_origin_{Origin::NONE};
  std::vector<uint8_t> panel_buf_, unit_buf_, in_flight_;
  uint32_t panel_last_rx_{0}, unit_last_rx_{0}, sent_at_{0}, idle_since_{0}, in_flight_since_{0}, guard_until_{0};
  Pending panel_req_{};
  bool panel_pending_{false};
  uint32_t panel_seq_{0}, in_flight_seq_{0};
  std::deque<Pending> local_queue_;
};

}  // namespace salda_proxy
}  // namespace esphome
