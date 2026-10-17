#include "tuya_protocol.h"

#include <cstdlib>
#include <cstring>

#include <mbedtls/aes.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>

// ESP_PLATFORM is defined by the ESP-IDF toolchain for every translation unit
// (also under the Arduino-ESP32 framework, which is built on ESP-IDF), while
// this file includes no ESPHome headers so it stays host-buildable.
#ifdef ESP_PLATFORM
#include <esp_random.h>
#endif

namespace esphome {
namespace tuya_lan {
namespace proto {

namespace {

constexpr uint32_t PREFIX_55AA = 0x000055AAu;
constexpr uint32_t SUFFIX_55AA = 0x0000AA55u;
constexpr uint32_t PREFIX_6699 = 0x00006699u;
constexpr uint32_t SUFFIX_6699 = 0x00009966u;

void put_u32_be(Bytes &b, uint32_t v) {
  b.push_back((v >> 24) & 0xff);
  b.push_back((v >> 16) & 0xff);
  b.push_back((v >> 8) & 0xff);
  b.push_back(v & 0xff);
}

void put_u16_be(Bytes &b, uint16_t v) {
  b.push_back((v >> 8) & 0xff);
  b.push_back(v & 0xff);
}

uint32_t get_u32_be(const uint8_t *p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// 3-byte version string + 12 null bytes = 15-byte protocol header.
const char *version_str(Version v) {
  switch (v) {
    case Version::V33: return "3.3";
    case Version::V34: return "3.4";
    case Version::V35: return "3.5";
  }
  return "3.3";
}

Bytes version_header(Version v) {
  Bytes h(version_str(v), version_str(v) + 3);
  h.resize(15, 0);  // pad to 15 with nulls
  return h;
}

}  // namespace

// --- Crypto primitives -----------------------------------------------------

Bytes aes_ecb_encrypt(const uint8_t key[16], const Bytes &plain) {
  // PKCS#7 pad to a multiple of 16.
  Bytes in = plain;
  uint8_t pad = 16 - (in.size() % 16);
  in.insert(in.end(), pad, pad);

  Bytes out(in.size());
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, key, 128);
  for (size_t i = 0; i < in.size(); i += 16)
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in.data() + i, out.data() + i);
  mbedtls_aes_free(&ctx);
  return out;
}

Bytes aes_ecb_decrypt(const uint8_t key[16], const Bytes &cipher, bool unpad) {
  if (cipher.empty() || (cipher.size() % 16) != 0)
    return {};

  Bytes out(cipher.size());
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_dec(&ctx, key, 128);
  for (size_t i = 0; i < cipher.size(); i += 16)
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, cipher.data() + i, out.data() + i);
  mbedtls_aes_free(&ctx);

  if (unpad) {
    uint8_t pad = out.back();
    if (pad < 1 || pad > 16 || pad > out.size())
      return {};
    out.resize(out.size() - pad);
  }
  return out;
}

bool aes_gcm_encrypt(const uint8_t key[16], const uint8_t iv[12], const uint8_t *aad,
                     size_t aad_len, const Bytes &plain, Bytes &cipher_out, uint8_t tag[16]) {
  cipher_out.resize(plain.size());
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  bool ok = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128) == 0 &&
            mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, plain.size(), iv, 12, aad,
                                      aad_len, plain.data(), cipher_out.data(), 16, tag) == 0;
  mbedtls_gcm_free(&ctx);
  return ok;
}

bool aes_gcm_decrypt(const uint8_t key[16], const uint8_t iv[12], const uint8_t *aad,
                     size_t aad_len, const Bytes &cipher, const uint8_t tag[16], Bytes &plain_out) {
  plain_out.resize(cipher.size());
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  bool ok = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128) == 0 &&
            mbedtls_gcm_auth_decrypt(&ctx, cipher.size(), iv, 12, aad, aad_len, tag, 16,
                                     cipher.data(), plain_out.data()) == 0;
  mbedtls_gcm_free(&ctx);
  if (!ok)
    plain_out.clear();
  return ok;
}

