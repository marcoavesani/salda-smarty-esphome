#include "salda_proxy.h"

#include <algorithm>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace salda_proxy {

static const char *const TAG = "salda_proxy";
static const uint32_t CONNECTED_WINDOW_MS = 15000;

void SaldaProxy::setup() {
  // La UART virtuale "eredita" i parametri della porta verso l'unità.
  this->set_baud_rate(this->unit_->get_baud_rate());
  this->set_data_bits(this->unit_->get_data_bits());
  this->set_stop_bits(this->unit_->get_stop_bits());
  this->set_parity(this->unit_->get_parity());
  this->set_rx_buffer_size(512);
  // Il riconoscimento dei frame si basa anche sul silenzio: il loop deve girare spesso.
  this->high_freq_.start();
}

void SaldaProxy::pump_rx_(uart::UARTComponent *port, bool from_panel, uint32_t now) {
  uint8_t buf[64];
  int avail;
  while ((avail = port->available()) > 0) {
    size_t n = std::min<size_t>((size_t) avail, sizeof(buf));
    if (!port->read_array(buf, n))
      break;
    if (from_panel)
      this->core_.on_panel_bytes(buf, n, now);
    else
      this->core_.on_unit_bytes(buf, n, now);
  }
}

void SaldaProxy::transmit_(uart::UARTComponent *port, std::vector<uint8_t> &frame) {
  ESP_LOGV(TAG, "%s <- %s", port == this->unit_ ? "unità" : "pannello", format_hex_pretty(frame).c_str());
  port->write_array(frame);
  port->flush();  // attende la fine della trasmissione
  // Alcuni transceiver con commutazione automatica rimandano indietro quanto trasmesso:
  // scarta al massimo tanti byte quanti ne abbiamo appena inviati (la risposta non può
  // essere già arrivata, l'altro lato deve prima rispettare il silenzio di fine frame).
  size_t echo = std::min<size_t>((size_t) std::max(port->available(), 0), frame.size());
  uint8_t sink;
  while (echo-- > 0)
    port->read_byte(&sink);
  frame.clear();
}

void SaldaProxy::loop() {
  uint32_t now = millis();
  this->pump_rx_(this->panel_, true, now);
  this->pump_rx_(this->unit_, false, now);

  this->core_.step(now);

  if (!this->core_.to_unit.empty())
    this->transmit_(this->unit_, this->core_.to_unit);
  if (!this->core_.to_panel.empty())
    this->transmit_(this->panel_, this->core_.to_panel);
}

// ---- UART virtuale ----------------------------------------------------------

void SaldaProxy::write_array(const uint8_t *data, size_t len) {
  // Il componente modbus scrive ogni richiesta con una sola write_array.
  this->core_.queue_local(data, len, millis());
}

bool SaldaProxy::peek_byte(uint8_t *data) {
  if (this->core_.to_local.empty())
    return false;
  *data = this->core_.to_local.front();
  return true;
}

bool SaldaProxy::read_array(uint8_t *data, size_t len) {
  if (this->core_.to_local.size() < len)
    return false;
  for (size_t i = 0; i < len; i++) {
    data[i] = this->core_.to_local.front();
    this->core_.to_local.pop_front();
  }
  return true;
}

int SaldaProxy::available() { return (int) this->core_.to_local.size(); }

// ---- Diagnostica ------------------------------------------------------------

void SaldaProxy::update() {
  const ProxyStats &s = this->core_.stats;
  uint32_t now = millis();
  if (this->panel_requests_ != nullptr)
    this->panel_requests_->publish_state(s.panel_requests);
  if (this->local_requests_ != nullptr)
    this->local_requests_->publish_state(s.local_requests);
  if (this->timeouts_ != nullptr)
    this->timeouts_->publish_state(s.timeouts);
  if (this->errors_ != nullptr)
    this->errors_->publish_state(s.dropped_bytes + s.late_bytes + s.stale_responses + s.local_expired);
  if (this->panel_latency_ != nullptr) {
    this->panel_latency_->publish_state(s.max_panel_latency_ms);
    this->core_.stats.max_panel_latency_ms = 0;  // massimo per intervallo
  }
  if (this->panel_connected_ != nullptr)
    this->panel_connected_->publish_state(s.seen_panel && now - s.last_panel_rx_ms < CONNECTED_WINDOW_MS);
  if (this->unit_connected_ != nullptr)
    this->unit_connected_->publish_state(s.seen_unit && now - s.last_unit_rx_ms < CONNECTED_WINDOW_MS);

  ESP_LOGD(TAG,
           "pannello: %u rich., %u risp. | HA: %u rich., %u risp., %u scadute | timeout %u, "
           "byte scartati %u, tardivi %u, risposte obsolete %u",
           (unsigned) s.panel_requests, (unsigned) s.panel_responses, (unsigned) s.local_requests,
           (unsigned) s.local_responses, (unsigned) s.local_expired, (unsigned) s.timeouts,
           (unsigned) s.dropped_bytes, (unsigned) s.late_bytes, (unsigned) s.stale_responses);
}

void SaldaProxy::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Salda proxy Modbus RTU:\n"
                "  Timeout risposta unità: %u ms\n"
                "  Scadenza richieste HA: %u ms\n"
                "  Silenzio fine frame: %u ms\n"
                "  UART virtuale: %u baud",
                (unsigned) this->core_.response_timeout_ms, (unsigned) this->core_.local_expire_ms,
                (unsigned) this->core_.gap_ms, (unsigned) this->get_baud_rate());
  // Nota: send_wait_time del blocco modbus deve essere maggiore di local_expire.
}

}  // namespace salda_proxy
}  // namespace esphome
