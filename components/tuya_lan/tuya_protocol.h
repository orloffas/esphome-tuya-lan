// Tuya LAN protocol codec: framing + crypto for protocol versions 3.3 / 3.4 / 3.5.
//
// This module is deliberately free of any ESPHome dependency so it can be
// compiled and unit-tested on a host machine (see tests/host/). The byte layout
// is cross-checked against jasonacox/tinytuya (MIT) frame-for-frame.
//
// Crypto is provided by mbedtls, which is available both in ESP-IDF and from
// Homebrew on macOS, so the same source compiles in both places.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace tuya_lan {
namespace proto {

using Bytes = std::vector<uint8_t>;

enum class Version { V33, V34, V35 };

// Tuya command types (subset we use). Reference: tuya lan_protocol.h.
enum : uint32_t {
  CMD_CONTROL = 7,            // 0x07 set DP values (3.3)
  CMD_STATUS = 8,            // 0x08
  CMD_HEART_BEAT = 9,        // 0x09
  CMD_DP_QUERY = 0x0a,       // 10 query DPs (3.3)
  CMD_CONTROL_NEW = 0x0d,    // 13 set DP values (3.4 / 3.5)
  CMD_DP_QUERY_NEW = 0x10,   // 16 query DPs (3.4 / 3.5)
  CMD_SESS_KEY_NEG_START = 3,
  CMD_SESS_KEY_NEG_RESP = 4,
  CMD_SESS_KEY_NEG_FINISH = 5,
};

// --- Low-level crypto primitives (thin mbedtls wrappers) -------------------

// AES-128-ECB with PKCS#7 padding. Returns ciphertext.
Bytes aes_ecb_encrypt(const uint8_t key[16], const Bytes &plain);
// AES-128-ECB decrypt. If unpad is true, strips PKCS#7 padding. Returns empty
// on malformed input (length not a multiple of 16, bad padding when unpad set).
Bytes aes_ecb_decrypt(const uint8_t key[16], const Bytes &cipher, bool unpad = true);

// AES-128-GCM encrypt. Produces ciphertext (same length as plain) and 16-byte tag.
bool aes_gcm_encrypt(const uint8_t key[16], const uint8_t iv[12], const uint8_t *aad,
                     size_t aad_len, const Bytes &plain, Bytes &cipher_out, uint8_t tag[16]);
// AES-128-GCM decrypt with tag verification. Returns false if the tag is wrong.
bool aes_gcm_decrypt(const uint8_t key[16], const uint8_t iv[12], const uint8_t *aad,
                     size_t aad_len, const Bytes &cipher, const uint8_t tag[16], Bytes &plain_out);

void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                 uint8_t out[32]);
uint32_t crc32(const uint8_t *data, size_t len);

// --- Decoded frame ---------------------------------------------------------

struct DecodedFrame {
  bool ok = false;        // header parsed, checksum/tag valid, decrypt succeeded
  uint32_t cmd = 0;
  uint32_t seqno = 0;
  uint32_t retcode = 0;
  std::string payload;    // decrypted application bytes (version header stripped)
};

// --- Codec -----------------------------------------------------------------
//
// Holds the protocol version, the device local_key, and (after negotiation for
// 3.4/3.5) the derived session key. Sequence numbers are owned by the caller
// and passed in, so the codec itself stays stateless w.r.t. the socket.
class Codec {
 public:
  Codec(Version version, const uint8_t local_key[16]);

  Version version() const { return version_; }

  // The key currently used for message encryption/HMAC. For 3.3 this is always
  // the local key; for 3.4/3.5 it is the local key until finalize_session() is
  // called, then the session key.
  const uint8_t *active_key() const { return key_; }

  // Build an outgoing frame carrying the given JSON payload for `cmd`.
  // For 3.5, if gcm_iv is non-null it is used (test determinism); otherwise a
  // random nonce is generated internally.
  Bytes encode(uint32_t cmd, const std::string &json_payload, uint32_t seqno,
               const uint8_t *gcm_iv = nullptr);

  // Parse an incoming frame. `data` must contain at least one full frame; extra
  // trailing bytes are ignored.
  DecodedFrame decode(const Bytes &data);

  // --- 3.4 / 3.5 session key negotiation -------------------------------------
  // Step 1: frame to send (SESS_KEY_NEG_START) carrying our 16-byte local nonce.
  // The nonce is stored for later key derivation. `gcm_iv` is the per-frame GCM
  // nonce for 3.5 (independent of the local nonce — reusing the local nonce as
  // the IV would repeat it in the finish frame and break GCM); null picks a
  // random one. Both args take fixed values in tests.
  Bytes session_step1(uint32_t seqno, const uint8_t *local_nonce16 = nullptr,
                      const uint8_t *gcm_iv = nullptr);
  // Step 3: given the decoded SESS_KEY_NEG_RESP payload (48+ bytes, already
  // decrypted by decode()), verify the HMAC and build the SESS_KEY_NEG_FINISH
  // frame. `gcm_iv` as in session_step1(). Returns empty on HMAC mismatch.
  Bytes session_step3(const std::string &resp_payload, uint32_t seqno,
                      const uint8_t *gcm_iv = nullptr);
  // Finalize: derive and install the session key from the two nonces. Call
  // after step 3 has been sent. Returns false if nonces are not set.
  bool finalize_session();

 private:
  Bytes pack_55aa(uint32_t seqno, uint32_t cmd, const Bytes &payload, bool use_hmac);
  Bytes pack_6699(uint32_t seqno, uint32_t cmd, const Bytes &payload, const uint8_t *iv12);
  static bool is_no_header_cmd(uint32_t cmd);

  Version version_;
  uint8_t real_key_[16];       // original device local_key
  uint8_t key_[16];            // active key (session key after negotiation)
  uint8_t local_nonce_[16];
  uint8_t remote_nonce_[16];
  bool have_local_nonce_ = false;
  bool have_remote_nonce_ = false;
};

}  // namespace proto
}  // namespace tuya_lan
}  // namespace esphome
