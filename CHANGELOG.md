# Changelog

Russian version: [CHANGELOG.ru.md](CHANGELOG.ru.md)

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html). There are no tagged releases yet.

## [Unreleased]

### Added

- **The `tuya_lan` ESPHome component:** a hub that keeps a local connection to each Tuya Wi-Fi plug, with `switch`, `sensor` and `binary_sensor` platforms on top of it. No Tuya cloud and no Home Assistant in the chain.
- **Tuya local protocols 3.3, 3.4 and 3.5.**
- **Host tests of the protocol codec,** checked byte for byte against [tinytuya](https://github.com/jasonacox/tinytuya), including negative cases (wrapped length fields, tampered tag/HMAC/CRC, truncated frames). Run them with `make -C tests/host test`.
- **Safety-minded networking:** all socket I/O in a FreeRTOS task that never blocks `loop()`; switch state published only on device confirmation; writes held as desired state and retried after reconnect so an OFF is not lost; hardware RNG for 3.5 GCM nonces; bounds-checked parsing of untrusted frames.
- **An example for two plugs** in `example/`, with credentials in `!secret`.
- **CI:** the host tests and an `esphome compile` of a test configuration.
