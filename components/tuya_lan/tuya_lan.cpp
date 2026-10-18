#include "tuya_lan.h"
#include "dp_state.h"

#include "esphome/core/log.h"

#ifdef USE_ESP32
#include <cstring>
#include <ctime>

#include <esp_random.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include "esphome/components/json/json_util.h"
#endif

namespace esphome {
namespace tuya_lan {

static const char *const TAG = "tuya_lan";

static const char *version_name(proto::Version v) {
  switch (v) {
    case proto::Version::V33: return "3.3";
    case proto::Version::V34: return "3.4";
    case proto::Version::V35: return "3.5";
  }
  return "?";
}

void TuyaLanDevice::dump_config() {
  ESP_LOGCONFIG(TAG, "Tuya LAN device:");
  ESP_LOGCONFIG(TAG, "  Host: %s:%u", this->host_.c_str(), this->port_);
  ESP_LOGCONFIG(TAG, "  Protocol version: %s", version_name(this->version_));
  ESP_LOGCONFIG(TAG, "  Poll interval: %u ms", this->poll_interval_);
  // Never log device_id or local_key: they are secrets.
}

#ifdef USE_ESP32

void TuyaLanDevice::setup() {
  if (this->local_key_.size() != 16) {
    ESP_LOGE(TAG, "local_key must be exactly 16 bytes, got %u", (unsigned) this->local_key_.size());
    this->mark_failed();
    return;
  }
  this->rx_queue_ = xQueueCreate(16, sizeof(DpUpdate));
  this->tx_queue_ = xQueueCreate(8, sizeof(Command));
  if (this->rx_queue_ == nullptr || this->tx_queue_ == nullptr) {
    ESP_LOGE(TAG, "failed to allocate queues");
    this->mark_failed();
    return;
  }
  // 8 KB stack: sockets + mbedtls GCM/ECB contexts fit (verify the high-water
  // mark with uxTaskGetStackHighWaterMark during the first live test).
  if (xTaskCreate(TuyaLanDevice::task_entry, "tuya_lan", 8192, this, tskIDLE_PRIORITY + 1,
                  &this->task_) != pdPASS) {
    ESP_LOGE(TAG, "failed to start network task");
    this->mark_failed();
  }
}

void TuyaLanDevice::loop() {
  // Drain decoded DP updates and publish on the main loop (entity-safe thread).
  DpUpdate u;
  while (this->rx_queue_ != nullptr && xQueueReceive(this->rx_queue_, &u, 0) == pdTRUE) {
    DpValue v{};
    v.type = static_cast<DpValue::Type>(u.type);
    if (v.type == DpValue::BOOL)
      v.b = u.value != 0;
    else
      v.i = u.value;
    for (auto &l : this->listeners_)
      if (l.dp == u.dp)
        l.cb(v);
  }

  bool online = this->online_.load();
  if (online != this->last_published_online_) {
    this->last_published_online_ = online;
    for (auto &cb : this->online_callbacks_)
      cb(online);
  }
}

void TuyaLanDevice::set_bool_dp(uint8_t dp, bool value) {
  Command c{Command::SET, dp, value ? 1 : 0};
  if (this->tx_queue_ != nullptr && xQueueSend(this->tx_queue_, &c, 0) != pdTRUE)
    ESP_LOGW(TAG, "command queue full, dropping write to DP %u", dp);
}

void TuyaLanDevice::release_dp(uint8_t dp) {
  Command c{Command::CLEAR, dp, 0};
  if (this->tx_queue_ != nullptr && xQueueSend(this->tx_queue_, &c, 0) != pdTRUE)
    ESP_LOGW(TAG, "command queue full, dropping release of DP %u", dp);
}

void TuyaLanDevice::release_all() {
  Command c{Command::CLEAR_ALL, 0, 0};
  if (this->tx_queue_ != nullptr && xQueueSend(this->tx_queue_, &c, 0) != pdTRUE)
    ESP_LOGW(TAG, "command queue full, dropping release-all");
}

void TuyaLanDevice::task_entry(void *arg) { static_cast<TuyaLanDevice *>(arg)->task_loop(); }

void TuyaLanDevice::task_loop() {
  const uint8_t *key = reinterpret_cast<const uint8_t *>(this->local_key_.data());

  while (true) {
    this->drain_commands_();  // keep desired_ current even while reconnecting
    if (this->sock_ < 0) {
      if (!this->connect_()) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }
      // Fresh codec per connection; 3.4/3.5 session keys do not survive a reconnect.
      this->seqno_ = 1;
      this->negotiated_ = false;
      this->rx_buf_.clear();
      this->confirmed_.clear();  // plug state is unknown until it reports again
      this->last_assert_ = 0;
      this->codec_ = std::make_unique<proto::Codec>(this->version_, key);
    }

    // (Re)negotiate the session key for 3.4/3.5 before any data exchange.
    if ((this->version_ == proto::Version::V34 || this->version_ == proto::Version::V35) &&
        !this->negotiated_) {
      if (!this->negotiate_session_()) {
        ESP_LOGW(TAG, "session key negotiation failed, reconnecting");
        this->disconnect_();
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
      this->negotiated_ = true;
    }

    uint32_t last_poll = 0, last_hb = 0, last_rx = millis();
    // Declare the device dead after it misses roughly two polls (never less than
    // 30 s). With 3.3 empty frames no longer counting as alive (CRC is unkeyed),
    // liveness rides on the polled status reports, so the window must clear a
    // couple of poll intervals to avoid false disconnects.
    const uint32_t liveness_ms =
        this->poll_interval_ * 2 + 5000 > 30000 ? this->poll_interval_ * 2 + 5000 : 30000;

    while (this->sock_ >= 0) {
      const uint32_t now = millis();

      this->drain_commands_();
      this->assert_desired_(now);  // may disconnect on send failure
      if (this->sock_ < 0)
        break;

      if (now - last_poll >= this->poll_interval_ || last_poll == 0) {
        last_poll = now;
        std::string query;
        uint32_t cmd;
        if (this->version_ == proto::Version::V33) {
          query = "{\"gwId\":\"" + this->device_id_ + "\",\"devId\":\"" + this->device_id_ +
                  "\",\"uid\":\"" + this->device_id_ + "\",\"t\":\"" + this->timestamp_() + "\"}";
          cmd = proto::CMD_DP_QUERY;
        } else {
          query = "{}";
          cmd = proto::CMD_DP_QUERY_NEW;
        }
        if (!this->send_command_(cmd, query)) {
          this->disconnect_();
          break;
        }
      }

      if (now - last_hb >= 10000) {
        last_hb = now;
        // tinytuya's default heartbeat payload; bare "{}" is accepted by most
        // firmwares but this matches the reference exactly.
        std::string hb = "{\"gwId\":\"" + this->device_id_ + "\",\"devId\":\"" + this->device_id_ + "\"}";
        if (!this->send_command_(proto::CMD_HEART_BEAT, hb)) {
          this->disconnect_();
          break;
        }
      }

      // Read whatever is available (blocks up to 1 s via SO_RCVTIMEO).
      uint8_t buf[512];
      int n = ::recv(this->sock_, buf, sizeof(buf), 0);
      if (n > 0) {
        this->rx_buf_.insert(this->rx_buf_.end(), buf, buf + n);
        if (this->pump_rx_()) {  // only authenticated frames count as "alive"
          last_rx = now;
          this->online_.store(true);
        }
      } else if (n == 0) {
        ESP_LOGW(TAG, "connection closed by device");
        this->disconnect_();
      } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGW(TAG, "recv error %d, reconnecting", errno);
        this->disconnect_();
      }

      if (this->sock_ >= 0 && now - last_rx > liveness_ms) {
        ESP_LOGW(TAG, "no authenticated data for %u ms, reconnecting", liveness_ms);
        this->disconnect_();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(3000));  // backoff so a flapping plug is not hammered
  }
}

bool TuyaLanDevice::connect_() {
  struct addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *res = nullptr;
  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%u", this->port_);
  if (::getaddrinfo(this->host_.c_str(), port_str, &hints, &res) != 0 || res == nullptr) {
    ESP_LOGW(TAG, "DNS resolution failed for %s", this->host_.c_str());
    return false;
  }

  int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0) {
    ::freeaddrinfo(res);
    return false;
  }

