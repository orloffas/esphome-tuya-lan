// Host test for the Tuya LAN codec. Replays tinytuya-generated fixtures
// (fixtures.kv) through our codec and asserts byte-identical frames.
//
// Build/run via the Makefile in this directory (needs mbedtls from Homebrew).
#include "../../components/tuya_lan/tuya_protocol.h"
#include "../../components/tuya_lan/dp_state.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using esphome::tuya_lan::proto::Bytes;
using esphome::tuya_lan::proto::Codec;
using esphome::tuya_lan::proto::extract_frame;
using esphome::tuya_lan::proto::Version;

static int g_pass = 0, g_fail = 0;

static Bytes unhex(const std::string &h) {
  Bytes b;
  for (size_t i = 0; i + 1 < h.size(); i += 2)
    b.push_back(static_cast<uint8_t>(std::stoul(h.substr(i, 2), nullptr, 16)));
  return b;
}

static std::string hex(const Bytes &b) {
  static const char *d = "0123456789abcdef";
  std::string s;
  for (uint8_t c : b) { s += d[c >> 4]; s += d[c & 0xf]; }
  return s;
}

static Version ver(const std::string &v) {
  if (v == "33") return Version::V33;
  if (v == "34") return Version::V34;
  return Version::V35;
}

static void check(const std::string &name, const std::string &got, const std::string &want) {
  if (got == want) {
    g_pass++;
    std::printf("  ok   %s\n", name.c_str());
  } else {
    g_fail++;
    std::printf("  FAIL %s\n       got  %s\n       want %s\n", name.c_str(), got.c_str(), want.c_str());
  }
}

// Parse fixtures.kv into records (maps of key=value, blank-line separated).
static std::vector<std::map<std::string, std::string>> load(const std::string &path) {
  std::vector<std::map<std::string, std::string>> recs;
  std::ifstream f(path);
  std::string line;
  std::map<std::string, std::string> cur;
  while (std::getline(f, line)) {
    if (!line.empty() && line[0] == '#') continue;
    if (line.empty()) {
      if (!cur.empty()) { recs.push_back(cur); cur.clear(); }
      continue;
    }
    auto eq = line.find('=');
    if (eq != std::string::npos) cur[line.substr(0, eq)] = line.substr(eq + 1);
  }
  if (!cur.empty()) recs.push_back(cur);
  return recs;
}