void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                 uint8_t out[32]) {
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, key_len, data, data_len, out);
}

// Fill a buffer with random bytes. On ESP this is the hardware RNG; rand() is
// only the host-build fallback for test tooling and never runs on a device.
static void fill_random(uint8_t *p, size_t n) {
#ifdef ESP_PLATFORM
  esp_fill_random(p, n);
#else
  for (size_t i = 0; i < n; i++)
    p[i] = static_cast<uint8_t>(rand());  // NOLINT: host-only fallback
#endif
}

uint32_t crc32(const uint8_t *data, size_t len) {
  // Reflected CRC-32 (zlib / binascii.crc32), poly 0xEDB88320.
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1) + 1));
  }
  return crc ^ 0xFFFFFFFFu;
}

// --- Codec -----------------------------------------------------------------

Codec::Codec(Version version, const uint8_t local_key[16]) : version_(version) {
  std::memcpy(real_key_, local_key, 16);
  std::memcpy(key_, local_key, 16);
}

bool Codec::is_no_header_cmd(uint32_t cmd) {
  switch (cmd) {
    case CMD_DP_QUERY:
    case CMD_DP_QUERY_NEW:
    case CMD_HEART_BEAT:
    case CMD_SESS_KEY_NEG_START:
    case CMD_SESS_KEY_NEG_RESP:
    case CMD_SESS_KEY_NEG_FINISH:
      return true;
    default:
      return false;
  }
}

Bytes Codec::pack_55aa(uint32_t seqno, uint32_t cmd, const Bytes &payload, bool use_hmac) {
  const size_t end_len = use_hmac ? 36 : 8;  // hmac(32)+suffix(4) or crc(4)+suffix(4)
  Bytes out;
  put_u32_be(out, PREFIX_55AA);
  put_u32_be(out, seqno);
  put_u32_be(out, cmd);
  put_u32_be(out, static_cast<uint32_t>(payload.size() + end_len));
  out.insert(out.end(), payload.begin(), payload.end());

  if (use_hmac) {
    uint8_t mac[32];
    hmac_sha256(key_, 16, out.data(), out.size(), mac);
    out.insert(out.end(), mac, mac + 32);
  } else {
    put_u32_be(out, crc32(out.data(), out.size()));
  }
  put_u32_be(out, SUFFIX_55AA);
  return out;
}

Bytes Codec::pack_6699(uint32_t seqno, uint32_t cmd, const Bytes &payload, const uint8_t *iv12) {
  // msg_len counts iv(12) + ciphertext(=payload size) + tag(16); suffix excluded.
  const uint32_t msg_len = static_cast<uint32_t>(payload.size() + 12 + 16);
  Bytes header;
  put_u32_be(header, PREFIX_6699);
  put_u16_be(header, 0);  // unknown
  put_u32_be(header, seqno);
  put_u32_be(header, cmd);
  put_u32_be(header, msg_len);
  // AAD is the header minus the 4-byte prefix (14 bytes).
  const uint8_t *aad = header.data() + 4;
  const size_t aad_len = header.size() - 4;

  Bytes cipher;
  uint8_t tag[16];
  if (!aes_gcm_encrypt(key_, iv12, aad, aad_len, payload, cipher, tag))
    return {};

  Bytes out = header;
  out.insert(out.end(), iv12, iv12 + 12);
  out.insert(out.end(), cipher.begin(), cipher.end());
  out.insert(out.end(), tag, tag + 16);
  put_u32_be(out, SUFFIX_6699);
  return out;
}