  // Non-blocking connect with a 5 s bound, then switch back to blocking.
  int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
  bool ok = (rc == 0);
  if (rc < 0 && errno == EINPROGRESS) {
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    struct timeval tv{5, 0};
    if (::select(fd + 1, nullptr, &wset, nullptr, &tv) > 0) {
      int err = 0;
      socklen_t len = sizeof(err);
      ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
      ok = (err == 0);
    }
  }
  ::freeaddrinfo(res);

  if (!ok) {
    ESP_LOGW(TAG, "connect to %s:%u failed", this->host_.c_str(), this->port_);
    ::close(fd);
    return false;
  }

  ::fcntl(fd, F_SETFL, flags);  // back to blocking
  struct timeval rto{1, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rto, sizeof(rto));
  struct timeval sto{5, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sto, sizeof(sto));
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  this->sock_ = fd;
  ESP_LOGI(TAG, "connected to %s:%u", this->host_.c_str(), this->port_);
  return true;
}

void TuyaLanDevice::disconnect_() {
  if (this->sock_ >= 0) {
    ::close(this->sock_);
    this->sock_ = -1;
  }
  this->negotiated_ = false;
  this->rx_buf_.clear();
  this->confirmed_.clear();     // plug state is unknown once the link is gone
  this->online_.store(false);   // never report control we no longer have
}

bool TuyaLanDevice::send_frame_(const proto::Bytes &frame) {
  size_t sent = 0;
  while (sent < frame.size()) {
    int n = ::send(this->sock_, frame.data() + sent, frame.size() - sent, 0);
    if (n <= 0)
      return false;
    sent += n;
  }
  return true;
}

bool TuyaLanDevice::pump_rx_() {
  bool any_ok = false;
  proto::Bytes frame;
  while (proto::extract_frame(this->rx_buf_, frame))
    any_ok |= this->handle_frame_(frame);
  return any_ok;
}

bool TuyaLanDevice::negotiate_session_() {
  // Hardware RNG for the client nonce and (3.5) the per-frame GCM nonces.
  uint8_t nonce[16];
  esp_fill_random(nonce, sizeof(nonce));
  uint8_t iv1[12], iv2[12];
  const uint8_t *iv1p = nullptr, *iv2p = nullptr;
  if (this->version_ == proto::Version::V35) {
    esp_fill_random(iv1, sizeof(iv1));
    esp_fill_random(iv2, sizeof(iv2));
    iv1p = iv1;
    iv2p = iv2;
  }

  if (!this->send_frame_(this->codec_->session_step1(this->seqno_++, nonce, iv1p)))
    return false;

  // Wait up to 3 s for the SESS_KEY_NEG_RESP frame (elapsed form survives the
  // ~49-day millis() wrap).
  const uint32_t start = millis();
  proto::Bytes frame;
  while (millis() - start < 3000) {
    if (proto::extract_frame(this->rx_buf_, frame)) {
      auto d = this->codec_->decode(frame);
      if (!d.ok || d.cmd != proto::CMD_SESS_KEY_NEG_RESP)
        continue;
      proto::Bytes finish = this->codec_->session_step3(d.payload, this->seqno_++, iv2p);
      if (finish.empty()) {
        ESP_LOGW(TAG, "session negotiation: HMAC check failed");
        return false;
      }
      if (!this->send_frame_(finish) || !this->codec_->finalize_session())
        return false;
      ESP_LOGI(TAG, "session key negotiated");
      return true;
    }
    uint8_t buf[256];
    int n = ::recv(this->sock_, buf, sizeof(buf), 0);
    if (n > 0)
      this->rx_buf_.insert(this->rx_buf_.end(), buf, buf + n);
    else if (n == 0)
      return false;
    else if (errno != EAGAIN && errno != EWOULDBLOCK)
      return false;
  }
  return false;
}

bool TuyaLanDevice::send_command_(uint8_t cmd, const std::string &json) {
  uint8_t iv[12];
  const uint8_t *ivp = nullptr;
  if (this->version_ == proto::Version::V35) {
    esp_fill_random(iv, sizeof(iv));  // fresh GCM nonce per frame
    ivp = iv;
  }
  proto::Bytes frame = this->codec_->encode(cmd, json, this->seqno_++, ivp);
  if (frame.empty())
    return false;
  return this->send_frame_(frame);
}

std::string TuyaLanDevice::timestamp_() const {
  return std::to_string((uint32_t) ::time(nullptr));
}

bool TuyaLanDevice::apply_dp_(uint8_t dp, bool value) {
  char dps[48];
  snprintf(dps, sizeof(dps), "{\"%u\":%s}", dp, value ? "true" : "false");
  if (this->version_ == proto::Version::V33) {
    std::string json = "{\"devId\":\"" + this->device_id_ + "\",\"uid\":\"" + this->device_id_ +
                       "\",\"t\":\"" + this->timestamp_() + "\",\"dps\":" + dps + "}";
    return this->send_command_(proto::CMD_CONTROL, json);
  }
  std::string json = "{\"protocol\":5,\"t\":" + this->timestamp_() + ",\"data\":{\"dps\":" + dps + "}}";
  return this->send_command_(proto::CMD_CONTROL_NEW, json);
}

void TuyaLanDevice::drain_commands_() {
  // Collapse all queued intents into the latest value per DP, so a burst (or a
  // backlog built up while offline) never replays stale commands.
  Command c;
  while (xQueueReceive(this->tx_queue_, &c, 0) == pdTRUE) {
    switch (c.op) {
      case Command::SET: {
        bool v = c.value != 0;
        if (this->desired_.find(c.dp) == this->desired_.end() || this->desired_[c.dp] != v)
          this->last_assert_ = 0;  // new intent: assert without waiting
        this->desired_[c.dp] = v;
        break;
      }
      case Command::CLEAR:
        this->desired_.erase(c.dp);  // stop re-asserting this DP
        break;
      case Command::CLEAR_ALL:
        this->desired_.clear();
        break;
    }
  }
}

void TuyaLanDevice::assert_desired_(uint32_t now) {
  if (this->sock_ < 0)
    return;
  auto needing = dps_needing_assert(this->desired_, this->confirmed_);
  this->pending_writes_.store(!needing.empty());  // exposed via has_pending_writes()
  if (needing.empty())
    return;
  // Re-send any DP the plug has not confirmed at the value we want. Retry at
  // most every 2 s so a plug that never confirms is not flooded.
  if (this->last_assert_ != 0 && now - this->last_assert_ < 2000)
    return;
  this->last_assert_ = now == 0 ? 1 : now;
  for (uint8_t dp : needing) {
    if (!this->apply_dp_(dp, this->desired_[dp])) {
      this->disconnect_();  // send failed: reconnect, then re-assert from scratch
      return;
    }
  }
}

void TuyaLanDevice::publish_dps_(const std::string &json) {
  json::parse_json(json, [this](JsonObject root) -> bool {
    JsonObject dps = root["dps"].as<JsonObject>();
    if (dps.isNull())
      dps = root["data"]["dps"].as<JsonObject>();
    if (dps.isNull())
      return false;
    for (JsonPair kv : dps) {
      long dp_id = atol(kv.key().c_str());
      if (!valid_dp_id(dp_id))
        continue;
      uint8_t dp = (uint8_t) dp_id;
      JsonVariant val = kv.value();
      DpUpdate u{dp, DpValue::INT, 0};
      if (val.is<bool>()) {
        u.type = DpValue::BOOL;
        u.value = val.as<bool>() ? 1 : 0;
        this->confirmed_[dp] = (u.value != 0);  // closes the write-retry loop
      } else if (val.is<long long>()) {
        u.value = (int32_t) val.as<long long>();
      } else {
        continue;  // ignore strings/null/float DPs (not supported yet)
      }
      xQueueSend(this->rx_queue_, &u, 0);
    }
    return true;
  });
}

bool TuyaLanDevice::handle_frame_(const proto::Bytes &frame) {
  auto d = this->codec_->decode(frame);
  if (!d.ok) {
    ESP_LOGW(TAG, "dropped an undecodable frame (%u bytes)", (unsigned) frame.size());
    return false;
  }
  // Any decoded payload that looks like a JSON object may carry DP state.
  if (!d.payload.empty() && d.payload[0] == '{')
    this->publish_dps_(d.payload);
  // A 3.3 frame is authenticated only by an unkeyed CRC, so an empty
  // (retcode-only) frame proves nothing about the sender — do not let it mark
  // the device alive, or anyone on-path on the LAN could forge liveness. A
  // non-empty 3.3 frame and every 3.4/3.5 frame (HMAC / GCM) do count.
  return !(this->version_ == proto::Version::V33 && d.payload.empty());
}

#else  // !USE_ESP32 — the network path requires FreeRTOS + lwip.

void TuyaLanDevice::setup() {
  ESP_LOGE(TAG, "tuya_lan requires an ESP32 target");
  this->mark_failed();
}
void TuyaLanDevice::loop() {}
void TuyaLanDevice::set_bool_dp(uint8_t, bool) {}
void TuyaLanDevice::release_dp(uint8_t) {}
void TuyaLanDevice::release_all() {}

#endif

}  // namespace tuya_lan
}  // namespace esphome