int main(int argc, char **argv) {
  std::string path = argc > 1 ? argv[1] : "fixtures.kv";
  auto recs = load(path);
  if (recs.empty()) { std::printf("no fixtures loaded from %s\n", path.c_str()); return 2; }

  // --- frame extractor: malformed lengths must resync, never hang ---
  {
    // 55AA header claiming length 0xFFFFFFF0 (would wrap 16+len to 0).
    Bytes buf = unhex("000055aa000000010000000afffffff0");
    Bytes out;
    bool got = extract_frame(buf, out);  // must consume garbage and return false
    check("extract_55aa_wrap_no_hang", (!got && buf.size() < 4) ? "ok" : "HANG", "ok");

    // 6699 header (prefix|00|seqno|cmd|len) claiming length 0xFFFFFFFC.
    buf = unhex("000066990000000000010000000dfffffffc");
    got = extract_frame(buf, out);
    check("extract_6699_wrap_no_hang", (!got && buf.size() < 4) ? "ok" : "HANG", "ok");

    // Garbage then a valid frame: the extractor must resync and find the frame.
    for (auto &r : recs) {
      if (r["op"] != "decode") continue;
      Bytes good = unhex(r["frame"]);
      Bytes mixed = unhex("deadbeef");
      mixed.insert(mixed.end(), good.begin(), good.end());
      Bytes out2;
      bool ok = extract_frame(mixed, out2);
      check("extract_resync_after_garbage", (ok && out2 == good) ? "ok" : "LOST", "ok");
      break;
    }
  }

  // --- hub DP state machine (pure logic extracted from tuya_lan.cpp) ---
  {
    using esphome::tuya_lan::BoolDpMap;
    using esphome::tuya_lan::dps_needing_assert;
    using esphome::tuya_lan::valid_dp_id;
    auto join = [](const std::vector<uint8_t> &v) {
      std::string s;
      for (size_t i = 0; i < v.size(); i++) { if (i) s += ","; s += std::to_string(v[i]); }
      return s;
    };
    // A fresh write (plug not yet confirmed) must be sent.
    check("assert_fresh", join(dps_needing_assert(BoolDpMap{{1, true}}, BoolDpMap{})), "1");
    // Already confirmed at the desired value: nothing to send.
    check("assert_confirmed", join(dps_needing_assert(BoolDpMap{{1, true}}, BoolDpMap{{1, true}})), "");
    // Desired OFF but plug reports ON: must resend.
    check("assert_off_differs", join(dps_needing_assert(BoolDpMap{{1, false}}, BoolDpMap{{1, true}})), "1");
    // After a reconnect confirmed_ is empty: a commanded OFF is re-asserted, not lost.
    check("assert_after_reconnect", join(dps_needing_assert(BoolDpMap{{1, false}}, BoolDpMap{})), "1");
    // Only the DP that differs is resent.
    check("assert_multi", join(dps_needing_assert(BoolDpMap{{1, true}, {7, false}}, BoolDpMap{{1, true}})), "7");
    // After release (desired cleared) the plug's reported ON is NOT re-asserted:
    // the board stops influencing a plug handed back to another controller.
    check("released_not_reasserted", join(dps_needing_assert(BoolDpMap{}, BoolDpMap{{1, true}})), "");
    // DP id range.
    check("dpid_0", valid_dp_id(0) ? "T" : "F", "F");
    check("dpid_1", valid_dp_id(1) ? "T" : "F", "T");
    check("dpid_255", valid_dp_id(255) ? "T" : "F", "T");
    check("dpid_256", valid_dp_id(256) ? "T" : "F", "F");
    check("dpid_neg", valid_dp_id(-1) ? "T" : "F", "F");
  }

  for (auto &r : recs) {
    const std::string &op = r["op"];
    const std::string &name = r["name"];

    if (op == "crc32") {
      Bytes d = unhex(r["data"]);
      uint32_t got = esphome::tuya_lan::proto::crc32(d.data(), d.size());
      uint32_t want = std::stoul(r["expect_crc"], nullptr, 0);
      check(name, std::to_string(got), std::to_string(want));
      continue;
    }

    Bytes key = unhex(r["key"]);
    Codec codec(ver(r["version"]), key.data());

    if (op == "encode") {
      Bytes payload = unhex(r["payload_hex"]);
      std::string json(payload.begin(), payload.end());
      uint32_t seqno = std::stoul(r["seqno"]);
      Bytes iv;
      const uint8_t *ivp = nullptr;
      if (r.count("iv")) { iv = unhex(r["iv"]); ivp = iv.data(); }
      Bytes frame = codec.encode(std::stoul(r["cmd"]), json, seqno, ivp);
      check(name, hex(frame), r["expect"]);

    } else if (op == "session_step1") {
      Bytes nonce = unhex(r["local_nonce"]);
      Bytes iv;
      const uint8_t *ivp = nullptr;
      if (r.count("iv")) { iv = unhex(r["iv"]); ivp = iv.data(); }
      Bytes frame = codec.session_step1(std::stoul(r["seqno"]), nonce.data(), ivp);
      check(name, hex(frame), r["expect"]);

    } else if (op == "session_finish") {
      Bytes nonce = unhex(r["local_nonce"]);
      Bytes iv;
      const uint8_t *ivp = nullptr;
      if (r.count("iv")) { iv = unhex(r["iv"]); ivp = iv.data(); }
      codec.session_step1(1, nonce.data(), ivp);  // establish local nonce
      Bytes resp = unhex(r["resp_payload"]);
      std::string resp_s(resp.begin(), resp.end());
      Bytes frame = codec.session_step3(resp_s, std::stoul(r["seqno"]), ivp);
      check(name, hex(frame), r["expect"]);
      // session key derivation
      bool ok = codec.finalize_session();
      Bytes sk(codec.active_key(), codec.active_key() + 16);
      check(name + "_key", ok ? hex(sk) : "DERIVE_FAILED", r["session_key"]);

    } else if (op == "decode") {
      Bytes frame = unhex(r["frame"]);
      auto d = codec.decode(frame);
      check(name, d.ok ? d.payload : "DECODE_FAILED", r["expect_json"]);

      // Negative: a single flipped byte must fail CRC (3.3), HMAC (3.4) or the
      // GCM tag (3.5). Catches a regression that drops the check.
      if (frame.size() > 8) {
        Bytes bad = frame;
        bad[frame.size() / 2] ^= 0x01;
        Codec c2(ver(r["version"]), key.data());
        check(name + "_tampered", c2.decode(bad).ok ? "ACCEPTED_BAD" : "rejected", "rejected");
      }
      // Negative: a truncated frame must not decode.
      if (frame.size() > 4) {
        Bytes trunc(frame.begin(), frame.end() - 3);
        Codec c3(ver(r["version"]), key.data());
        check(name + "_truncated", c3.decode(trunc).ok ? "ACCEPTED_SHORT" : "rejected", "rejected");
      }
    }
  }

  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