Bytes Codec::encode(uint32_t cmd, const std::string &json_payload, uint32_t seqno,
                    const uint8_t *gcm_iv) {
  Bytes payload(json_payload.begin(), json_payload.end());

  if (version_ == Version::V35) {
    if (!is_no_header_cmd(cmd)) {
      Bytes vh = version_header(version_);
      payload.insert(payload.begin(), vh.begin(), vh.end());
    }
    uint8_t iv[12];
    if (gcm_iv != nullptr) {
      std::memcpy(iv, gcm_iv, 12);
    } else {
      // Random 12-byte nonce; transmitted in the frame, so a fresh value per
      // message is required for GCM security and is wire-compatible.
      fill_random(iv, 12);
    }
    return pack_6699(seqno, cmd, payload, iv);
  }

  if (version_ == Version::V34) {
    if (!is_no_header_cmd(cmd)) {
      Bytes vh = version_header(version_);
      payload.insert(payload.begin(), vh.begin(), vh.end());
    }
    Bytes enc = aes_ecb_encrypt(key_, payload);  // header is encrypted for 3.4
    return pack_55aa(seqno, cmd, enc, /*use_hmac=*/true);
  }

  // 3.3: encrypt first, then prepend the (plaintext) version header.
  Bytes enc = aes_ecb_encrypt(key_, payload);
  if (!is_no_header_cmd(cmd)) {
    Bytes vh = version_header(version_);
    enc.insert(enc.begin(), vh.begin(), vh.end());
  }
  return pack_55aa(seqno, cmd, enc, /*use_hmac=*/false);
}

DecodedFrame Codec::decode(const Bytes &data) {
  DecodedFrame f;
  if (data.size() < 4)
    return f;
  uint32_t prefix = get_u32_be(data.data());

  if (prefix == PREFIX_55AA) {
    if (data.size() < 16)
      return f;
    f.seqno = get_u32_be(data.data() + 4);
    f.cmd = get_u32_be(data.data() + 8);
    uint32_t length = get_u32_be(data.data() + 12);
    const size_t msg_end = 16 + length;
    if (data.size() < msg_end)
      return f;

    const bool use_hmac = (version_ != Version::V33);
    const size_t end_len = use_hmac ? 36 : 8;
    const size_t retcode_len = 4;
    if (length < retcode_len + end_len)
      return f;

    // Verify checksum over header + retcode + ciphertext (everything before it).
    const size_t check_len = msg_end - end_len;
    if (use_hmac) {
      uint8_t mac[32];
      hmac_sha256(key_, 16, data.data(), check_len, mac);
      if (std::memcmp(mac, data.data() + check_len, 32) != 0)
        return f;
    } else {
      uint32_t have = crc32(data.data(), check_len);
      if (have != get_u32_be(data.data() + check_len))
        return f;
    }

    f.retcode = get_u32_be(data.data() + 16);
    Bytes raw(data.begin() + 16 + retcode_len, data.begin() + check_len);

    Bytes plain;
    if (version_ == Version::V34) {
      plain = aes_ecb_decrypt(key_, raw, /*unpad=*/true);
      Bytes vh = version_header(version_);
      if (plain.size() >= 15 && std::memcmp(plain.data(), vh.data(), 3) == 0)
        plain.erase(plain.begin(), plain.begin() + 15);
    } else {  // 3.3
      Bytes vh = version_header(version_);
      if (raw.size() >= 15 && std::memcmp(raw.data(), vh.data(), 3) == 0)
        raw.erase(raw.begin(), raw.begin() + 15);
      plain = aes_ecb_decrypt(key_, raw, /*unpad=*/true);
    }
    f.payload.assign(plain.begin(), plain.end());
    f.ok = true;
    return f;
  }

  if (prefix == PREFIX_6699) {
    if (version_ != Version::V35)
      return f;
    const size_t header_len = 18;
    if (data.size() < header_len)
      return f;
    f.seqno = get_u32_be(data.data() + 6);
    f.cmd = get_u32_be(data.data() + 10);
    uint32_t length = get_u32_be(data.data() + 14);
    // length counts iv(12)+ct+tag(16); full frame adds the 4-byte suffix.
    const size_t msg_end = header_len + length;  // end of iv+ct+tag
    if (length < 12 + 16 || data.size() < msg_end + 4)
      return f;

    const uint8_t *iv = data.data() + header_len;
    const size_t ct_len = length - 12 - 16;
    Bytes cipher(data.begin() + header_len + 12, data.begin() + header_len + 12 + ct_len);
    const uint8_t *tag = data.data() + header_len + 12 + ct_len;
    const uint8_t *aad = data.data() + 4;  // header minus prefix (14 bytes)

    Bytes plain;
    if (!aes_gcm_decrypt(key_, iv, aad, header_len - 4, cipher, tag, plain))
      return f;

    // Strip 4-byte retcode prefix, then the version header if present.
    if (plain.size() >= 4) {
      f.retcode = get_u32_be(plain.data());
      plain.erase(plain.begin(), plain.begin() + 4);
    }
    Bytes vh = version_header(version_);
    if (plain.size() >= 15 && std::memcmp(plain.data(), vh.data(), 3) == 0)
      plain.erase(plain.begin(), plain.begin() + 15);

    f.payload.assign(plain.begin(), plain.end());
    f.ok = true;
    return f;
  }

  return f;
}

