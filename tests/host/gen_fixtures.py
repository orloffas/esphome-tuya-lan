#!/usr/bin/env python3
"""Generate byte-exact Tuya LAN protocol fixtures from tinytuya (MIT).

Run with:  uv run --with tinytuya python tests/host/gen_fixtures.py

Every fixture pins the key, nonces, GCM IV, sequence numbers and timestamp so
the frames are deterministic. The C++ host test (test_protocol.cpp) replays the
same inputs through our codec and asserts identical bytes.
"""
import binascii
import hashlib
import hmac as hmaclib
import json
import logging
import os
import struct
import sys
import time

# Deterministic IV for 3.5 GCM: tinytuya uses the fixed b"0123456789ab" nonce
# whenever the crypto logger is at DEBUG level (see crypto_helper.get_encryption_iv).
logging.getLogger("tinytuya.core.crypto_helper").setLevel(logging.DEBUG)

import tinytuya  # noqa: E402
from tinytuya.core import XenonDevice as X  # noqa: E402
from tinytuya.core.crypto_helper import AESCipher  # noqa: E402
from tinytuya.core.message_helper import pack_message, MessagePayload, TuyaMessage  # noqa: E402
from tinytuya.core import command_types as CT  # noqa: E402
from tinytuya.core import header as H  # noqa: E402

KEY = b"0123456789abKKKK"          # 16-byte device local_key
LOCAL_NONCE = b"nonce-abcdef-456"   # 16-byte client nonce (distinct from GCM_IV
                                    # on purpose: the frame IV must not be the nonce)
REMOTE_NONCE = b"fedcba9876543210"  # 16-byte device nonce
GCM_IV = b"0123456789ab"            # 12-byte fixed frame nonce (tinytuya debug mode)
T = 1700000000                      # fixed timestamp
DEV_ID = "bfabcd1234567890abcdef"

time.time = lambda: T  # freeze timestamps used by generate_payload

fixtures = []


def hx(b):
    return binascii.hexlify(b).decode()


def make_dev(version, session_key=None):
    dev = tinytuya.Device(DEV_ID, "10.0.0.1", KEY.decode(), version=version)
    dev.seqno = 1
    if session_key is not None:
        dev.local_key = session_key
    return dev


def derive_session_key(version):
    """Replicate tinytuya's finalize to get the session key for fixed nonces."""
    dev = make_dev(version)
    dev.real_local_key = KEY
    dev.local_key = KEY
    dev.local_nonce = LOCAL_NONCE
    dev.remote_nonce = REMOTE_NONCE
    dev._negotiate_session_key_generate_finalize()
    return dev.local_key


def encode_case(name, version, command, data=None, session_key=None):
    """Full frame for one high-level command via tinytuya._encode_message."""
    dev = make_dev(version, session_key=session_key)
    seqno = dev.seqno
    mp = dev.generate_payload(command, data=data)
    json_payload = mp.payload.decode()      # exact bytes we will hand our codec
    cmd_sent = mp.cmd                        # command_override resolved by tinytuya
    frame = dev._encode_message(mp)
    fixtures.append({
        "name": name,
        "op": "encode",
        "version": {3.3: "33", 3.4: "34", 3.5: "35"}[version],
        "cmd": cmd_sent,
        "seqno": seqno,
        "key": hx(session_key if session_key else KEY),
        "iv": hx(GCM_IV) if version == 3.5 else None,
        "payload": json_payload,
        "payload_hex": hx(mp.payload),
        "expect": hx(frame),
    })


def session_step1_case(version):
    dev = make_dev(version)
    dev.real_local_key = KEY
    dev.local_key = KEY
    dev.seqno = 1
    # force fixed local nonce
    orig = os.urandom
    os.urandom = lambda n: LOCAL_NONCE[:n]
    try:
        mp = dev._negotiate_session_key_generate_step_1()
        frame = dev._encode_message(mp)
    finally:
        os.urandom = orig
    fixtures.append({
        "name": f"v{version}_session_step1",
        "op": "session_step1",
        "version": {3.4: "34", 3.5: "35"}[version],
        "seqno": 1,
        "key": hx(KEY),
        "iv": hx(GCM_IV) if version == 3.5 else None,
        "local_nonce": hx(LOCAL_NONCE),
        "expect": hx(frame),
    })


