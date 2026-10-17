#include "tuya_lan.h"

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
  // 8 KB stack: TLS-free sockets + mbedtls GCM/ECB contexts fit comfortably.
  xTaskCreate(TuyaLanDevice::task_entry, "tuya_lan", 8192, this, tskIDLE_PRIORITY + 1, &this->task_);
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
  if (this->tx_queue_ == nullptr)
    return;
  Command c{dp, value ? 1 : 0};
  if (xQueueSend(this->tx_queue_, &c, 0) != pdTRUE)
    ESP_LOGW(TAG, "command queue full, dropping write to DP %u", dp);
}

void TuyaLanDevice::task_entry(void *arg) { static_cast<TuyaLanDevice *>(arg)->task_loop(); }

void TuyaLanDevice::task_loop() {
  const uint8_t *key = reinterpret_cast<const uint8_t *>(this->local_key_.data());

  while (true) {
    if (this->sock_ < 0) {
      if (!this->connect_()) {
        this->online_.store(false);
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }
      // Fresh codec per connection; 3.4/3.5 session keys do not survive a reconnect.
      this->seqno_ = 1;
      this->negotiated_ = false;
      this->rx_buf_.clear();
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

    while (this->sock_ >= 0) {
      const uint32_t now = millis();

      // Outgoing boolean DP writes from the main loop.
      Command c;
      while (xQueueReceive(this->tx_queue_, &c, 0) == pdTRUE) {
        char dps[48];
        snprintf(dps, sizeof(dps), "{\"%u\":%s}", c.dp, c.value ? "true" : "false");
        std::string json;
        if (this->version_ == proto::Version::V33) {
          json = "{\"devId\":\"" + this->device_id_ + "\",\"uid\":\"" + this->device_id_ +
                 "\",\"t\":\"" + std::to_string((uint32_t) ::time(nullptr)) + "\",\"dps\":" + dps + "}";
          this->send_command_(proto::CMD_CONTROL, json);
        } else {
          json = "{\"protocol\":5,\"t\":" + std::to_string((uint32_t) ::time(nullptr)) +
                 ",\"data\":{\"dps\":" + dps + "}}";
          this->send_command_(proto::CMD_CONTROL_NEW, json);
        }
      }

      if (now - last_poll >= this->poll_interval_ || last_poll == 0) {
        last_poll = now;
        if (this->version_ == proto::Version::V33)
          this->send_command_(proto::CMD_DP_QUERY,
                              "{\"gwId\":\"" + this->device_id_ + "\",\"devId\":\"" + this->device_id_ +
                                  "\",\"uid\":\"" + this->device_id_ + "\",\"t\":\"" +
                                  std::to_string((uint32_t) ::time(nullptr)) + "\"}");
        else
          this->send_command_(proto::CMD_DP_QUERY_NEW, "{}");
      }

      if (now - last_hb >= 10000) {
        last_hb = now;
        this->send_command_(proto::CMD_HEART_BEAT, "{}");
      }

      // Read whatever is available (blocks up to 1 s via SO_RCVTIMEO).
      uint8_t buf[512];
      int n = ::recv(this->sock_, buf, sizeof(buf), 0);
      if (n > 0) {
        this->rx_buf_.insert(this->rx_buf_.end(), buf, buf + n);
        this->pump_rx_();
        last_rx = now;
        this->online_.store(true);
      } else if (n == 0) {
        ESP_LOGW(TAG, "connection closed by device");
        this->disconnect_();
      } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGW(TAG, "recv error %d, reconnecting", errno);
        this->disconnect_();
      }

      if (now - last_rx > 30000) {
        ESP_LOGW(TAG, "no data for 30 s, reconnecting");
        this->online_.store(false);
        this->disconnect_();
      }
    }
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

// Extract one complete frame from rx_buf_, resynchronising on garbage.
static bool extract_frame(std::vector<uint8_t> &buf, proto::Bytes &out) {
  while (buf.size() >= 4) {
    uint32_t prefix = (uint32_t(buf[0]) << 24) | (uint32_t(buf[1]) << 16) | (uint32_t(buf[2]) << 8) | buf[3];
    size_t total;
    if (prefix == 0x000055AAu) {
      if (buf.size() < 16)
        return false;
      uint32_t len = (uint32_t(buf[12]) << 24) | (uint32_t(buf[13]) << 16) | (uint32_t(buf[14]) << 8) | buf[15];
      total = 16 + len;
    } else if (prefix == 0x00006699u) {
      if (buf.size() < 18)
        return false;
      uint32_t len = (uint32_t(buf[14]) << 24) | (uint32_t(buf[15]) << 16) | (uint32_t(buf[16]) << 8) | buf[17];
      total = 18 + len + 4;
    } else {
      buf.erase(buf.begin());  // desynced, drop a byte and rescan
      continue;
    }
    if (total > 2048) {  // corrupt length, resync
      buf.erase(buf.begin());
      continue;
    }
    if (buf.size() < total)
      return false;
    out.assign(buf.begin(), buf.begin() + total);
    buf.erase(buf.begin(), buf.begin() + total);
    return true;
  }
  return false;
}

void TuyaLanDevice::pump_rx_() {
  proto::Bytes frame;
  while (extract_frame(this->rx_buf_, frame))
    this->handle_frame_(frame);
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

  // Wait up to 3 s for the SESS_KEY_NEG_RESP frame.
  const uint32_t deadline = millis() + 3000;
  proto::Bytes frame;
  while (millis() < deadline) {
    if (extract_frame(this->rx_buf_, frame)) {
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

void TuyaLanDevice::publish_dps_(const std::string &json) {
  json::parse_json(json, [this](JsonObject root) -> bool {
    JsonObject dps = root["dps"].as<JsonObject>();
    if (dps.isNull())
      dps = root["data"]["dps"].as<JsonObject>();
    if (dps.isNull())
      return false;
    for (JsonPair kv : dps) {
      uint8_t dp = (uint8_t) atoi(kv.key().c_str());
      JsonVariant val = kv.value();
      DpUpdate u{dp, DpValue::INT, 0};
      if (val.is<bool>()) {
        u.type = DpValue::BOOL;
        u.value = val.as<bool>() ? 1 : 0;
      } else {
        u.value = val.as<int32_t>();
      }
      xQueueSend(this->rx_queue_, &u, 0);
    }
    return true;
  });
}

void TuyaLanDevice::handle_frame_(const proto::Bytes &frame) {
  auto d = this->codec_->decode(frame);
  if (!d.ok)
    return;
  // Any decoded payload that looks like a JSON object may carry DP state.
  if (!d.payload.empty() && d.payload[0] == '{')
    this->publish_dps_(d.payload);
}

#else  // !USE_ESP32 — the network path requires FreeRTOS + lwip.

void TuyaLanDevice::setup() {
  ESP_LOGE(TAG, "tuya_lan requires an ESP32 target");
  this->mark_failed();
}
void TuyaLanDevice::loop() {}
void TuyaLanDevice::set_bool_dp(uint8_t, bool) {}

#endif

}  // namespace tuya_lan
}  // namespace esphome
