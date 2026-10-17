**English** | [Русский](CONTRIBUTING.ru.md)

# Contributing

Thanks for looking. This is a small project with one maintainer, so the short version is: open an issue before writing anything large, and expect replies to take days rather than hours.

## Before you start

- **Bugs and questions** go through [issues](https://github.com/orloffas/esphome-tuya-lan/issues/new). Say which plug model you have, the protocol version (3.3, 3.4 or 3.5), the ESPHome version and the log with `logger: level: DEBUG`. Remove the `local_key`, the `device_id` and addresses from anything you paste.
- **Security problems** go through [private reporting](https://github.com/orloffas/esphome-tuya-lan/security/advisories/new). See [SECURITY.md](SECURITY.md).
- **Large changes** — ask first. A new protocol version or a change to the connection model is a design question, not a patch.

## What will not be accepted

- Anything that depends on the Tuya cloud, or on Home Assistant being present. Local control without either is the point of the project.
- Real `local_key`, `device_id` or addresses in code, examples, tests or fixtures. Examples use `!secret`; test data is made up.
- Telemetry of any kind.

## Setting up

```bash
git clone https://github.com/orloffas/esphome-tuya-lan.git
cd esphome-tuya-lan
```

You need a C++17 compiler, mbedtls (`brew install mbedtls@3` on macOS, `apt install libmbedtls-dev` on Debian and Ubuntu) and [uv](https://docs.astral.sh/uv/) or Python with `tinytuya`.

## Testing

```bash
make -C tests/host test     # protocol codec against tinytuya
esphome compile .github/test/compile-test.yaml
```

- The host test is mandatory for any change to `components/tuya_lan/tuya_protocol.*`. It generates reference frames with `tinytuya` and compares them byte for byte, so a green run means the codec talks to real plugs the way `tinytuya` does.
- The compile test builds every platform with both frame formats (3.3 and 3.5). It is slow on a first run because it downloads the ESP-IDF toolchain.
- CI runs both. Neither replaces trying the change on a real plug, so say in the PR if you did.

## Code

- The protocol core (`tuya_protocol.*`) has no ESPHome dependency. Keep it that way, so the host test can build it alone.
- C++17 for the component, Python for the ESPHome schema side (`__init__.py` in each platform).
- Comments explain why, not what. If a line looks strange and is deliberate, say why it is deliberate.
- Commit messages and PR descriptions in English.

## AI assistance

This project is written with heavy use of AI coding assistants, which is why
the commit history carries `Co-Authored-By` trailers. You are welcome to use
them too.

One expectation either way: you are responsible for the code you send. Be able
to explain what it does and why it is written that way, and run the tests
before opening the PR. A patch nobody understands is a liability regardless of
who or what produced it.

## Pull requests

Work goes into `main` through pull requests, including the maintainer's own. Merges are squash merges, and CI must be green before merging.

Keep a PR to one thing. Two unrelated fixes are two PRs — they get reviewed faster and revert cleanly.

If your change alters behaviour or configuration options, update **both halves** of `README.md` — the English one and the Russian section below it. They are expected to say the same things; changing one without the other is an unfinished change. Add a line to `CHANGELOG.md` and `CHANGELOG.ru.md` under `[Unreleased]`.

Final call on scope and design is the maintainer's. If a PR is declined, it is usually about fit rather than quality.
