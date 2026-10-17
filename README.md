<!-- markdownlint-disable MD033 MD041 -->
**English** | [Русский](#esphome-tuya-lan-русский)

# esphome-tuya-lan

**An ESPHome external component that controls Tuya Wi-Fi plugs over the local network — no Tuya cloud, no Home Assistant in the control path.**

[![Release](https://img.shields.io/github/v/release/orloffas/esphome-tuya-lan?color=blue)](https://github.com/orloffas/esphome-tuya-lan/releases/latest)
[![CI](https://github.com/orloffas/esphome-tuya-lan/actions/workflows/ci.yml/badge.svg)](https://github.com/orloffas/esphome-tuya-lan/actions/workflows/ci.yml)
[![CodeRabbit Pull Request Reviews](https://img.shields.io/coderabbit/prs/github/orloffas/esphome-tuya-lan?utm_source=oss&utm_medium=github&labelColor=171717&color=FF570A&label=CodeRabbit+Reviews)](https://coderabbit.ai)
![ESPHome 2026.9+](https://img.shields.io/badge/ESPHome-2026.9%2B-000000?logo=esphome&logoColor=white)
![Protocols 3.3 / 3.4 / 3.5](https://img.shields.io/badge/Tuya-3.3%20%2F%203.4%20%2F%203.5-1BA0E2)
![License: MIT](https://img.shields.io/badge/license-MIT-green)

## What it is

A Tuya plug speaks a local TCP protocol on your network. Normally you let the Tuya cloud or a Home Assistant integration talk to it. This component lets an **ESP32 do it directly**: the ESP32 becomes the plug's controller, so the logic that owns the plug — a thermostat, a safety cut-off, an alarm — lives on the microcontroller and keeps working whether or not anything else is online.

The original use is a snail terrarium: an ESP32 reads temperatures and drives two Tuya plugs (heating mats and an IR heater). The plugs must never be left to the cloud, so the board owns them outright and Home Assistant only watches.

## Why not the existing options

- **The Tuya cloud** puts your heating behind someone else's servers and an internet link.
- **The `tuya` ESPHome component** talks to a Tuya MCU over a serial UART inside a flashed device — it does not reach a finished plug over Wi-Fi.
- **Home Assistant + a local Tuya integration** works, but then Home Assistant is in the control path. If it is down, nothing turns the heater off.

A plug you can reach from your own ESP32, with no third party in the loop, closes that gap.

## Features

- **Tuya local protocols 3.3, 3.4 and 3.5** — AES-ECB with CRC (3.3), session-key negotiation with HMAC-SHA256 (3.4), and AES-GCM framing (3.5).
- **`switch` platform** — turn a DP on and off. State is published **only when the plug confirms it**, never optimistically, so what you see is what the hardware is doing.
- **`sensor` platform** — any numeric DP (current, power, voltage on a metering plug). Scale with the standard ESPHome `filters:`.
- **`binary_sensor` platform** — whether the plug is reachable, so you can alarm on losing control of the heater.
- **Non-blocking.** All socket I/O runs in a dedicated FreeRTOS task with connect/receive timeouts, a command queue and automatic reconnect; `loop()` is never blocked.
- **Works with the ESPHome `thermostat`** — the switch is a valid `heat_action`.
- **Byte-exact protocol**, cross-checked against [tinytuya](https://github.com/jasonacox/tinytuya) by a host test on every change.

## Requirements

- An ESP32 (developed on an ESP32-S3-DevKitC-1).
- ESPHome 2026.9 or newer. The ESP-IDF framework is the primary target; the Arduino framework is expected to work too.
- The plug's `device_id` and `local_key` (see below).

## Installation

Add the component straight from GitHub:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/orloffas/esphome-tuya-lan
    components: [tuya_lan]
```

Pin a release for reproducible builds by adding `ref: v0.1.0` under `source`.

## Configuration

Declare one `tuya_lan:` hub per plug, then add platform entities that point at it with `tuya_lan_id`.

```yaml
tuya_lan:
  - id: mats
    host: !secret mats_host
    device_id: !secret mats_id
    local_key: !secret mats_key
    version: "3.5"

switch:
  - platform: tuya_lan
    name: "Mats power"
    tuya_lan_id: mats
    datapoint: 1

sensor:
  - platform: tuya_lan
    name: "Mats power draw"
    tuya_lan_id: mats
    datapoint: 19
    unit_of_measurement: W
    device_class: power
    filters:
      - multiply: 0.1 # raw DP is deciwatts

binary_sensor:
  - platform: tuya_lan
    name: "Mats online"
    tuya_lan_id: mats
```

### Hub options (`tuya_lan:`)

| Option | Required | Default | Description |
|--------|----------|---------|-------------|
| `host` | yes | — | IP address or hostname of the plug |
| `device_id` | yes | — | Tuya device id |
| `local_key` | yes | — | Tuya local key, exactly 16 characters |
| `version` | no | `"3.3"` | Protocol version: `"3.3"`, `"3.4"` or `"3.5"` |
| `port` | no | `6668` | Tuya local TCP port |
| `poll_interval` | no | `15s` | How often the plug is polled for its state |

### Platform options

- **`switch`** / **`sensor`** require `tuya_lan_id` and `datapoint` (the DP number). All standard ESPHome options apply (`filters`, `device_class`, `unit_of_measurement`, …).
- **`binary_sensor`** requires only `tuya_lan_id`; it reports LAN reachability (`device_class: connectivity`).

A full two-plug example is in [`example/snails-plugs.yaml`](example/snails-plugs.yaml).

> **Naming tip for non-Latin configs.** ESPHome derives an entity's object id by
> transliterating its `name` to ASCII. An all-Cyrillic `name` collapses to an
> empty id and collides with others (`Both convert to ASCII ID`). Give such
> entities a `name` with some Latin characters, or an explicit `id:`.

## Finding `device_id` and `local_key`

These come from Tuya, not from the plug itself. The usual route is the [tinytuya wizard](https://github.com/jasonacox/tinytuya#setup-wizard) (a free Tuya IoT developer account, then `python -m tinytuya wizard`). The DP numbers for a metering plug — current, power, voltage — are the ones its device model uses; `18`, `19` and `20` are common, and the [tuya-local](https://github.com/make-all/tuya-local) device files are a good reference.

**Keep these secret.** Anyone with the `local_key` who can reach the plug can switch it. Put them in `secrets.yaml` (git-ignored) and reference them with `!secret`.

## How it works

On connect, 3.4 and 3.5 devices negotiate a per-session key from a client and a device nonce (HMAC-SHA256, then an AES step). After that the component polls the plug on `poll_interval`, sends commands from a queue, and sends a heartbeat so the plug keeps the connection. Each decoded data-point update is handed to the main loop, which publishes it on the matching entity. For 3.5, every GCM nonce comes from the ESP32 hardware RNG.

## Tested devices

The protocol codec is verified against tinytuya in CI, and the component has been
run against these plugs on real hardware: it reads and decodes their status
correctly, with current/power/voltage matching a reference client to the tenth,
and stays stable over long read-only runs. Switching the plugs from the board has
not yet been exercised live (that write path is host-tested but is the next
on-device step), so hold that part as not-yet-field-proven.

| Plug | Protocol | Switch DP | Notes |
|------|----------|-----------|-------|
| Metering smart plug (`smartplugv2_energy`) | 3.5 | 1 | current (DP 18), power (DP 19), voltage (DP 20) |
| Smart plug with child lock (`smartplugv2_childlock`) | 3.3 | 1 | also reports energy (DP 18/19/20) |

## Security notes

The Tuya local protocol trusts a shared key. Protocol 3.3 authenticates frames
with a CRC only, so anyone on-path on your LAN can in principle replay or forge a
status frame and mislead the reported switch state; 3.4 and 3.5 bind each session
to a negotiated key, which blocks cross-session replay, though neither checks a
per-message sequence number within a session. Keep the plugs on a trusted network
and keep the `local_key` secret. This is a property of the protocol, not of this
implementation.

## Limitations

- ESP32 only — it relies on mbedtls and a FreeRTOS task.
- Boolean and integer data points are supported; string, enum and bitmap DPs are ignored for now.
- The Arduino framework is expected to work but has only been built on ESP-IDF so far.

## Credits

The protocol is implemented from scratch against [tinytuya](https://github.com/jasonacox/tinytuya) (MIT) as the reference. Thanks to that project and to the people who documented the Tuya local protocol.

## License

[MIT](LICENSE). An unofficial Russian translation is in [LICENSE.ru.md](LICENSE.ru.md); only the English original is legally binding.

---

<a name="esphome-tuya-lan-русский"></a>

[English](#esphome-tuya-lan) | **Русский**

# esphome-tuya-lan (русский)

**Внешний компонент ESPHome, который управляет Wi-Fi-розетками Tuya по локальной сети — без облака Tuya и без Home Assistant в цепочке управления.**

## Что это

Розетка Tuya говорит по локальному TCP-протоколу в вашей сети. Обычно с ней говорит облако Tuya или интеграция Home Assistant. Этот компонент даёт говорить с ней **напрямую с ESP32**: плата становится хозяином розетки, и логика, которая ей владеет, — термостат, аварийная отсечка, тревога — живёт на микроконтроллере и работает независимо от того, доступно ли что-то ещё.

Исходная задача — террариум улиток: ESP32 измеряет температуры и управляет двумя розетками Tuya (маты подогрева и ИК-обогреватель). Отдавать нагрев облаку нельзя, поэтому плата владеет розетками полностью, а Home Assistant только наблюдает.

## Почему не готовые варианты

- **Облако Tuya** уводит нагрев за чужие серверы и интернет-канал.
- **Компонент `tuya` в ESPHome** говорит с MCU Tuya по UART внутри прошиваемого устройства — до готовой розетки по Wi-Fi он не дотягивается.
- **Home Assistant с локальной интеграцией Tuya** работает, но тогда HA стоит в цепочке управления. Если он лёг, нагрев никто не выключит.

Розетка, до которой ESP32 дотягивается сам, без посредников, закрывает этот разрыв.

## Возможности

- **Локальные протоколы Tuya 3.3, 3.4 и 3.5** — AES-ECB с CRC (3.3), согласование сессионного ключа с HMAC-SHA256 (3.4) и кадры AES-GCM (3.5).
- **Платформа `switch`** — включает и выключает DP. Состояние публикуется **только после подтверждения розеткой**, не оптимистично: на экране то, что делает железо.
- **Платформа `sensor`** — любой числовой DP (ток, мощность, напряжение у розетки с ваттметром). Масштаб задаётся штатными `filters:` ESPHome.
- **Платформа `binary_sensor`** — на связи ли розетка, чтобы поднимать тревогу при потере управления нагревом.
- **Не блокирует.** Весь обмен по сокету идёт в отдельной задаче FreeRTOS с таймаутами на подключение и приём, очередью команд и автопереподключением; `loop()` не блокируется.
- **Работает с `thermostat` ESPHome** — выключатель годится как `heat_action`.
- **Протокол точен до байта** — на каждое изменение сверяется host-тестом с [tinytuya](https://github.com/jasonacox/tinytuya).

## Требования

- ESP32 (разработка на ESP32-S3-DevKitC-1).
- ESPHome 2026.9 или новее. Основная цель — фреймворк ESP-IDF; Arduino тоже должен работать.
- `device_id` и `local_key` розетки (см. ниже).

## Установка

Подключите компонент прямо с GitHub:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/orloffas/esphome-tuya-lan
    components: [tuya_lan]
```

Для воспроизводимых сборок зафиксируйте релиз, добавив `ref: v0.1.0` под `source`.

## Настройка

Опишите по одному хабу `tuya_lan:` на розетку, затем добавьте сущности платформ, указывающие на него через `tuya_lan_id`.

```yaml
tuya_lan:
  - id: mats
    host: !secret mats_host
    device_id: !secret mats_id
    local_key: !secret mats_key
    version: "3.5"

switch:
  - platform: tuya_lan
    id: mats_power
    name: "Маты, питание"
    tuya_lan_id: mats
    datapoint: 1

sensor:
  - platform: tuya_lan
    id: mats_power_draw
    name: "Маты, мощность"
    tuya_lan_id: mats
    datapoint: 19
    unit_of_measurement: W
    device_class: power
    filters:
      - multiply: 0.1 # сырой DP — в дециваттах

binary_sensor:
  - platform: tuya_lan
    id: mats_online
    name: "Маты на связи"
    tuya_lan_id: mats
```

### Параметры хаба (`tuya_lan:`)

| Параметр | Обязателен | По умолчанию | Описание |
|----------|------------|--------------|----------|
| `host` | да | — | IP-адрес или имя хоста розетки |
| `device_id` | да | — | идентификатор устройства Tuya |
| `local_key` | да | — | локальный ключ Tuya, ровно 16 символов |
| `version` | нет | `"3.3"` | версия протокола: `"3.3"`, `"3.4"` или `"3.5"` |
| `port` | нет | `6668` | локальный TCP-порт Tuya |
| `poll_interval` | нет | `15s` | как часто опрашивать состояние розетки |

### Параметры платформ

- **`switch`** и **`sensor`** требуют `tuya_lan_id` и `datapoint` (номер DP). Работают все штатные параметры ESPHome (`filters`, `device_class`, `unit_of_measurement`, …).
- **`binary_sensor`** требует только `tuya_lan_id`; показывает доступность по сети (`device_class: connectivity`).

Полный пример на две розетки — в [`example/snails-plugs.yaml`](example/snails-plugs.yaml).

> **Про имена в нелатинских конфигах.** ESPHome строит object id сущности,
> транслитерируя `name` в ASCII. Имя целиком из кириллицы схлопывается в пустой
> id и конфликтует с другими (`Both convert to ASCII ID`). Давайте таким
> сущностям `name` хотя бы с латиницей или явный `id:`.

## Где взять `device_id` и `local_key`

Их выдаёт Tuya, а не сама розетка. Обычный путь — [мастер tinytuya](https://github.com/jasonacox/tinytuya#setup-wizard) (бесплатный аккаунт разработчика Tuya IoT, затем `python -m tinytuya wizard`). Номера DP для розетки с ваттметром — ток, мощность, напряжение — те, что использует её модель; часто это `18`, `19` и `20`, а хороший справочник — файлы устройств [tuya-local](https://github.com/make-all/tuya-local).

**Держите эти значения в секрете.** Кто знает `local_key` и достаёт розетку по сети — тот ею и управляет. Храните их в `secrets.yaml` (он в `.gitignore`) и подставляйте через `!secret`.

## Как это работает

При подключении устройства 3.4 и 3.5 согласуют сессионный ключ из клиентского и серверного nonce (HMAC-SHA256, затем шаг AES). Дальше компонент опрашивает розетку по `poll_interval`, шлёт команды из очереди и heartbeat, чтобы розетка держала соединение. Каждое разобранное изменение DP передаётся в главный цикл, который публикует его на нужной сущности. Для 3.5 все GCM-nonce берутся из аппаратного генератора случайных чисел ESP32.

## Проверенные устройства

Кодек протокола сверяется с tinytuya в CI, и компонент прогнан с этими розетками
на реальном железе: он корректно читает и декодирует их статус — ток, мощность и
напряжение совпадают с эталонным клиентом до десятой, — и держится стабильно на
долгих прогонах в режиме только чтения. Переключение розеток самой платой вживую
ещё не гоняли (этот путь записи покрыт host-тестом, но это следующий шаг на
железе), так что эту часть считайте пока не подтверждённой в поле.

| Розетка | Протокол | DP выключателя | Примечания |
|---------|----------|----------------|------------|
| Розетка с ваттметром (`smartplugv2_energy`) | 3.5 | 1 | ток (DP 18), мощность (DP 19), напряжение (DP 20) |
| Розетка с защитой от детей (`smartplugv2_childlock`) | 3.3 | 1 | тоже отдаёт ваттметр (DP 18/19/20) |

## О безопасности

Локальный протокол Tuya построен на общем ключе. Протокол 3.3 подтверждает кадры
только CRC, поэтому тот, кто сидит на пути в вашей локальной сети, в принципе может
переиграть или подделать кадр статуса и исказить показанное состояние выключателя;
3.4 и 3.5 привязывают каждую сессию к согласованному ключу, что перекрывает реплей
между сессиями, но порядковый номер внутри сессии ни один из них не проверяет.
Держите розетки в доверенной сети, а `local_key` — в секрете. Это свойство самого
протокола, а не этой реализации.

## Ограничения

- Только ESP32 — нужны mbedtls и задача FreeRTOS.
- Поддержаны булевы и целочисленные DP; строковые, enum и битовые DP пока игнорируются.
- Фреймворк Arduino должен работать, но собиралось пока только на ESP-IDF.

## Благодарности

Протокол написан с нуля, эталоном служит [tinytuya](https://github.com/jasonacox/tinytuya) (MIT). Спасибо этому проекту и всем, кто описал локальный протокол Tuya.

## Лицензия

[MIT](LICENSE). Неофициальный перевод на русский — в [LICENSE.ru.md](LICENSE.ru.md); юридическую силу имеет только английский оригинал.
