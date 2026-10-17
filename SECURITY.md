**English** | [Русский](SECURITY.ru.md)

# Security policy

## Reporting a vulnerability

Please do not open a public issue.

Use [private vulnerability reporting](https://github.com/orloffas/esphome-tuya-lan/security/advisories/new) instead. It is the fastest route and keeps the details out of public view until there is a fix.

Expect a first reply within a week. This is a spare-time project with one maintainer, so please read that as an honest estimate rather than an SLA.

## Never commit device secrets

The `local_key`, the `device_id` and the addresses of your plugs are secrets. Anyone who has the `local_key` and can reach the plug on the network can switch it. Never put them in the repository, an issue, a pull request or a log excerpt. Examples and configs use `!secret` only; the `secrets.yaml` file is in `.gitignore`.

If a key ends up in a public place, treat it as compromised: re-pair the plug so it gets a new `local_key`.

## What is in scope

- the protocol code in `components/tuya_lan/tuya_protocol.*`: framing, encryption, HMAC and GCM handling, parsing of frames from the network
- the hub and the platforms: anything that lets a device on the LAN crash the node, make it run something unexpected, or leak a `local_key` into a log
- the example and CI configuration, if they teach or cause unsafe handling of secrets

## What is not

- Vulnerabilities in [ESPHome](https://github.com/esphome/esphome), ESP-IDF or mbedtls themselves — report those to those projects.
- Weaknesses of the Tuya local protocol as designed: it relies on a shared key and offers no more than that. The component implements it as it is.
- Anything about the Tuya cloud, its apps or its terms.

## Supported versions

The latest commit on `main`. There are no maintained release branches.