// --- Session key negotiation (3.4 / 3.5) -----------------------------------

Bytes Codec::session_step1(uint32_t seqno, const uint8_t *local_nonce16, const uint8_t *gcm_iv) {
  if (local_nonce16 != nullptr) {
    std::memcpy(local_nonce_, local_nonce16, 16);
  } else {
    fill_random(local_nonce_, 16);
  }
  have_local_nonce_ = true;
  have_remote_nonce_ = false;
  std::memcpy(key_, real_key_, 16);  // negotiation uses the real local key

  std::string nonce(reinterpret_cast<char *>(local_nonce_), 16);
  return encode(CMD_SESS_KEY_NEG_START, nonce, seqno, gcm_iv);
}

Bytes Codec::session_step3(const std::string &resp_payload, uint32_t seqno, const uint8_t *gcm_iv) {
  if (resp_payload.size() < 48 || !have_local_nonce_)
    return {};
  const uint8_t *p = reinterpret_cast<const uint8_t *>(resp_payload.data());
  std::memcpy(remote_nonce_, p, 16);

  uint8_t check[32];
  hmac_sha256(real_key_, 16, local_nonce_, 16, check);
  if (std::memcmp(check, p + 16, 32) != 0)
    return {};
  have_remote_nonce_ = true;

  uint8_t rkey_hmac[32];
  hmac_sha256(real_key_, 16, remote_nonce_, 16, rkey_hmac);
  std::string finish(reinterpret_cast<char *>(rkey_hmac), 32);
  return encode(CMD_SESS_KEY_NEG_FINISH, finish, seqno, gcm_iv);
}

bool Codec::finalize_session() {
  if (!have_local_nonce_ || !have_remote_nonce_)
    return false;

  uint8_t xored[16];
  for (int i = 0; i < 16; i++)
    xored[i] = local_nonce_[i] ^ remote_nonce_[i];

  if (version_ == Version::V34) {
    // session key = AES-ECB(real_key, nonce_xor), no padding (single block).
    Bytes in(xored, xored + 16);
    Bytes out(16);
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, real_key_, 128);
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in.data(), out.data());
    mbedtls_aes_free(&ctx);
    std::memcpy(key_, out.data(), 16);
  } else if (version_ == Version::V35) {
    // session key = AES-GCM(real_key, iv=local_nonce[:12], nonce_xor)[ciphertext].
    uint8_t iv[12];
    std::memcpy(iv, local_nonce_, 12);
    Bytes pt(xored, xored + 16);
    Bytes ct;
    uint8_t tag[16];
    if (!aes_gcm_encrypt(real_key_, iv, nullptr, 0, pt, ct, tag) || ct.size() < 16)
      return false;
    std::memcpy(key_, ct.data(), 16);  // first 16 bytes of ciphertext
  } else {
    return false;  // 3.3 has no session key
  }
  return true;
}

}  // namespace proto
}  // namespace tuya_lan
}  // namespace esphome