def session_finish_case(version):
    """SESS_KEY_NEG_FINISH frame + derived session key, given fixed nonces."""
    dev = make_dev(version)
    dev.real_local_key = KEY
    dev.local_key = KEY
    dev.local_nonce = LOCAL_NONCE
    dev.remote_nonce = REMOTE_NONCE
    dev.seqno = 5
    rkey_hmac = hmaclib.new(KEY, REMOTE_NONCE, hashlib.sha256).digest()
    mp = MessagePayload(CT.SESS_KEY_NEG_FINISH, rkey_hmac)
    frame = dev._encode_message(mp)
    # the device RESP payload our codec must verify at step 3
    resp_hmac = hmaclib.new(KEY, LOCAL_NONCE, hashlib.sha256).digest()
    resp_payload = REMOTE_NONCE + resp_hmac
    fixtures.append({
        "name": f"v{version}_session_finish",
        "op": "session_finish",
        "version": {3.4: "34", 3.5: "35"}[version],
        "seqno": 5,
        "key": hx(KEY),
        "iv": hx(GCM_IV) if version == 3.5 else None,
        "local_nonce": hx(LOCAL_NONCE),
        "resp_payload": hx(resp_payload),
        "session_key": hx(derive_session_key(version)),
        "expect": hx(frame),
    })


def decode_case(name, version, command, dps, session_key=None):
    """Build a device-style response frame and record the expected JSON."""
    key = session_key if session_key else KEY
    body = json.dumps({"dps": dps}, separators=(",", ":")).encode()
    cipher = AESCipher(key)

    if version == 3.3:
        inner = cipher.encrypt(body, use_base64=False)
        payload = struct.pack(">I", 0) + inner
        msg = TuyaMessage(3, command, 0, payload, 0, True, H.PREFIX_55AA_VALUE, False)
        frame = pack_message(msg)
    elif version == 3.4:
        inner = cipher.encrypt(body, use_base64=False, pad=True)
        payload = struct.pack(">I", 0) + inner
        msg = TuyaMessage(3, command, 0, payload, 0, True, H.PREFIX_55AA_VALUE, False)
        frame = pack_message(msg, hmac_key=key)
    else:  # 3.5
        raw = struct.pack(">I", 0) + body  # retcode + json, GCM-encrypted
        msg = TuyaMessage(3, command, None, raw, 0, True, H.PREFIX_6699_VALUE, GCM_IV)
        frame = pack_message(msg, hmac_key=key)

    fixtures.append({
        "name": name,
        "op": "decode",
        "version": {3.3: "33", 3.4: "34", 3.5: "35"}[version],
        "key": hx(key),
        "frame": hx(frame),
        "expect_json": body.decode(),
    })


def main():
    # --- 3.3 (no session key) ---
    encode_case("v3.3_control_on", 3.3, CT.CONTROL, data={"1": True})
    encode_case("v3.3_dp_query", 3.3, CT.DP_QUERY)
    decode_case("v3.3_status_resp", 3.3, CT.DP_QUERY, {"1": True, "18": 123})

    # --- 3.4 (session key) ---
    sk34 = derive_session_key(3.4)
    session_step1_case(3.4)
    session_finish_case(3.4)
    encode_case("v3.4_control_on", 3.4, CT.CONTROL, data={"1": True}, session_key=sk34)
    encode_case("v3.4_dp_query", 3.4, CT.DP_QUERY, session_key=sk34)
    decode_case("v3.4_status_resp", 3.4, CT.DP_QUERY_NEW, {"1": True}, session_key=sk34)

    # --- 3.5 (session key, GCM) ---
    sk35 = derive_session_key(3.5)
    session_step1_case(3.5)
    session_finish_case(3.5)
    encode_case("v3.5_control_on", 3.5, CT.CONTROL, data={"1": True}, session_key=sk35)
    encode_case("v3.5_dp_query", 3.5, CT.DP_QUERY, session_key=sk35)
    decode_case("v3.5_status_resp", 3.5, CT.DP_QUERY_NEW, {"1": True, "20": 2301}, session_key=sk35)

    # --- standalone crypto KATs ---
    fixtures.append({"name": "crc32_check", "op": "crc32",
                     "data": hx(b"123456789"), "expect_crc": 0xCBF43926})

    here = os.path.dirname(__file__)
    with open(os.path.join(here, "fixtures.json"), "w") as f:
        json.dump({"tinytuya_version": tinytuya.__version__, "fixtures": fixtures}, f, indent=2)

    # Flat key=value file, records separated by blank lines, for the C++ test.
    with open(os.path.join(here, "fixtures.kv"), "w") as f:
        f.write(f"# generated from tinytuya {tinytuya.__version__}; do not edit by hand\n")
        for fx in fixtures:
            f.write("\n")
            for k, v in fx.items():
                if v is None:
                    continue
                f.write(f"{k}={v}\n")
    print(f"wrote {len(fixtures)} fixtures (tinytuya {tinytuya.__version__})")


if __name__ == "__main__":
    sys.exit(main())
