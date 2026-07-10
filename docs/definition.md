# X-28 Alarm ESPHome Component — System Definition

## 1. Overview

This document defines an ESPHome external component that interfaces with X-28 home
alarm systems over the MPX/MPXH bus.

### 1.1 Reference Repositories

| Repository | Description |
|------------|-------------|
| [`x28-mpx-controller`](https://github.com/fedapon/x28-mpx-controller) | Arduino library with full TX/RX, circular buffer, event callbacks, zone monitoring |
| [`esphome-x28`](https://github.com/hjf/esphome-x28) | ESPHome custom component using deprecated `custom_component` API |
| [`x28_sniffer`](https://github.com/gbisheimer/x28_sniffer) | Protocol sniffer using RCSwitch library configured for MPX timing |

### 1.2 Design Goals

1. **Proper ESPHome integration** — Use modern external component API with `CONFIG_SCHEMA`,
   code generation, and typed entities. No deprecated `custom_component`.
2. **Full bidirectional protocol support** — Receive alarm state events AND transmit key
   sequences to arm/disarm/program the panel.
3. **Alarm Control Panel entity** — Standard Home Assistant alarm_control_panel with
   states: DISARMED, ARMED_HOME, ARMED_AWAY, PENDING, TRIGGERED.
4. **Zone monitoring** — Binary sensors for zones 1–32 (model-dependent), both MPXH and wired types.
5. **Sniffing mode** — All-bus packet logging for discovery and debugging.
6. **Virtual zone injection** — Bridge HA binary sensors into the MPX bus as native
   alarm zones, enabling the X-28 panel to respond to HA sensors.
7. **Full keyboard emulation** — Send any key sequence, enabling complete panel
   programming from Home Assistant.
8. **No external Arduino libraries** — Protocol implemented from scratch for full
   control and platform compatibility.

### 1.3 Glossary

| Term | Definition |
|------|------------|
| MPX | Original 12 V bus protocol used by X-28 alarm panels |
| MPXH | Enhanced 8 V variant, electrically compatible with MPX devices |
| Central | The main X-28 alarm panel (e.g., N8F-MPXH) |
| Zone | A monitored input (sensor) connected to the bus |
| Estoy | "I'm staying" — Stay mode (interior zones excluded) |
| Me Voy | "I'm leaving" — Away mode (all zones active) |
| MODO | Panel key that toggles between Estoy and Me Voy |
| MODO P | Programming key used to enter programming modes |
| P-code | Advanced programming function (e.g., P 881 for entry time) |
| Circular buffer | Fixed-size FIFO used to store received packets between ISR and loop() |
| CTS | Clear To Send — minimum bus idle time before transmitting |
| ACP | Alarm Control Panel (HA entity) |
| BLE | Both Leading Edges — interrupt triggering on both rising and falling |

## 2. Hardware

### 2.1 Bus Characteristics

| Parameter   | MPX          | MPXH         |
|-------------|--------------|--------------|
| Voltage     | 12 V         | ~8 V         |
| Wiring      | +12V, GND, MPX | +12V, GND, MPXH |
| Topology    | Bus (daisy-chain or star) | same |
| Max devices per bus | ~32 (with level 1 repeaters) | same |
| Data rate   | ~262 bps     | same         |
| Bit time    | 3.8125 ms    | same         |

The central N8F-MPXH provides two independent MPXH buses: **12V AUX1 MPXH** and
**12V AUX2 MPXH**, each on a 3-pin terminal block (+12V, GND, MPXH). Both are
fused at 3 A.

### 2.2 Required Interface Circuit

A level shifter + line driver is required between the ESP GPIO (3.3 V) and the
MPX bus (12 V / 8 V).

**Reference circuit** (from `circuito.png` in `x28-mpx-controller`):

```
                +12V (from alarm bus)
                 │
                 ├─ R1 (10kΩ) ──┬─ RX pin (ESP)
                 │              │
MPX bus ──┬──────┤              ├─ GND (ESP + alarm common)
          │      │              │
          │      └─ D1 (zener 3.3V) ─ GND
          │
          └──────┬─ C1 (100nF)
                 │
                 ├─ R2 (1kΩ) ──┬─ Collector Q1 (2N2222)
                               │
                              TX pin (ESP) ── R3 (1kΩ) ── Base Q1
                                              │
                                             Emitter Q1 ── GND
```

**Components:**
| Component | Value | Purpose |
|-----------|-------|---------|
| Q1 | 2N2222 or similar NPN | Line driver for TX |
| D1 | 3.3 V zener | RX voltage clamp |
| R1 | 10 kΩ | RX pull-up |
| R2 | 1 kΩ | Collector load |
| R3 | 1 kΩ | Base current limit |
| C1 | 100 nF | RX noise filter (optional) |

**Alternative:** An optocoupler-based circuit (e.g., PC817) can be used for
galvanic isolation. The same `invert_tx` / `invert_rx` config flags accommodate
whichever circuit topology is chosen.

### 2.3 ESP Pin Requirements

| Pin | Direction | Function | Requirements |
|-----|-----------|----------|-------------|
| RX  | Input     | Bus data receive | Interrupt-capable, CHANGE trigger |
| TX  | Output    | Bus data transmit | Standard GPIO, push-pull or open-drain |

**ESP32:** Any GPIO works (all support external interrupts).
**ESP8266:** Only GPIO 0, 2, 4, 5, 12, 13, 14, 15 support interrupts
(`digitalPinToInterrupt`).

### 2.4 Pin Inversion

Most interface circuits invert the bus signal. The component handles this via
the `invert_rx` and `invert_tx` configuration flags:

```yaml
x28_alarm:
  rx_pin: 22
  tx_pin: 23
  invert_rx: true   # default: true
  invert_tx: true   # default: true
```

When `invert_rx` is true:
- The idle (bus HIGH) state is read as LOW on the ESP RX pin.
- The interrupt handler swaps HIGH/LOW logic accordingly.

When `invert_tx` is true:
- The TX pin drives inverted levels (LOW = bus HIGH, HIGH = bus LOW).
- The transmit timing logic flips the output bits.

## 3. Model Compatibility

### 3.1 Supported Models

The MPX/MPXH protocol is shared across the entire X-28 residential lineup.
All models use the same bus timing, packet format, key codes, event codes,
and programming sequences (P-codes). The differences are in zone capacity,
expansion capability, and physical I/O.

**N-Series (MPXH) — primary target:**

| Model | Wired Zones | MPXH Zones | Max Zones | Users | Events | Partitions | Notes |
|-------|------------|------------|-----------|-------|--------|------------|-------|
| N4-MPXH | 4 + 2* | 4 | 32 | 30 → 240 | 512 → 4096 | 8 plug-in | Entry-level |
| N8-MPXH | 8† | 8 | 64 | 30 → 240 | 512 → 4096 | 8 plug-in | Wired + MPXH hybrid |
| N8F-MPXH | 8† | 8 | 64 | 30 → 240 | 512 → 4096 | 8 plug-in | MPXH-only (no wired sensors) |
| N16-MPXH | 8† | 16 | 128 | 30 → 240 | 512 → 4096 | 8 plug-in | — |
| N32-MPXH | 8† | 32 | 256 | 30 → 240 | 512 → 4096 | 8 plug-in | Supports P 888 (hermanar) |

*\* N4: 4 wired + 1 panic (Z7) + 1 tamper (Z8) = "4+2" format.*
*† N8/N16/N32: 6 wired + 1 panic (Z7) + 1 tamper (Z8) as wired zones.*
*All N-series share the same default codes: owner `282828`, installer `467825`.*

**900X Series (MPX, older):**

| Model | Zones | Interface | Notes |
|-------|-------|-----------|-------|
| 9002-MPX | 2 | Built-in keypad | Basic, no expansion |
| 9003-MPX | 3 | Built-in keypad | — |
| 9004-MPX | 4 | Built-in keypad | — |
| 9004PW-MPX | 4 | Built-in keypad | Variant |
| 9004P-MPX | 4 | Built-in keypad | Variant |

*900X series uses the same MPX protocol timing and packet format. Some
P-codes may differ or be unavailable. The component auto-limits zone count
and disables partition features when model is set to a 900X variant.*

**Other models:**

| Model | Type | Notes |
|-------|------|-------|
| 6002W | Wireless | Different protocol — not supported |
| PH 120 | Kit | Based on N4-MPXH internally |

### 3.2 `model` Configuration Parameter

The `model` parameter tells the component which X-28 hardware it is
connected to, enabling model-specific capability checks.

```yaml
x28_alarm:
  model: N8F-MPXH   # optional, defaults to auto-detect
```

**Valid values:**

| Value | Family | Max MPXH zones | Max wired zones | Wireless | RF learning |
|-------|--------|---------------|----------------|----------|-------------|
| `AUTO` (default) | — | 32 | 8 | Yes | Yes |
| `N4-MPXH` | N | 4 | 4 | No | No |
| `N8-MPXH` | N | 8 | 8 | Yes | No |
| `N8F-MPXH` | N | 8 | 0 | No | No |
| `N16-MPXH` | N | 16 | 8 | Yes | Yes |
| `N32-MPXH` | N | 32 | 8 | Yes | Yes |
| `N32F-MPXH` | N | 32 | 0 | No | Yes |
| `9002-MPX` | 900X | 2 | 2 | No | No |
| `9003-MPX` | 900X | 3 | 3 | No | No |
| `9004-MPX` | 900X | 4 | 4 | No | No |

**When `AUTO` is selected:**
- The component uses conservative defaults: 32 MPXH zones, 8 wired zones,
  wireless and RF learning enabled. This ensures all zone packets are matched
  regardless of the actual panel model.
- Users should set the explicit model when possible for optimal behavior.

**Capability implications:**
- **Zone count:** Services validate `zone` against `model_capabilities_.max_mpxh_zones`.
- **Wired zones:** Models with `max_wired_zones == 0` (N8F-MPXH, N32F-MPXH)
  have no wired zone inputs; the `set_wired_zones` service is not registered.
- **RF learning:** Only registered for models with `has_rf_learning == true`
  (N16, N32, N32F).
- **P 888 (hermanar particiones):** Available on N16-MPXH and N32-MPXH via
  the `set_partition_merge` service.

### 3.3 Factory Default Zone Configurations

Each model has different factory pre-programmed zone configurations for
Estoy and Me Voy modes. The component can use these to set up default
binary sensor labels and zone types.

| Model | Estoy default (included zones) | Me Voy default |
|-------|-------------------------------|----------------|
| 9002-MPX | Z1 instant, Z2 excluded | Z1 instant, Z2 excluded |
| 9003-MPX | Z1, Z3 instant, Z2 excluded | Z1, Z3 instant, Z2 excluded |
| 9004-MPX | Z1, Z4 instant, Z2, Z3 excluded | Z1, Z4 instant, Z2, Z3 excluded |
| N4-MPXH | Z1 instant; Z2–Z4 excluded | Z1 timed; Z2–Z4 instant |
| N8-MPXH | Z1, Z5, Z6, Z7, Z8 instant; Z2, Z3, Z4 excluded | Z1 timed; Z2–Z8 instant |
| N8F-MPXH | Z1 instant; Z2–Z8 excluded | Z1 timed; Z2–Z8 instant |
| N16-MPXH | Z1 instant; Z2–Z16 excluded | Z1 timed; Z2–Z16 instant |
| N32-MPXH | Z1 instant; Z2–Z32 excluded | Z1 timed; Z2–Z32 instant |

### 3.4 Common Programming Interface

All models share these common elements:

| Element | All models | Notes |
|---------|-----------|-------|
| Default owner code | `282828` | 6 digits |
| Default installer code | `467825` | 6 digits |
| Basic programming entry | `<code>PP` | Enter within 30 s of disarm |
| Advanced programming entry | `<code>PPp` | Hold P for 2 s |
| P 773–P 778 | ✓ | Common P-codes |
| P 880–P 886, P 889 | ✓ | Common P-codes |
| P 990–P 994, P 997–P 998 | ✓ | Common zone config P-codes |
| P 770–P 772 (PGM) | ✓ | All models with PGM outputs |
| P 885 (use wired zones) | N-series only | Not on 900X |
| P 888 (hermanar) | N32 only | Merge partitions |
| User codes F2633 | ✓ | 30 users (240 with expansion) |
| RF learning F2337 | ✓ | With plug-in receiver |
| Panic, Fire, Emergency | ✓ | Via long-press on F or numeric keys (900X) |

### 3.5 Model Detection in C++

```cpp
enum class X28Model : uint8_t {
    AUTO = 0,
    N4_MPXH,
    N8_MPXH,
    N8F_MPXH,
    N16_MPXH,
    N32_MPXH,
    _9002_MPX,
    _9003_MPX,
    _9004_MPX,
};

struct ModelCapabilities {
  uint8_t max_mpxh_zones;
  uint8_t max_wired_zones;
  bool has_wireless;
  bool has_rf_learning;
};

constexpr ModelCapabilities get_model_capabilities(X28Model model) {
  switch (model) {
    case X28Model::N4_MPXH:   return { 4,  4,  false, false };
    case X28Model::N8_MPXH:   return { 8,  8,  true,  false };
    case X28Model::N8F_MPXH:  return { 8,  0,  false, false };
    case X28Model::N16_MPXH:  return { 16, 8,  true,  true  };
    case X28Model::N32_MPXH:  return { 32, 8,  true,  true  };
    case X28Model::N32F_MPXH: return { 32, 0,  false, true  };
    case X28Model::_9002_MPX: return { 2,  2,  false, false };
    case X28Model::_9003_MPX: return { 3,  3,  false, false };
    case X28Model::_9004_MPX: return { 4,  4,  false, false };
    case X28Model::AUTO:
    default:                  return { 32, 8,  true,  true  };
  }
}
```

## 4. MPX Protocol Specification

### 4.1 Timing Constants

```
BIT_TIME  = 1270 µs    — basic pulse width (single unit)
ZERO_TIME = 2000 µs    — threshold to distinguish 0 vs 1 bit (must be < 2×BIT_TIME)
IDLE_TIME = 5000 µs    — bus idle detection threshold
CTS_TIME  = 25000 µs   — clear-to-send: minimum bus idle before TX start
```

All times are measured in microseconds using `micros()` on the ESP.

### 4.2 Packet Structure

Each packet consists of a 1-bit start condition followed by 16 data bits.
Total transmission time: ~61 ms.

```
Timing diagram:

    HIGH ──┐      ┌──┐    ┌──┐         ┌──┐    ┐
           │      │  │    │  │         │  │    │
    LOW ───┘      └──┘    └──┘    ...  └──┘    ┘
         START    bit 15   bit 14        bit 0

START:  1 × BIT_TIME LOW
bit 15: 3.8125 ms (3 × BIT_TIME)
bit 14: 3.8125 ms
  ...
bit 0:  3.8125 ms
```

### 4.3 Bit Encoding

Each bit is encoded as a 3-segment waveform. The MPX bus uses negative logic
(active-low signalling). The bit value is determined by the timing of the HIGH
segment:

| Bit Value | Waveform | HIGH duration | LOW duration  | Total  |
|-----------|----------|---------------|---------------|--------|
| `0`       | `100`    | 1 × BIT_TIME  | 2 × BIT_TIME  | 3.81 ms |
| `1`       | `110`    | 2 × BIT_TIME  | 1 × BIT_TIME  | 3.81 ms |

The ISR processes on the **active bus level** (precomputed from `invert_rx`
during setup). It measures the duration of the idle pulse by timing between
consecutive active transitions:

```
For non-inverted (invert_rx=false, rx_idle_level=false): process on pin LOW
  → measures HIGH (idle) pulse width
For inverted (invert_rx=true, rx_idle_level=true): process on pin HIGH
  → measures LOW (idle) pulse width
```

**Decoding logic** (from the interrupt handler):
- Measure time between consecutive active edges (idle pulse width).
- If duration < ZERO_TIME (2000 µs) → short idle → Manchester bit = 0.
- If duration > ZERO_TIME → long idle → Manchester bit = 1.
- If duration > IDLE_TIME (5000 µs) → bus was idle → start of new packet.

### 4.4 16-bit Word Layout

```
Bit:    15       14    13    12    11    10     9     8     7     6     5     4     3     2     1     0
Field: [Parity] [---- ID ----] [---------------- DATA ----------------] [--------- CHECKSUM ---------]
Width:    1          3                       8                                4
```

| Field    | Bits | Description |
|----------|------|-------------|
| Parity   | 15   | Even parity over all 16 bits (including this bit) |
| ID       | 14–12 | Device type or group identifier (3 bits, 0–7) |
| Data     | 11–4 | Payload byte (8 bits, 0–255) |
| Checksum | 3–0  | 4-bit checksum |

**Packet validation (`isValid()`):**
- Count set bits (popcount) across all 16 bits using `__builtin_popcount()`
  (single Xtensa instruction on ESP8266/ESP32, vs 16-iteration loop).
- If popcount is even, parity is correct → return true.
- If popcount is odd → packet is corrupt → discard.

### 4.5 Reception Algorithm (Interrupt Handler)

The ISR fires on every CHANGE of the RX pin. It measures **idle pulse widths**
(durations between consecutive active edges) by processing on the **active bus
level**:

```
ISR on CHANGE of RX pin
│
├─ Read current micros → curr_micros
├─ length = curr_micros - prev_micros
│
├─ Read pin via direct GPIO register (cached pin number)
├─ IF pin_level != rx_idle_level_ (active level, precomputed from invert_rx):
│     IF length > IDLE_TIME (5000 µs):
│         // Bus was idle — new packet starting
│         recbuf = 0
│         bit_number = 0
│     ELSE IF bit_number < 16:
│         // Data bit — length = idle pulse width between active edges
│         recbuf = recbuf << 1
│         IF length > ZERO_TIME (2000 µs):
│             recbuf |= 1   // long idle → Manchester bit 1
│         ELSE:
│             recbuf |= 0   // short idle → Manchester bit 0
│         bit_number++
│         IF bit_number == 16:
│             circular_buffer.push(recbuf)
│             recbuf = 0
│             bit_number = 0
│
└─ prev_micros = curr_micros
```

**Processing on active level:** The ISR fires on every pin CHANGE. When the
pin transitions to the ACTIVE level (the level that is NOT the idle bus
state), it measures the time elapsed since the last active transition. That
time is the **idle pulse width** — the duration the bus spent at the idle
level between two active pulses. Manchester-encoded bits use:
- Long idle (>2000 µs) = Manchester bit 1 (short active + long idle)
- Short idle (<2000 µs) = Manchester bit 0 (long active + short idle)

The start bit (1× BIT_TIME active) is followed by the first idle period; a
long idle (>IDLE_TIME) resets the packet decoder, ensuring the start bit's
active pulse is not misinterpreted as a data bit. This matches how the
reference library (`x28-mpx-controller`) processes the bus.

**Critical implementation notes:**
- The ISR is marked `IRAM_ATTR` and placed in IRAM on ESP32/ESP8266.
- On ESP8266 and ESP32, the pin is read via direct GPIO register access
  (`GPIO_REG_READ(GPIO_IN_ADDRESS)` / `REG_READ(GPIO_IN_REG)`) using a
  cached pin number. This avoids virtual dispatch through the ESPHome GPIO
  abstraction layer and ensures IRAM safety. On other platforms, falls back
  to `digitalRead()`.
- `rx_idle_level_` is precomputed during `setup()`: `!invert_rx`. For
  non-inverted buses this is `false` (idle=HIGH, process on LOW = active);
  for inverted buses it is `true` (idle=LOW, process on HIGH = active).
- All ISR state (`recbuf_`, `bit_number_`, `prev_micros_`) lives in DRAM
  accessed through a static `instance_` pointer — no flash access in the ISR.
- `micros()` returns `unsigned long` (32-bit, wraps every ~71 minutes). Wrap
  handling is implicit in unsigned subtraction.

### 4.6 Transmission Algorithm

```
sendPacket(word):
│
├─ WAIT: bus idle for CTS_TIME (25000 µs)
│   └─ Poll RX pin (direct GPIO read); reset idle counter on ACTIVE bus
│
├─ Detach RX interrupt (prevents false triggers from own TX edges)
├─ Enable TX: set TX pin as OUTPUT
│
├─ Precompute register masks for invert_tx:
│   reg_idle   = invert_tx ? W1TC : W1TS
│   reg_active = invert_tx ? W1TS : W1TC
│
├─ START BIT:
│   └─ MPX_REG_WRITE(reg_active, 1 << pin) for BIT_TIME (1270 µs)
│
├─ 16 DATA BITS (MSB first, i = 15 → 0):
│   Precomputed register writes — no per-bit branching on invert_tx:
│   IF bit i of word == 0:
│       MPX_REG_WRITE(reg_idle, 1 << pin)   for BIT_TIME
│       MPX_REG_WRITE(reg_active, 1 << pin) for 2 * BIT_TIME
│   ELSE: // bit == 1
│       MPX_REG_WRITE(reg_idle, 1 << pin)   for 2 * BIT_TIME
│       MPX_REG_WRITE(reg_active, 1 << pin) for BIT_TIME
│
├─ STOP BIT:
│   └─ MPX_REG_WRITE(reg_idle, 1 << pin) for BIT_TIME
│
├─ Disable TX: set TX pin as INPUT (high-Z)
├─ Reattach RX interrupt (CHANGE)
└─ Return
```

**CTS wait logic:**
- The bus is idle-HIGH. Before transmitting, wait until the bus has been idle
  continuously for CTS_TIME. If any bus activity is detected during the wait,
  reset the timer.
- The check compares the raw pin level against `rx_idle_level_`. When the
  level matches (bus is active, i.e., not at the idle level), the idle timer
  is reset.

**Interrupt discipline:**
- Only the RX interrupt is detached during transmission (via
  `detachInterrupt()` / `attachInterrupt()`), not all interrupts. This allows
  WiFi, timers, and other system interrupts to fire normally during the
  ~61 ms TX window — critical for ESP8266 stability.
- The Manchester-encoded bit loop uses direct GPIO register writes
  (`GPIO_OUT_W1TS` / `GPIO_OUT_W1TC` on ESP8266, `GPIO.out_w1ts` /
  `GPIO.out_w1tc` on ESP32) rather than `digitalWrite()`, reducing per-bit
  overhead from ~1µs to ~0.1µs. On other platforms, falls back to the
  ESPHome `digital_write()` API.

**Platform-specific GPIO macros (defined in `x28.cpp`):**

| Platform | Read pin | Set pin high | Set pin low |
|----------|----------|--------------|-------------|
| ESP8266 | `GPIO_REG_READ(GPIO_IN_ADDRESS)` | `GPIO_REG_WRITE(GPIO_OUT_W1TS_ADDRESS, mask)` | `GPIO_REG_WRITE(GPIO_OUT_W1TC_ADDRESS, mask)` |
| ESP32 | `REG_READ(GPIO_IN_REG)` | `REG_WRITE(GPIO_OUT_W1TS_REG, mask)` | `REG_WRITE(GPIO_OUT_W1TC_REG, mask)` |
| Other | `digitalRead()` | `digitalWrite(HIGH)` | `digitalWrite(LOW)` |

### 4.7 Bus Arbitration

The MPX bus is an open-collector / active-low shared bus. Multiple devices can
drive the bus LOW simultaneously without damage (wired-AND). The bus is
idle-HIGH (pulled up by resistors in the central and devices).

**Collision avoidance:**
- Every device must wait CTS_TIME of idle before starting a transmission.
- The central panel has priority — it can transmit after IDLE_TIME.
- Keyboard/keypad transmissions have lower priority; the ESP should behave
  as a keyboard device.
- In practice, collisions are unlikely because the ESP is the only device
  transmitting on behalf of the user. The bus is mostly receive-only for the
  ESP during normal operation.

## 5. Known MPX Packet Codes

### 5.1 Alarm State Events (received from the central panel)

These packets are transmitted by the central panel to all bus devices when the
alarm state changes. The ESP receives them passively.

| Event              | Code      | ID (bin) | Data (hex) | Description |
|--------------------|-----------|----------|------------|-------------|
| ALARM_ARMED        | `0x49C1`  | 001      | 0x9C       | System armed (away or stay) |
| ALARM_DISARMED     | `0xC92B`  | 110      | 0x92       | System disarmed |
| ESTOY              | `0x4BE8`  | 001      | 0xBE       | Stay mode selected |
| ME_VOY             | `0xCBAE`  | 110      | 0xBA       | Away mode selected |
| Z1_MPXH            | `0x1615`  | 000      | 0x61       | Zone 1 sensor triggered (MPXH) |
| Z2_MPXH            | `0x1623`  | 000      | 0x62       | Zone 2 sensor triggered (MPXH) |
| Z3_MPXH            | `0x9630`  | 001      | 0x63       | Zone 3 sensor triggered (MPXH) |
| Z4_MPXH            | `0x1640`  | 000      | 0x64       | Zone 4 sensor triggered (MPXH) |
| Z1_WIRED           | `0xB08A`  | 101      | 0x08       | Zone 1 sensor triggered (wired) |
| Z2_WIRED           | `0xB045`  | 101      | 0x04       | Zone 2 sensor triggered (wired) |
| Z3_WIRED           | `0xB026`  | 101      | 0x02       | Zone 3 sensor triggered (wired) |
| Z4_WIRED           | `0xB010`  | 101      | 0x01       | Zone 4 sensor triggered (wired) |

**Zone code generation:** MPXH zone codes follow a consistent pattern for all
zones 1–32: data byte = `0x60 + zone`, checksum cycles through `[5, 3, 0, 0]`
by `(zone-1) % 4`, with even parity over all 16 bits. Wired codes are known
only for zones 1–8 (enum constants in code). The component generates MPXH
codes on-the-fly via `mpxh_code_for_zone()` and matches against wired codes
up to the model's `max_wired_zones`. Codes for specific zones can be
overridden via `zone_code_overrides` config.

### 5.2 Zone Status Interpretation

Zone sensors periodically (or on state change) transmit their state as a
packet on the bus. The packet indicates that the sensor has been **triggered**
(zone open / alarm condition). Once the sensor returns to normal, it transmits
no packet — the zone is assumed to have returned to normal.

**Behaviour observed in the reference library:**
- Zone packets are momentary events, not continuous states.
- The ESP component publishes the binary_sensor as `ON` when the packet is
  received, and automatically resets to `OFF` after a configurable debounce
  window (default 500 ms).
- If the sensor is held open (e.g., a door held ajar), it will re-transmit
  its zone packet periodically (typical interval undocumented — requires
  sniffing to determine).

### 5.3 Keyboard Codes (transmitted by the ESP to the panel)

When the ESP sends a key press, it transmits the corresponding 16-bit packet.
Some keys have a "long press" variant that sends a second packet after the
first.

| Key                 | Short press | Long press (additional packet) |
|---------------------|-------------|--------------------------------|
| KEY_0               | `0x0000`    | — |
| KEY_1               | `0x8013`    | — |
| KEY_2               | `0x8025`    | — |
| KEY_3               | `0x0036`    | — |
| KEY_4               | `0x8046`    | — |
| KEY_5               | `0x0055`    | — |
| KEY_6               | `0x0063`    | — |
| KEY_7               | `0x8070`    | — |
| KEY_8               | `0x8089`    | — |
| KEY_9               | `0x009A`    | — |
| KEY_P               | `0x00AC`    | `0x810A` |
| KEY_F               | `0x80BF`    | `0x813C` |
| KEY_PANIC           | `0x80EA`    | `0x012F` |
| KEY_FIRE            | `0x00F9`    | `0x0119` |
| KEY_MODO            | `0x80DC`    | — |
| KEY_ZONA_IN         | `0x00CF` then `0x0000` | — |
| KEY_ZONA_OUT        | `0x00CF` then `0x8169` | — |

**Long press implementation:** Send the short press packet, wait 500 ms
(packet time + margin), then send the second packet. This matches how
physical keypads behave.

### 5.4 Keyboard Code Set (for `sendKeys()`)

The keyboard code array from `x28-mpx-controller` contains 25 entries.
Index 0 = KEY_0, index 1 = KEY_1, ... index 9 = KEY_9,
then special keys at indices 10–24:

| Index | Key               | Code      |
|-------|-------------------|-----------|
| 0     | KEY_0             | `0x0000`  |
| 1     | KEY_1             | `0x8013`  |
| 2     | KEY_2             | `0x8025`  |
| 3     | KEY_3             | `0x0036`  |
| 4     | KEY_4             | `0x8046`  |
| 5     | KEY_5             | `0x0055`  |
| 6     | KEY_6             | `0x0063`  |
| 7     | KEY_7             | `0x8070`  |
| 8     | KEY_8             | `0x8089`  |
| 9     | KEY_9             | `0x009A`  |
| 10    | KEY_P             | `0x00AC`  |
| 11    | KEY_P_LONG_PRESS  | `0x00AC` + `0x810A` |
| 12    | KEY_F             | `0x80BF`  |
| 13    | KEY_F_LONG_PRESS  | `0x80BF` + `0x813C` |
| 14    | KEY_ZONA_IN       | `0x00CF` |
| 15    | KEY_MODO          | `0x80DC`  |
| 16    | KEY_PANIC         | `0x80EA`  |
| 17    | KEY_P             | (duplicate of 10) |
| 18    | KEY_P_LONG_PRESS  | (duplicate of 11) |
| 19    | KEY_F             | (duplicate of 12) |
| 20    | KEY_F_LONG_PRESS  | (duplicate of 13) |
| 21    | KEY_FIRE          | `0x00F9`  |
| 22    | KEY_PANIC_LONG_PRESS | `0x80EA` + `0x012F` |
| 23    | KEY_FIRE_LONG_PRESS  | `0x00F9` + `0x0119` |
| 24    | KEY_ZONA_OUT      | `0x00CF` + `0x8169` |

## 6. Component Architecture

### 6.1 File Layout

```
components/
└── x28_alarm/
    ├── __init__.py            # Component registration, CONFIG_SCHEMA, C++ code generation
    ├── alarm_control_panel.py # AlarmControlPanel entity class
    ├── binary_sensor.py       # Zone + Estoy binary sensors
    ├── button.py              # Panic, Fire buttons
    ├── text_sensor.py         # Sniffer output text sensor (optional)
    ├── switch.py              # Sniffer toggle switch entity
    ├── x28.h                  # C++ header: MPXBus, X28Alarm, packet types
    └── x28.cpp                # C++ implementation
```

### 6.2 Python Module Responsibilities

**`__init__.py`:**
- Define `CONFIG_SCHEMA` with all configuration parameters.
- Register the `x28_alarm` component with `async_to_py()` setup.
- Generate C++ code for `MPXBus` and `X28Alarm` instantiation.
- Register `send_keys` HA service.
- Validate configuration: pins are integers, code is 4-6 digit string, etc.

**`alarm_control_panel.py`:**
- Define `CONFIG_SCHEMA` for the alarm control panel platform.
- Generate C++ code for an `AlarmControlPanel` subclass.
- Wire up arm/disarm actions to `MPXBus::send_keys()`.

**`binary_sensor.py`:**
- Define `CONFIG_SCHEMA` for zone and estoy binary sensors.
- Accept `zone` parameter: `ESTOY` or `1`–`32` (capped by model capabilities).
- Generate C++ code for binary sensor publish calls.

**`button.py`:**
- Define `CONFIG_SCHEMA` for action buttons.
- Accept `action` parameter: `PANIC`, `FIRE`.
- Generate C++ code to send the appropriate key packet on press.

### 6.3 YAML Configuration Schema (Complete)

```yaml
# ─── Minimal configuration ───────────────────────────────────────────────

x28_alarm:
  rx_pin: GPIO22
  tx_pin: GPIO23

# ─── Full configuration ──────────────────────────────────────────────────

x28_alarm:
  id: my_alarm
  rx_pin:
    number: GPIO22
    inverted: true          # default: true
  tx_pin:
    number: GPIO23
    inverted: true          # default: true
  model: N8F-MPXH           # optional: model override (default: AUTO)
  debug: true               # enable verbose ESP_LOGV output
  sniffing:
    enabled: true
    throttle_ms: 1000       # min ms between duplicate packet logs
  virtual_zones:
    - zone: 5
      sensor_id: binary_sensor.back_door
      trigger: ON
      zone_type: MPXH
      clear_on_close: true
    - zone: 6
      sensor_id: binary_sensor.garage
      trigger: ON
      zone_type: MPXH
      clear_on_close: true
  zone_debounce_ms: 500     # how long to hold zone ON before auto-clearing

# ─── Entities ────────────────────────────────────────────────────────────

alarm_control_panel:
  - platform: x28_alarm
    name: "Home Alarm"
    id: alarm_panel
    # Automatically uses code from x28_alarm config

binary_sensor:
  - platform: x28_alarm
    name: "Estoy Mode"
    id: sensor_estoy
    zone: ESTOY

  - platform: x28_alarm
    name: "Zone 1"
    zone: 1
  - platform: x28_alarm
    name: "Zone 2"
    zone: 2
  - platform: x28_alarm
    name: "Zone 3"
    zone: 3
  - platform: x28_alarm
    name: "Zone 4"
    zone: 4
  - platform: x28_alarm
    name: "Zone 5"
    zone: 5
  - platform: x28_alarm
    name: "Zone 6"
    zone: 6
  - platform: x28_alarm
    name: "Zone 7"
    zone: 7
  - platform: x28_alarm
    name: "Zone 8"
    zone: 8

text_sensor:
  - platform: x28_alarm
    name: "Bus Sniffer"
    id: sniffer_output

button:
  - platform: x28_alarm
    name: "Panic"
    action: PANIC
  - platform: x28_alarm
    name: "Fire"
    action: FIRE
  - platform: x28_alarm
    name: "Panic Long"
    action: PANIC_LONG
  - platform: x28_alarm
    name: "Fire Long"
    action: FIRE_LONG
```

### 6.4 Configuration Parameters (Complete Reference)

| Parameter           | Type      | Required | Default  | Description |
|---------------------|-----------|----------|----------|-------------|
| `id`                | string    | no       | —        | Component ID for referencing |
| `rx_pin`            | pin       | yes      | —        | Input pin connected to MPX bus |
| `tx_pin`            | pin       | yes      | —        | Output pin driving the bus |
| `model`             | string    | no       | `AUTO`   | X-28 model for capability checks (see §3.2) |
| `invert_tx`         | boolean   | no       | `true`   | Invert TX signal |
| `invert_rx`         | boolean   | no       | `true`   | Invert RX signal |
| `debug`             | boolean   | no       | `false`  | Enable verbose ESP_LOGV logging |
| `zone_debounce_ms`  | int       | no       | `500`    | Auto-clear time for zone sensors (ms) |
| `sniffing.enabled`  | boolean   | no       | `false`  | Enable all-packet bus sniffing |
| `sniffing.throttle_ms` | int    | no       | `1000`   | Min interval between duplicate packet logs |
| `virtual_zones`     | list      | no       | `[]`     | List of virtual zone mappings |

**Virtual zone sub-parameters:**

| Parameter        | Type    | Required | Default  | Description |
|------------------|---------|----------|----------|-------------|
| `zone`           | int (1-32) | yes   | —        | X-28 zone number to emulate (capped by model capabilities) |
| `sensor_id`      | string  | yes      | —        | ESPHome entity ID to monitor |
| `trigger`        | string  | no       | `ON`     | State that triggers zone packet (`ON`, `OPEN`) |
| `zone_type`      | string  | no       | `MPXH`   | Packet type: `MPXH` or `WIRED` |
| `clear_on_close` | boolean | no       | `true`   | Send zone restore packet when sensor clears |

### 6.5 Code Generation Phase

During ESPHome's C++ code generation (after validation), the Python modules
emit C++ code that:

1. Instantiates a global `X28Alarm` component.
2. Calls `X28Alarm::set_rx_pin()`, `set_tx_pin()`, `set_code()`, `set_model()`, etc. with
   values from the YAML config.
3. Creates `AlarmControlPanel` subclass instance and registers it with
   `App.register_alarm_control_panel()`.
4. Creates `binary_sensor::BinarySensor` instances for each zone and the
   Estoy sensor.
5. Creates `button::Button` instances for each action button.
6. Creates `text_sensor::TextSensor` instance if sniffing is enabled.
7. Registers the `send_keys` service with `App.register_service()`.
8. For virtual zones, stores sensor ID → zone mapping and registers a
   callback on the target binary sensor.

## 7. Entity Definitions

### 7.1 Alarm Control Panel (`alarm_control_panel`)

Implements ESPHome's `AlarmControlPanel` abstract class.

**States:**

| HA State        | Triggered When |
|-----------------|----------------|
| DISARMED        | `0xC92B` received from panel, or after successful `disarm()` call |
| ARMED_AWAY      | `0x49C1` received AND last mode was ME_VOY (or no mode tracked) |
| ARMED_HOME      | `0x49C1` received AND last mode was ESTOY |
| PENDING         | After user issues arm command, waiting for panel confirmation |
| TRIGGERED       | ARMED + zone packet received (zone opened while system is armed) |

**Actions:**

| Action         | Implementation |
|----------------|----------------|
| `arm_away()`   | 1. If currently in Estoy mode, press MODO (toggle to Me Voy). 2. Wait up to 1 s per attempt (up to 10 attempts) for ME_VOY packet. 3. Call `send_keys(code)`. 4. Set state to PENDING. 5. Wait for ALARM_ARMED packet → set ARMED_AWAY. 6. Timeout after 10 s → revert to DISARMED + log error. |
| `arm_home()`   | 1. If currently in Me Voy mode, press MODO (toggle to Estoy). 2. Wait up to 1 s per attempt (up to 10 attempts) for ESTOY packet. 3. Call `send_keys(code)`. 4. Set state to PENDING. 5. Wait for ALARM_ARMED packet → set ARMED_HOME. 6. Timeout after 10 s → revert. |
| `disarm()`     | 1. Call `send_keys(code)`. 2. Set state to PENDING. 3. Wait for ALARM_DISARMED packet → set DISARMED. 4. Timeout after 10 s → log error. |

**Mode tracking:**
- On `ESTOY` packet receipt: `_last_mode = ESTOY`.
- On `ME_VOY` packet receipt: `_last_mode = ME_VOY`.
- On `ALARM_DISARMED` packet receipt: `_last_mode` unchanged (mode is
  remembered across arm/disarm cycles).
- On user `arm_home()` when `_last_mode != ESTOY`: send MODO(s) until
  panel responds with ESTOY packet, then send code.
- On user `arm_away()` when `_last_mode != ME_VOY`: send MODO(s) until
  panel responds with ME_VOY packet, then send code.

**TRIGGERED state:**
- Entered when: state is ARMED (either mode) AND a zone packet is received.
- Exited when: state transitions to DISARMED (user disarms via code).
- Once triggered, the ACP remains in TRIGGERED until disarmed, regardless
  of zone state. This matches real alarm behaviour.

### 7.2 Binary Sensors (`binary_sensor`)

**Estoy Mode Sensor:**
- `zone: ESTOY`
- `ON` when `0x4BE8` (ESTOY) packet received.
- `OFF` when `0xCBAE` (ME_VOY) or `0xC92B` (DISARMED) received.

**Zone Sensors:**
- `zone: 1` through `zone: N` where N is the model's max zone count (see §3.2).
  For AUTO mode, zones 1–8 are available by default and more can be added as
  they are detected on the bus.
- `ON` when a zone packet matching that zone is received (both MPXH and wired
  codes are checked).
- `OFF` after `zone_debounce_ms` (default 500 ms) of no matching packet.
- The debounce timer is a simple "last received" timestamp. On each matching
  packet, the timer is extended.
- This implements a retriggerable one-shot: the sensor stays ON as long as
  packets keep arriving, and auto-clears when they stop.

**Zone code matching:** The component generates MPXH codes dynamically for all
zones 1–32 using the formula: data = `0x60 + zone`, checksum cycled from
`[5, 3, 0, 0]` by `(zone-1) % 4`, with even parity. For wired zones, known
codes exist only for zones 1–8 (see §5.1). The match loop is bounded by
`model_capabilities_.max_mpxh_zones` and `model_capabilities_.max_wired_zones`.
Any zone code can be overridden via `zone_codes` config.

### 7.3 Buttons (`button`)

Standard ESPHome `button` entities. On press, the appropriate key sequence
is transmitted.

| Button action  | Key sequence sent       | Notes |
|----------------|-------------------------|-------|
| `PANIC`        | `MPX_KEY_PANIC`         | Short press — triggers panic alarm |
| `PANIC_LONG`   | `MPX_KEY_PANIC_LONG_PRESS` | Long press — may activate additional alarm features |
| `FIRE`         | `MPX_KEY_FIRE`          | Short press — triggers fire alarm |
| `FIRE_LONG`    | `MPX_KEY_FIRE_LONG_PRESS`  | Long press |

### 7.4 Text Sensor (Sniffer Output)

When `sniffing.enabled` is true, a `text_sensor` is created that publishes
the latest decoded packet as a hex string:

```
0x49C1
```

Keyboard codes are not published to the text sensor (they are filtered in
the C++ event handler), but all packets appear in the ESP logs.

### 7.5 Sniffing Mode (Detailed)

Sniffing mode exposes all bus traffic, not just recognized event codes.
This is the primary tool for discovering new packet codes and debugging.

**What it logs:**

Every valid packet received, including:
- Known event codes (alarm state, zone triggers).
- Keyboard codes (keys pressed on physical keypads).
- Unknown/uncategorized codes.
- Packets with parity errors (logged separately as warnings).

**What it does NOT log (to avoid flooding):**
- Duplicate packets within `throttle_ms` (default 1000 ms) that are
  identical to the last logged packet.
- Transmitted packets (they are logged separately by the TX path if
  debug mode is on).

**Log output (ESP_LOGV):**
```
PKT 0x49C1 P=0 ID=1 DATA=0x9C CS=1
PKT 0x8013 P=1 ID=0 DATA=0x13 CS=3
PKT 0x1615 P=0 ID=0 DATA=0x61 CS=5
```

Keyboard codes are logged separately at `ESP_LOGV`:
```
KBD code 0x8013
```

**Text sensor throttling:**
- Not every sniffed packet is published to the text sensor (that would
  overwhelm HA).
- Only the latest packet is published, updated at a maximum rate of 1 Hz.
- All packets are available in the ESP logs at `ESP_LOGV` level (visible
  when `debug: true` is set).

### 7.6 Virtual Zone Sensors

Virtual zones bridge HA-connected binary sensors into the X-28 alarm system
as if they were physical MPX zones. When the HA sensor triggers, the ESP
transmits the appropriate zone packet onto the MPX bus.

**Use case example:**
You have an Aqara door sensor connected via Zigbee2MQTT → Home Assistant →
ESPHome. With virtual zones, triggering that door sensor causes the X-28
panel to see it as zone 5, sound the siren, and notify the monitoring
station.

**Configuration:**
```yaml
x28_alarm:
  virtual_zones:
    - zone: 5
      sensor_id: binary_sensor.back_door
      trigger: ON
      zone_type: MPXH
      clear_on_close: true
    - zone: 6
      sensor_id: binary_sensor.garage_motion
      trigger: ON
      zone_type: MPXH
      clear_on_close: true
```

**How it works:**
1. In the C++ `loop()`, the `X28Alarm` component polls each virtual zone's
   sensor entity via `id(sensor_id)` → `BinarySensor::state`.
2. On a rising edge (OFF → ON), it calls:
   - `_mpxbus.send_packet(zone_code)` where `zone_code` is the MPXH or wired
     code for that zone.
3. If `clear_on_close` is true, on the falling edge (ON → OFF), it sends the
   same packet again. (This signals "zone restored" to the panel.)
4. Zone packets are sent with proper CTS timing, same as any other
   transmission.

**Important considerations:**
- The X-28 panel expects zone packets to be transmitted by the sensor itself.
  There is no explicit "zone closed" packet — the panel infers closure from
  the absence of trigger packets over a timeout. Sending a second packet
  on sensor clear may or may not restore the zone, depending on the panel
  firmware. Testing is required.
- If the panel ignores the second packet, the alternative is: clear_on_close
  does nothing, and the zone auto-restores after the panel's own timeout
  (typically 2–10 seconds for momentary sensors).

### 7.7 `send_keys` Service

A generic HA service enabling any key sequence to be sent to the alarm panel.
This unlocks all programming and configuration operations from Home Assistant.

**Service definition:**
```yaml
service: x28_alarm.send_keys
fields:
  keys:
    required: true
    type: string
    description: "Key sequence. Use 0-9 for digits, P, p, F, f, M, Z, L, !, @ for special keys. See mapping below."
    example: "282828PPpP88130"
```

**Key character mapping:**

| Char | MPXKey | Notes |
|------|--------|-------|
| `0`–`9` | KEY_0 – KEY_9 | Digit keys, same as pressing on keypad |
| `P` | KEY_P | Short press of P key |
| `p` | KEY_P_LONG | Long press of P key (hold ~1 s) |
| `F` | KEY_F | Short press of F key |
| `f` | KEY_F_LONG | Long press of F key |
| `M` | KEY_MODO | MODO key (toggle Estoy/Me Voy) |
| `Z` | KEY_ZONA_IN | ZONA key alone (`0x00CF`), no trailing "00" |
| `L` | KEY_ZONA_OUT | ZONA key followed by OUT code |
| `!` | KEY_PANIC | Panic short press |
| `@` | KEY_FIRE | Fire short press |
| `#` | KEY_PANIC_LONG | Panic long press |
| `*` | KEY_FIRE_LONG | Fire long press |

**Implementation in C++ (`MPXBus::send_keys`):**
```cpp
void MPXBus::send_keys(const char *keys, size_t len) {
  for (size_t i = 0; i < len; i++) {
    char c = keys[i];
    if (c >= '0' && c <= '9')
      send_key(c - '0');
    else if (c == 'P')
      send_key(10);
    else if (c == 'p') {
      send_packet(0x00AC);
      delay(KEY_DELAY_MS);
      send_packet(0x810A);
    } else if (c == 'F')
      send_key(11);
    else if (c == 'f') {
      send_packet(0x80BF);
      delay(KEY_DELAY_MS);
      send_packet(0x813C);
    } else if (c == 'M')
      send_key(13);
    else if (c == 'Z')
      send_packet(0x00CF);
    else if (c == 'L') {
      send_packet(0x00CF);
      delay(KEY_DELAY_MS);
      send_packet(0x8169);
    } else if (c == '!')
      send_key(14);
    else if (c == '@')
      send_key(15);
    else if (c == '#') {
      send_packet(0x80EA);
      delay(KEY_DELAY_MS);
      send_packet(0x012F);
    } else if (c == '*') {
      send_packet(0x00F9);
      delay(KEY_DELAY_MS);
      send_packet(0x0119);
    }
    delay(KEY_DELAY_MS);
  }
}
```

**Timing:** Between each key in a sequence, send 150 ms delay to match
typical keypad inter-key timing. The panel expects keys at roughly human
speed — too fast may cause missed keys.

### 7.8 Full Documented Interaction Map

All known X-28 panel operations expressed as key sequences for the
`send_keys` service.

#### 7.8.1 Basic Operation

| Action                       | Key Sequence          |
|------------------------------|-----------------------|
| Arm (current mode)           | `<code>`              |
| Arm Me Voy (Away)            | `M<code>`             |
| Arm Estoy (Stay)             | `MM<code>`            |
| Disarm                       | `<code>`              |
| Panic                        | `!`                   |
| Fire                         | `@`                   |

#### 7.8.2 Programming Mode Entry

| Action                      | Key Sequence |
|-----------------------------|--------------|
| Basic programming           | `<code>PP`   |
| Advanced programming        | `<code>PPp`  |
| Exit programming            | `F`          |

Where `<code>` is the owner or installer code.

#### 7.8.3 Advanced Programming (P-codes)

All P-codes are entered from advanced programming mode. The table below is sourced from the official N-series installer manuals (available at `manuales.x-28.com`).

| Code | Function | Sequence | Parameters | Models | Factory |
|------|----------|----------|------------|--------|---------|
| 770 | PGM0 / LED output | `P770<O><P>` | O=0-9 (option), P=1-8 (partition) | N4,N8,N16,N32 | Activated in partition 1 |
| 771 | PGM1 output | `P771<O><P>` | O=0-9 (option), P=1-8 (partition) | N8,N16,N32 | — |
| 772 | PGM2 output | `P772<O><P>` | O=0-9 (option), P=1-8 (partition) | N8,N16,N32 | — |
| 773 | AC line frequency | `P7730` or `P7731` | 0=50Hz, 1=60Hz | All N-series | 50Hz |
| 774 | Sabotage inhibit | `P7740` or `P7741` | 0=enabled, 1=inhibited | All N-series | Enabled |
| 775 | Siren B duration | `P775<MM>` | MM=01–12 minutes | N8,N16,N32,N8F,N32F | 4min |
| 776 | Entry annunciator | `P7760` or `P7761` | 0=off, 1=on | All N-series | Off |
| 777 | Clock source | `P7770` or `P7771` | 0=grid (50Hz), 1=crystal | All N-series | Crystal |
| 778 | Save zone config for mode | `P7781` or `P7782` | 1=Estoy, 2=MeVoy | All N-series | — |
| 880 | Owner code condition | `P8800` or `P8801` | 0=arm+disarm, 1=disarm only | All N-series | Arm+disarm |
| 881 | Entry delay | `P881<SS>` | SS=05–99 seconds | All N-series | 20s |
| 882 | Exit delay | `P882<SS>` | SS=15–99 seconds | All N-series | 60s |
| 883 | Alarm siren A duration | `P883<MM>` | MM=01–12 minutes | All N-series | 4min |
| 884 | Zones 2 and 4 conditionality | `P8840` or `P8841` | 0=no, 1=yes | All N-series | No |
| 885 | Enable/disable wired zones | `P8850` or `P8851` | 0=disable, 1=enable | N4,N8,N16,N32 | Enabled |
| 886 | Battery save mode | `P8860` or `P8861` | 0=normal, 1=save | All N-series | Normal |
| 888 | Hermanar particiones | `P8880` or `P8881` | 0=no, 1=yes | N16,N32,N32F | No |
| 889 | Installer code | `P889<XXXXXX>` | 6 digits | All N-series | 467825 |
| 990 | Query zone config | `P990<N>` | N=zone number (read-only, TLCD) | All N-series | — |
| 991 | Output B only | `P991<N>` / `P991<NN>` | N=1..8 (N8, N8F), NN=1..32 (N16,N32) | N8,N16,N32,N8F,N32F | None |
| 992 | Output A+B | `P992<N>` / `P992<NN>` | N=1..8 (N8, N8F), NN=1..32 (N16,N32) | N8,N16,N32,N8F,N32F | All zones |
| 993 | Fire zone | `P993<N>` / `P993<NN>` | N=1..8 (N8, N8F), NN=1..32 (N16,N32) | All N-series | None |
| 994 | Normal robbery zone | `P994<N>` / `P994<NN>` | N=1..8 (N8, N8F), NN=1..32 (N16,N32) | All N-series | All zones |
| 995 | 24h protection zone | `P995<NN>` | NN=09–16 | N16,N32,N32F | None |
| 996 | Fast robbery zone (seismic) | `P996<N>` | N=1–8 | N4,N8,N16,N32 | None |
| 997 | Panic zone (zone 7 fixed) | `P997` | — | N8,N16,N32,N8F,N32F | None |
| 998 | Sabotage zone (zone 8 fixed) | `P998` | — | N8,N16,N32,N8F,N32F | None |

**Notes:**
- Zone parameters use 1 digit for N4/N8/N8F and 2 digits for N16/N32/N32F
- P 997 and P 998 are hardwired for zones 7 and 8 respectively (no zone parameter)
- P 885 is absent on "F" models (no wired zone inputs)
- P 888 requires a partition plug-in board (E1P, E3P, or E7P series)
- P 995 applies to zones 9–16 only (requires N16+ with sufficient zones)
- P 996 is for seismic/impact detectors (robo rápida)
- P-codes P 770–P 772 (PGM) set output behavior based on system states (0=No asignada, 1=Activada, 2=Lista, 3=Modo Estoy, 4=Modo Me Voy, 5=Alarma disparada, 6=Pedido de ayuda (4s monostable), 7=Asalto (4s monostable), 8=Falta línea, 9=Falta red); partition defaults to 1. Values per official N-series manual.

**Note on P-code range overlap:** P-codes in the 880–889 range are shared between the central panel and keypad programming, but accessed via different entry sequences:
- **Central panel:** Enter via `<code>PPp` (advanced programming)
- **Keypad:** Enter via `F 83 25 23` (F TECLAD) while in central programming mode
The keypad's P881 assigns the keypad ID; the central's P881 sets entry delay. They are independent functions using the same code numbers.

**Example — set entry delay to 30 seconds:**
```yaml
service: x28_alarm.send_keys
data: { keys: "467825PPpP88130F" }
```
This enters with the installer code (467825), enters basic programming (PP),
enters advanced programming (p), sets entry delay to 30 seconds (P88130),
and exits (F).

#### 7.8.4 User Code Management

| Action                              | Sequence |
|-------------------------------------|----------|
| Program user 02 with code 1234 (arm all, can disarm) | `<code>PPF2633P024123411F` |
| Program user 02 with code 1234 (estoy only, can disarm) | `<code>PPF2633P024123411F` |

Sequence breakdown: `F2633` = user code programming command. `P02` = user number 02.
`4` = code length (single digit). `1234` = the code. `1` = arm permissions (see below).
`1` = can disarm. `F` = exit.

**Arm permission values:**
| Value | Meaning |
|-------|---------|
| 0 | Register only (users 16–31) |
| 1 | Estoy only |
| 2 | Me Voy only |
| 3 | Any mode |
| 4 | Assault code (triggers alarm subtly) |

#### 7.8.5 Remote Control / Sensor Coding

| Action                              | Sequence |
|-------------------------------------|----------|
| Enter RF learning mode              | `<code>PPF2337` |
| Learn transmitter (slot 02)         | `P02` then press TX button on remote |
| Learn sensor (slot 32)              | `P32` then trigger sensor |
| Delete device in slot 02            | `P020` |
| Exit RF learning                    | `F` |

#### 7.8.6 Zone Configuration for Estoy/Me Voy

| Action                              | Sequence |
|-------------------------------------|----------|
| Toggle zone 1 in current mode       | `Z01` (or `Z1` on N4/N8/N8F models; repeated toggles: included ↔ demorada ↔ excluded) |
| Save current config as Estoy mode   | `<code>PPpP7781F` |
| Save current config as Me Voy mode  | `<code>PPpP7782F` |

#### 7.8.7 Owner Code Change

| Action               | Sequence |
|----------------------|----------|
| Change owner code to 123456 | `467825PP123456` |

#### 7.8.8 Installer Code Change

| Action                          | Sequence |
|---------------------------------|----------|
| Change installer code to 999999 | `467825PPpP889999999F` |

### 7.9 High-Level Programming Services

The `send_keys` service (§7.7) is a low-level primitive that requires knowing
the exact key sequences for each operation. To eliminate memorising P-codes
and programming sequences, the component exposes high-level HA services that
wrap the most common operations. Each service internally constructs the
correct key sequence using the configured `code` (owner PIN) and
`installer_code` (default `467825`), then sends it to the panel.

**Two conventions:**
- `<code>` = owner PIN from the `code` config parameter.
- `<ic>` = installer code from the `installer_code` config parameter
  (default factory code `467825`).

#### 7.9.1 Programming Mode Entry/Exit

These are low-level services for manual programming mode control. Most users
will use the self-contained services in §7.9.2–§7.9.6 instead.

| Service | Parameters | Sequence sent | Description |
|---------|-----------|---------------|-------------|
| `enter_programming` | — | `<code>PP` | Enter basic programming mode |
| `enter_advanced_programming` | — | `<code>PPp` | Enter advanced programming mode |
| `exit_programming` | — | `F` | Exit programming mode |

```yaml
service: x28_alarm.enter_advanced_programming
```

#### 7.9.2 Panel Configuration (P-codes)

All services in this section are **self-contained**: they enter advanced
programming, set the value, and exit. No manual programming mode management
needed.

| Service | Parameters | Sequence sent | Description |
|---------|-----------|---------------|-------------|
| `set_entry_delay` | `seconds: 15–99` | `<ic>PPpP881<SS>F` | Entry delay in seconds |
| `set_exit_delay` | `seconds: 15–99` | `<ic>PPpP882<SS>F` | Exit delay in seconds |
| `set_siren_duration` | `minutes: 1–12` | `<ic>PPpP883<MM>F` | Siren A on-time in minutes |
| `set_siren_b_duration` | `minutes: 1–12` | `<ic>PPpP775<MM>F` | Siren B on-time in minutes |
| `set_clock_source` | `crystal: bool` | `<ic>PPpP777<N>F` | 0=grid (50Hz), 1=crystal |
| `set_sabotage_inhibit` | `enabled: bool` | `<ic>PPpP774<N>F` | 0=normal (habilitado), 1=inhibited — `enabled: true` INHIBITS sabotage |
| `set_ac_frequency` | `hz: 50\|60` | `<ic>PPpP773<N>F` | AC mains frequency |
| `set_entry_annunciator` | `enabled: bool` | `<ic>PPpP776<N>F` | Entry beep on/off |
| `set_annunciator_gap` | `seconds: 0–99` | `<ic>PPpP887<SS>F` | Annunciator beep interval in seconds (default 8) |
| `set_battery_save` | `enabled: bool` | `<ic>PPpP886<N>F` | Battery save mode |
| `set_owner_code_condition` | `disarm_only: bool` | `<ic>PPpP880<N>F` | 0=arm+disarm, 1=disarm only |
| `set_zone_conditionality` | `enabled: bool` | `<ic>PPpP884<N>F` | 0=no, 1=yes (zones 2 & 4 conditionality) |
| `set_wired_zones` | `enabled: bool` | `<ic>PPpP885<N>F` | 0=disable wired, 1=enable (N16/N32) |
| `set_partition_merge` | `enabled: bool` | `<ic>PPpP888<N>F` | 0=independent, 1=merged (N16/N32) |
| `set_siren_b_duration` | `minutes: 1–12` | `<ic>PPpP775<MM>F` | Siren B on-time in minutes |
| `set_clock_source` | `crystal: bool` | `<ic>PPpP777<N>F` | Clock source: 0=grid, 1=crystal |
| `set_wired_zones` | `enabled: bool` | `<ic>PPpP885<N>F` | Enable/disable wired zones (models with wired only) |
| `set_partition_merge` | `enabled: bool` | `<ic>PPpP888<N>F` | Merge partitions (N16/N32) |
| `set_pgm_output` | `output, option, partition` | `<ic>PPpP77OOPF` | PGM0/1/2 behavior |

Note: `<ic>` is the installer code (default factory code `467825`). Both the owner code (`code`) and installer code are **not configured in YAML** — they are set at runtime via the `change_owner_code` and `change_installer_code` services (see §7.9.4). The installer code defaults to `467825` on first boot.
RF learning services (`rf_learn_mode`, `rf_learn_slot`, `rf_delete_slot`,
`exit_rf_learning`) are only registered for models with `has_rf_learning == true`.
The `set_wired_zones` service is only registered for models with
`max_wired_zones > 0`.

```yaml
service: x28_alarm.set_entry_delay
data: { seconds: 30 }

service: x28_alarm.set_sabotage_inhibit
data: { enabled: true }

service: x28_alarm.set_clock_source
data: { crystal: true }

service: x28_alarm.set_ac_frequency
data: { hz: 50 }
```

#### 7.9.3 Zone Configuration

| Service | Parameters | Sequence sent | Self-contained? |
|---------|-----------|---------------|-----------------|
| `save_estoy_config` | — | `<code>PPpP7781F` | yes |
| `save_mevoy_config` | — | `<code>PPpP7782F` | yes |
| `set_zone_type` | `zone`, `type` | `<ic>PPpP99X<NN>F` | yes |
| `set_panic_zone` | — | `<ic>PPpP997F` | yes |
| `set_tamper_zone` | — | `<ic>PPpP998F` | yes |
| `toggle_zone_in_mode` | `zone: 1–N` | `Z<NN>` | no* |

`type` values for `set_zone_type`:

| Type string | P-code | Function | Models | Zone range |
|------------|--------|----------|--------|------------|
| `output_b` | 991 | Output on siren B only | N8,N8F,N16,N32,N32F | 1–8 or 1–32 |
| `output_ab` | 992 | Output on sirens A+B (factory default) | N8,N8F,N16,N32,N32F | 1–8 or 1–32 |
| `fire` | 993 | Fire zone | All N-series | 1–8 or 1–32 |
| `normal`, `robbery` | 994 | Normal robbery zone (factory default) | All N-series | 1–8 or 1–32 |
| `24h_protection` | 995 | 24-hour protection zone | N16,N32,N32F | 9–16 only |
| `fast_robbery` | 996 | Fast robbery zone (seismic) | N4,N8,N16,N32 | 1–4 or 1–8 or 1–32 |

`toggle_zone_in_mode` is not self-contained — the panel must already be in
zone configuration mode (press MODO + Zona keys on a physical keypad or use
`send_keys` to enter that state first).

```yaml
service: x28_alarm.save_estoy_config

service: x28_alarm.set_zone_type
data:
  zone: 3
  type: fire
```

#### 7.9.4 Code Management

| Service | Parameters | Sequence sent | Description |
|---------|-----------|---------------|-------------|
| `change_owner_code` | `new_code: 4–6 digits` | `<ic>PP<new_code>` | Changes the owner PIN. Uses the installer code (`<ic>`) to enter programming, per official procedure. |
| `change_installer_code` | `new_code: 6 digits` | `<ic>PPpP889<new_code>F` | Changes the installer code. Requires current installer code. |
| `program_user` | `user: 2–31`, `code: 4–6 digits`, `permissions: 0–4`, `can_disarm: bool` | `<code>PPF2633P<NN><L><CODE><AP><DP>F` | Programs a user code with permissions |

**`program_user` parameters in detail:**

| Param | Values | Description |
|-------|--------|-------------|
| `user` | 2–31 | User number (02–31) |
| `code` | 4–6 digits | The user's PIN |
| `code_len` | N/A (auto) | Code length sent as single digit (2–6), not zero-padded |
| `permissions` | 0=register, 1=Estoy, 2=Me Voy, 3=Any, 4=Assault | Arm permission level |
| `can_disarm` | bool | Whether user can disarm |

```yaml
# Change owner code to 123456
service: x28_alarm.change_owner_code
data: { new_code: "123456" }

# Program user 02 with code 4321, can arm all modes and disarm
service: x28_alarm.program_user
data:
  user: 2
  code: "4321"
  permissions: 3
  can_disarm: true
```

#### 7.9.5 RF / Wireless Sensor Management

| Service | Parameters | Sequence sent | Self-contained? |
|---------|-----------|---------------|-----------------|
| `rf_learn_mode` | — | `<code>PPF2337` | yes |
| `rf_learn_slot` | `slot: 2–32` | `P<NN>` | no* |
| `rf_delete_slot` | `slot: 2–32` | `P<NN>0` | no* |
| `exit_rf_learning` | — | `F` | no* |

`rf_learn_slot` and `rf_delete_slot` must be called while the panel is in RF
learning mode (after calling `rf_learn_mode`). `exit_rf_learning` exits RF
mode.

```yaml
service: x28_alarm.rf_learn_mode
# Then trigger the sensor or remote
service: x28_alarm.rf_learn_slot
data: { slot: 5 }
```

#### 7.9.6 Implementation in C++

Each high-level service is a thin wrapper around a `send_programmed_sequence`
helper that assembles the complete key sequence:

```cpp
void X28Alarm::send_programmed_sequence(const std::string &body,
                                        bool use_installer,
                                        bool advanced,
                                        bool exit_f) {
  std::string seq;
  seq += use_installer ? installer_code_ : code_;
  seq += "PP";
  if (advanced) seq += "p";
  seq += body;
  if (exit_f) seq += "F";
  bus_.send_keys(seq);
}

void X28Alarm::set_entry_delay_service(int seconds) {
  char buf[8];
  snprintf(buf, sizeof(buf), "P881%02d", seconds);
  send_programmed_sequence(buf, true, true);
}

void X28Alarm::change_owner_code_service(const std::string &new_code) {
  std::string seq = installer_code_ + "PP" + new_code;  // uses installer code per official procedure
  bus_.send_keys(seq);
  code_ = new_code;  // update in-memory copy
}
```

Services are registered in `setup()` via `register_service` with typed
parameters, making them callable directly from Home Assistant automations
and scripts.

### 7.10 Learn Mode for Unknown Zone Codes

Since zones 5–8 have predicted (but unconfirmed) packet codes, the component
provides a **learn mode** to capture actual codes from the bus.

**How it works:**
1. User physically triggers the real zone sensor (e.g., open the door on zone 5).
2. The sniffer captures the packet.
3. User copies the code from the text sensor or log into the YAML config as
   an override.

**Configuration override:**
```yaml
x28_alarm:
  zone_codes:
    5: 0x1655      # override zone 5 packet code (replaces both MPXH and wired matching)
```

This allows the component to work with any panel firmware that may use
slightly different codes than the reference.

## 8. C++ Class Design

### 8.1 `MPXPacket` — Packet Decoding

```cpp
class MPXPacket {
public:
    explicit MPXPacket(uint16_t word);
    
    uint16_t get_word() const;
    uint16_t get_parity() const;    // bit 15
    uint16_t get_id() const;        // bits 14-12
    uint16_t get_data() const;      // bits 11-4
    uint16_t get_checksum() const;  // bits 3-0
    bool is_valid() const;          // even parity check
    
private:
    uint16_t word_;
};
```

### 8.2 `CircularBuffer` — ISR-Safe Packet Storage

```cpp
class CircularBuffer {
public:
    CircularBuffer();
    
    void push(uint16_t word);   // ISR-safe (called from interrupt)
    uint16_t read();            // called from loop()
    bool is_empty() const;
    bool is_full() const;
    
private:
    uint16_t buffer_[64];
    volatile uint8_t write_index_ = 0;
    volatile uint8_t read_index_ = 0;
};
```

**Thread safety:**
- `push()` is called from the ISR context; `read()` from the main `loop()`.
- The buffer is single-producer, single-consumer. The only shared state is
  `write_index_` and `read_index_`.
- Both are declared `volatile` to prevent compiler optimization of the
  ISR-visible variables.
- No mutex is needed because the ISR cannot be preempted by the main loop,
  and the main loop reads only when the ISR is not running (interrupts
  enabled). The ISR writes only; the main loop reads only. The critical
  section is the `isEmpty()` check in `loop()`, which reads both indices:
  if write_index_ changes between the read of write_index_ and read_index_,
  the result may be stale but never unsafe — the worst case is one extra
  iteration of `loop()` without processing a newly arrived packet.

### 8.3 `MPXBus` — Low-Level Bus I/O

```cpp
class MPXBus {
 public:
  void setup(InternalGPIOPin *rx_pin, InternalGPIOPin *tx_pin,
             bool invert_rx, bool invert_tx);
  void loop();
  void send_packet(uint16_t payload);
  void send_key(uint8_t key_index);
  void send_keys(const std::string &keys);
  void send_keys(const char *keys, size_t len);

  void set_event_callback(std::function<void(uint16_t)> callback) {
    event_callback_ = callback;
  }

  bool is_keyboard_code(uint16_t code);

 protected:
  static void interrupt_handler();

  InternalGPIOPin *rx_pin_{nullptr};
  InternalGPIOPin *tx_pin_{nullptr};
  bool invert_rx_{true};
  bool invert_tx_{true};
  uint8_t rx_pin_num_{0};
  uint8_t tx_pin_num_{0};
  bool rx_idle_level_{false};

  CircularBuffer buffer_;
  uint16_t recbuf_{0};
  uint8_t bit_number_{0};
  uint32_t prev_micros_{0};

  std::function<void(uint16_t)> event_callback_;

  static MPXBus *instance_;
};
```

### 8.4 `X28Alarm` — ESPHome Component

```cpp
class X28Alarm : public Component {
 public:
  // ── Configuration ──
  void set_rx_pin(InternalGPIOPin *pin) { rx_pin_ = pin; }
  void set_tx_pin(InternalGPIOPin *pin) { tx_pin_ = pin; }
  void set_code(const std::string &code) { code_ = code; }
  void set_installer_code(const std::string &code) { installer_code_ = code; }
  void set_model(X28Model model) { model_ = model; }
  void set_invert_rx(bool inv) { invert_rx_ = inv; }
  void set_invert_tx(bool inv) { invert_tx_ = inv; }
  void set_debug(bool debug) { debug_ = debug; }
  void set_sniffing_enabled(bool en);
  void set_sniffing_throttle(uint32_t ms) { sniffing_throttle_ms_ = ms; }
  void set_zone_debounce_ms(uint32_t ms) { zone_debounce_ms_ = ms; }
  void set_zone_code_override(uint8_t zone, uint16_t code);

  void add_virtual_zone(uint8_t zone, uint16_t packet_code,
                         bool clear_on_close, bool trigger_on_open,
                         binary_sensor::BinarySensor *sensor);

  // ── Entity Registration ──
  void set_alarm_control_panel(alarm_control_panel::AlarmControlPanel *acp) { acp_ = acp; }
  void set_estoy_sensor(binary_sensor::BinarySensor *s) { estoy_sensor_ = s; }
  void set_zone_sensor(uint8_t zone, binary_sensor::BinarySensor *s);
  void set_sniffer_text_sensor(text_sensor::TextSensor *s) { sniffer_text_ = s; }
  void set_sniffer_switch(X28SnifferSwitch *s) { sniffer_switch_ = s; }

  // ── Services ──
  void send_keys_service(const std::string &keys);
  void send_packet(uint16_t code) { bus_.send_packet(code); }
  void enter_programming_service();
  void enter_advanced_service();
  void exit_programming_service();
  void set_entry_delay_service(int seconds);
  void set_exit_delay_service(int seconds);
  void set_siren_duration_service(int minutes);
  void set_siren_b_duration_service(int minutes);
  void set_sabotage_inhibit_service(bool enabled);
  void set_ac_frequency_service(int hz);
  void set_entry_annunciator_service(bool enabled);
  void set_annunciator_gap_service(int seconds);
  void set_battery_save_service(bool enabled);
  void set_owner_code_condition_service(bool disarm_only);
  void set_zone_conditionality_service(bool enabled);
  void change_owner_code_service(const std::string &new_code);
  void change_installer_code_service(const std::string &new_code);
  void program_user_service(int user, const std::string &code, int permissions, bool can_disarm);
  void rf_learn_mode_service();
  void rf_learn_slot_service(int slot);
  void rf_delete_slot_service(int slot);
  void exit_rf_learning_service();
  void toggle_zone_in_mode_service(int zone);
  void save_estoy_config_service();
  void save_mevoy_config_service();
  void set_zone_type_service(int zone, const std::string &type);
  void set_panic_zone_service();
  void set_tamper_zone_service();
  void set_clock_source_service(bool crystal);
  void set_wired_zones_service(bool enabled);
  void set_partition_merge_service(bool enabled);
  void set_pgm_output_service(int output, int option, int partition);

  // ── HA Alarm Control Panel Actions ──
  void arm_away();
  void arm_home();
  void disarm();

  // ── Event Handlers ──
  void on_event(uint16_t word);
  void on_packet(uint16_t word);
  void on_zone(uint8_t zone, bool triggered);

 protected:
  struct VirtualZoneState {
    uint8_t zone;
    uint16_t packet_code;
    bool clear_on_close;
    binary_sensor::BinarySensor *sensor;
    bool trigger_on_open;
    bool last_state;
  };

  void send_programmed_sequence(const std::string &body, bool use_installer, bool advanced, bool exit_f = true);
  void toggle_mode(uint16_t target_mode);

  MPXBus bus_;
  InternalGPIOPin *rx_pin_{nullptr};
  InternalGPIOPin *tx_pin_{nullptr};
  bool invert_rx_{true};
  bool invert_tx_{true};

  std::string code_;
  std::string installer_code_ = "467825";
  X28Model model_{X28Model::AUTO};
  ModelCapabilities model_capabilities_{get_model_capabilities(X28Model::AUTO)};
  bool debug_{false};
  bool sniffing_enabled_{false};
  uint32_t sniffing_throttle_ms_{1000};
  uint32_t zone_debounce_ms_{500};

  uint16_t last_mode_{MPX_CODE_ME_VOY};
  bool armed_confirmed_{false};
  uint32_t arm_pending_start_{0};
  bool arm_pending_{false};
  bool disarm_pending_{false};
  bool mode_waiting_{false};
  uint32_t last_sniff_log_{0};
  uint16_t last_sniff_word_{0};
  uint32_t zone_last_packet_[MAX_ZONES]{};

  alarm_control_panel::AlarmControlPanel *acp_{nullptr};
  binary_sensor::BinarySensor *estoy_sensor_{nullptr};
  binary_sensor::BinarySensor *zone_sensors_[MAX_ZONES]{};
  text_sensor::TextSensor *sniffer_text_{nullptr};
  X28SnifferSwitch *sniffer_switch_{nullptr};

  std::vector<VirtualZoneState> virtual_zones_;
  uint16_t zone_code_overrides_[MAX_ZONES + 1]{};
};
```

### 8.5 Memory Layout & Flash Usage

| Section | Size estimate | Notes |
|---------|--------------|-------|
| MPXBus code | ~2 kB | Timing-critical TX/RX in IRAM |
| X28Alarm code | ~3 kB | Event dispatching, state machine |
| Keyboard code table | 50 bytes | 25 × uint16_t |
| Packet code constants | 48 bytes | 12 × uint16_t for known event codes |
| Circular buffer | 128 bytes | 64 × uint16_t |
| X28Alarm instance | ~200 bytes | Pointers, state vars, strings |
| **Total approximate** | **~6 kB** | Well within ESP32/ESP8266 flash |

Critical functions (`_interrupt_handler`, `enable_transmit`,
`disable_transmit`) must be placed in IRAM. On ESP32, this is automatic with
`IRAM_ATTR`. On ESP8266, also use `ICACHE_RAM_ATTR`.

### 8.6 ISR Design & Performance

**Interrupt rate:**
- The MPX bus has transitions every ~1.27 ms at minimum.
- Each transition triggers a CHANGE interrupt on the RX pin.
- Maximum ISR frequency: ~787 Hz (once per BIT_TIME).
- This is well within ESP32 capability (handles MHz-range interrupts for WiFi).

**ISR execution time:**
- The ISR reads the pin state, computes a duration from `micros()`, performs a
  few integer ops, and optionally stores to the buffer.
- Expected execution: < 2 µs.
- CPU usage: ~0.16% at 787 Hz × 2 µs per call.

**WiFi impact:**
- Interrupts are disabled for ~61 ms during packet transmission.
- On ESP32, this is handled by the hardware WiFi stack and should not cause
  disconnections if WiFi is idle.
- On ESP8266, long interrupt-free periods may trigger WiFi watchdog. If this
  occurs, consider:
  - Breaking the TX into smaller chunks with brief interrupt re-enable windows.
  - Using the RCSwitch library's hardware-timed TX (from x28_sniffer).

## 9. State Machine

### 9.1 Alarm Control Panel States

```
                    ┌───────────────┐
                    │   DISARMED    │ ◄─────────────────────────────────┐
                    └───────┬───────┘                                   │
                            │ user arm_home() / arm_away()              │
                            v                                           │
                    ┌───────────────┐                                   │
                    │   PENDING     │  (waiting for panel confirmation) │
                    └───────┬───────┘                                   │
                            │                                           │
                    ┌───────┴───────┐                                   │
                    │   ALARM_ARMED packet received?                    │
                    └───────┬───────┘                                   │
                   YES      │      NO (timeout 10s)                     │
                    v       v                                            │
           ┌────────────────┐    revert to DISARMED ────────────────────┘
           │    ARMED        │
           └───────┬────────┘
                   │
           ┌───────┴────────┐
           │ Zone packet received while ARMED?                          │
           └───────┬────────┘
          YES      │      NO
           v       v
    ┌────────────┐   stay in ARMED
    │ TRIGGERED  │   (or ARMED_HOME / ARMED_AWAY)
    └──────┬─────┘
           │
           │ user sends code (disarm)
           v
    ┌────────────┐
    │ PENDING    │
    └──────┬─────┘
           │
           │ ALARM_DISARMED received?
           v
    ┌────────────┐
    │ DISARMED   │
    └────────────┘
```

### 9.2 State Transition Triggers

| Current State  | Trigger                     | Next State    | Action |
|----------------|-----------------------------|---------------|--------|
| DISARMED       | `arm_away()` service call   | PENDING       | Send MODO(s) + code. Start 10 s timeout. |
| DISARMED       | `arm_home()` service call   | PENDING       | Send MODO(s) + code. Start 10 s timeout. |
| DISARMED       | `disarm()` service call     | PENDING       | Send code. Start 10 s timeout. (No-op if already disarmed but harmless.) |
| PENDING        | ALARM_ARMED packet (mode=away) | ARMED_AWAY | Cancel timeout. |
| PENDING        | ALARM_ARMED packet (mode=stay) | ARMED_HOME | Cancel timeout. |
| PENDING        | ALARM_DISARMED packet       | DISARMED      | Cancel timeout. |
| PENDING        | Timeout (10 s)              | DISARMED      | Log error "Arm/disarm timeout". |
| ARMED_AWAY     | Zone packet received        | TRIGGERED     | Log "Alarm triggered by zone N". |
| ARMED_HOME     | Zone packet received        | TRIGGERED     | Log "Alarm triggered by zone N". |
| ARMED_AWAY     | ALARM_DISARMED packet       | DISARMED      | — |
| ARMED_HOME     | ALARM_DISARMED packet       | DISARMED      | — |
| ARMED_AWAY     | ARM_HOME service call       | PENDING       | Toggle to Estoy + send code. |
| ARMED_HOME     | ARM_AWAY service call       | PENDING       | Toggle to Me Voy + send code. |
| TRIGGERED      | `disarm()` service call     | PENDING       | Send code. Start 10 s timeout. |
| TRIGGERED      | ALARM_DISARMED packet       | DISARMED      | — |
| TRIGGERED      | Zone packet received        | TRIGGERED     | Re-trigger (stay in TRIGGERED, update log). |

### 9.3 PENDING State Details

The PENDING state bridges the gap between the user's arm/disarm command and
the panel's confirmation packet. During this state:

1. The ACP shows "PENDING" in HA (typically rendered as a yellow/amber state).
2. The component sends the key sequence to the panel.
3. It then waits for the confirmation packet (ALARM_ARMED or ALARM_DISARMED).
4. If the panel does not respond within 10 seconds (timeout), the ACP reverts
   to DISARMED and logs an error.
5. If the user issues another arm/disarm command while PENDING, it is queued
   and executed after the current operation completes or times out.

### 9.4 TRIGGERED State

The TRIGGERED state means the alarm has been tripped while armed.

**Entry conditions:**
- State is ARMED_AWAY or ARMED_HOME.
- A zone packet (Z1–Z8) is received from the bus.
- Only zone packets from the bus trigger this state, NOT virtual zone
  transmissions (which are sent by the ESP itself).

**Exit conditions:**
- Successful disarm (code sent + ALARM_DISARMED received).
- TRIGGERED does NOT auto-reset when the zone returns to normal. This matches
  real alarm panel behaviour: once triggered, the alarm stays triggered until
  disarmed, preserving the memory of the event.

**HA integration:**
When TRIGGERED is set, HA can fire automations: send push notifications,
flash lights, start recording cameras, etc.

## 10. ARM_HOME vs ARM_AWAY

### 10.1 The Problem

The X-28 panel distinguishes Estoy (Stay) and Me Voy (Away) modes. However,
the ALARM_ARMED packet (`0x49C1`) is the same regardless of which mode was
selected. The mode is signalled by a separate ESTOY (`0x4BE8`) or ME_VOY
(`0xCBAE`) packet that precedes the armed packet.

### 10.2 Mode Tracking Strategy

```
Last mode: ESTOY ──→ ALARM_ARMED received ──→ ARMED_HOME
Last mode: ME_VOY ──→ ALARM_ARMED received ──→ ARMED_AWAY
```

1. The component tracks `last_mode_` as an `MPXEvent` member variable.
2. On every ESTOY or ME_VOY packet received, update `last_mode_`.
3. On ALARM_ARMED, use `last_mode_` (default to ME_VOY if no mode packet
   has been seen) to determine ARMED_HOME vs ARMED_AWAY.

### 10.3 Mode Toggling Logic for arm_home() / arm_away()

When the user requests a specific mode, the component must ensure the panel
is in that mode before sending the disarm/arm code:

```
User calls arm_home():
  if current mode != ESTOY:
      send MODO key once
      wait up to 2 s for ESTOY packet
      if ESTOY received → proceed
      else → send another MODO (up to 10 attempts)
  send code

User calls arm_away():
  if current mode != ME_VOY:
      send MODO key once
      wait up to 2 s for ME_VOY packet (or ALARM_ARMED)
      if ME_VOY received → proceed
      if ALARM_ARMED received → panel was already armed; disarm first
      else → send another MODO (up to 10 attempts)
  send code
```

**Wait mechanism:**
- After sending MODO, poll `last_mode_` in a loop with `yield()`.
- If the expected mode is not received within 2 seconds, try again.
- This avoids race conditions where the mode toggle hasn't propagated yet.

### 10.4 Edge Cases

**Already armed, user requests arm:**
- If ACP is in ARMED_HOME and user calls `arm_away()`: send MODO to toggle
  mode, then send code again (which re-arms in the new mode).
- If ACP is in ARMED_AWAY and user calls `arm_home()`: same logic.

**Already disarmed, user requests disarm:**
- Still send code (it re-disarms or shows "already disarmed" on the keypad).
- The panel will respond with ALARM_DISARMED (or no response if already
  disarmed). Set state to DISARMED regardless.

**Panel not responding:**
- Arm/disarm timeout after 10 seconds.
- Log error and revert to DISARMED.

## 11. Security Considerations

### 11.1 Code Storage

- The alarm code is stored in plaintext in the ESPHome YAML configuration
  and compiled into the firmware binary.
- Anyone with physical access to the ESP can extract the code by reading
  flash (via serial or JTAG).
- **Recommendation:** Use ESPHome's `!secret` tag to keep the code out of
  the YAML file in version control:
  ```yaml
  x28_alarm:
    code: !secret alarm_code
  ```
- **Future enhancement:** Support hashed codes (panel-side only) if the panel
  firmware supports challenge-response. Currently not supported by X-28.

### 11.2 Bus Eavesdropping

- The MPX bus is not encrypted. Any device on the bus can see all traffic,
  including alarm codes entered via keypads.
- The ESP itself is a bus device and can read all codes transmitted by
  keypads.
- **Mitigation:** The component discards keyboard codes by default (they are
  only logged in sniffing mode). Codes are not stored or transmitted off-device
  unless sniffing is explicitly enabled.

### 11.3 HA API Security

- The `send_keys` service allows arbitrary key sequences to be sent to the
  panel. This includes the ability to change alarm codes and disable the alarm.
- This service should be protected by HA's API security (authentication,
  TLS) and should not be exposed to untrusted users.
- Consider adding a `requires_code` option to the service that prompts for
  HA API password before executing sensitive sequences.

### 11.4 Physical Security

- The ESP should be installed in a secure location (inside the alarm panel
  enclosure or nearby).
- The TX/RX connection to the bus should be protected (series resistor,
  fuse) to prevent a short from disabling the bus.
- OTA updates should be password-protected.

### 11.5 OTA Safety

During an OTA firmware update, the ESP will reboot. During the reboot:
1. GPIO pins float briefly (may cause spurious bus activity).
2. The MPX bus is uncontrolled for ~2–3 seconds.
3. The alarm panel may interpret this as tamper/sabotage.

**Mitigation:**
- Use a pull-up resistor on the TX pin to keep it in high-impedance during
  ESP reset.
- Optionally, automatically enable sabotage inhibit (P 774 1) via the panel
  before OTA, then disable after reboot. This should be configurable as a
  lifecycle hook:
  ```yaml
  x28_alarm:
    ota_safety: true   # send sabotage inhibit before reboot, restore after
  ```

## 12. Bus Diagnostics & Error Handling

### 12.1 Diagnostics Sensors

When `debug: true` is set, the component tracks bus health metrics:

| Metric | Implementation | Unit |
|--------|----------------|------|
| `bus.packets_total` | Rolling counter of valid packets received | count |
| `bus.packets_tx` | Rolling counter of packets transmitted | count |
| `bus.parity_errors` | Rolling counter of packets with invalid parity | count |
| `bus.last_packet_time` | Timestamp of last valid packet | ms |
| `bus.idle_percent` | Percentage of time bus is idle (estimated) | % |

These are exposed as `sensor` entities if `diagnostics: true` is set in config:
```yaml
x28_alarm:
  diagnostics: true
```

### 12.2 Error States

| Error | Cause | Recovery |
|-------|-------|----------|
| `ERR_RX_STUCK` | RX pin not toggling for > 60 s | Log warning. May indicate disconnected bus or circuit fault. |
| `ERR_TX_TIMEOUT` | Bus never idle for CTS_TIME within 5 s | Log error. Retry TX later. |
| `ERR_ARM_TIMEOUT` | No ALARM_ARMED received within 10 s of arm command | Log error. Revert to DISARMED. |
| `ERR_DISARM_TIMEOUT` | No ALARM_DISARMED received within 10 s of disarm command | Log error. Stay in PENDING. |
| `ERR_BUS_COLLISION` | TX interrupted by bus activity | Retry TX after CTS wait. |

### 12.3 Watchdog

The component does NOT use the hardware watchdog (to avoid unexpected
reboots during long transmissions). Instead, it implements a software
watchdog that monitors the main loop execution time. If `loop()` takes
longer than 500 ms to execute, a warning is logged.

## 13. Testing Strategy

### 13.1 Unit Tests (ESP32 test framework)

| Test | What it verifies |
|------|------------------|
| `test_packet_parity` | Known good packets pass `isValid()`, bad ones fail |
| `test_packet_fields` | `getId()`, `getData()`, `getChecksum()` return correct fields |
| `test_bit_decoding` | Simulated pin transitions decode to correct bits |
| `test_circular_buffer` | Push/read/isEmpty/isFull boundary conditions |
| `test_keyboard_codes` | Every key maps to the correct packet code |
| `test_send_key` | `send_key()` calls `send_packet()` with correct code |
| `test_known_events` | `0x49C1` → ALARM_ARMED, `0xC92B` → ALARM_DISARMED, etc. |
| `test_estoy_tracking` | Mode toggles correctly on ESTOY/ME_VOY packets |

### 13.2 Integration Tests

| Test | Setup | Expected result |
|------|-------|-----------------|
| Arm away | Connect ESP to bus, send `arm_away()` | Panel shows armed in away mode |
| Arm home | Connect ESP to bus, send `arm_home()` | Panel shows armed in stay mode |
| Disarm | Send code | Panel shows disarmed |
| Zone detect | Trigger real zone sensor | Binary sensor goes ON, then OFF after debounce |
| Sniffing | Enable sniffing, generate traffic | All packets logged |
| Virtual zone | Trigger HA sensor | Corresponding zone packet appears on bus (verified with second sniffer) |
| Send keys | Send programming sequence | Panel enters programming mode |

### 13.3 Hardware Test Procedure

1. Build the interface circuit (level shifter + driver).
2. Connect to the MPX bus (any available terminal — a free zone, a keypad
   tap point, or directly at the central panel AUX MPXH terminals).
3. Flash the ESP with a test YAML that enables debug + sniffing.
4. Open serial monitor at 115200 baud.
5. Observe packets flowing when:
   - Keypad keys are pressed.
   - The alarm is armed/disarmed via keypad.
   - Zone sensors trigger.
6. Send test key sequences via serial command or HA service.
7. Verify the panel responds as expected.

## 14. Dependencies

- **ESPHome** 2024.6+ (for `alarm_control_panel` platform support).
- **No external Arduino libraries.** The MPX protocol is implemented from
  scratch. This avoids library conflicts and ensures full control over
  timing-critical ISR code.

**Platform support:**

| Platform | Status | Notes |
|----------|--------|-------|
| ESP32    | Primary target | All GPIOs support interrupts. Hardware WiFi avoids TX-induced disconnects. |
| ESP32-S2/S3 | Should work | Same interrupt model. Verify GPIO mapping. |
| ESP32-C3 | Should work | Same as S2. |
| ESP8266  | Supported | Limited interrupt-capable pins. May experience WiFi drops during TX (61 ms interrupt-free window). |

## 15. Future Enhancements

### 15.1 High Priority

- **Alarm TRIGGERED detection** — Set ACP to TRIGGERED when a zone packet
  arrives while ARMED (away or home). Clear on DISARMED.
- **Zone bypass/exclusion** — HA service to send `Z<NN>` toggle sequence
  for a given zone, controlling whether it's included/excluded in the
  current mode.
- **Learn mode for unknown zone codes** — When a zone packet is received that
  doesn't match any known code, offer it as a new zone mapping via HA event or
  config (override via `zone_codes`).
- **Entry/exit number entities** — `Number` entities in HA for entry delay
  (P 881) and exit delay (P 882), with write-back via `send_keys`.
- **Model auto-detection refinement** — Improve AUTO mode detection by
  comparing observed zone packets against model-specific factory defaults.
  Expose detected model as a text_sensor diagnostic.

### 15.2 Medium Priority

- **Partition support** — The N8F-MPXH supports up to 8 partitions (virtual
  independent alarm systems). Add `partition` config option and instantiate
  one ACP per partition.
- **User code management services** — HA services wrapping the `F2633`
  sequence: `add_user(code, permissions)`, `remove_user(number)`,
  `change_code(number, new_code)`.
- **Wireless sensor management** — HA services wrapping `F2337` for
  learning/deleting RF transmitters and wireless sensors.
- **Siren test button** — Momentary button that triggers siren B via the
  panel for testing.
- **Model-specific zone presets** — When `model` is set (not AUTO), auto-create
  binary sensors for all factory-default zones (see §3.3) instead of requiring
  manual definition.
- **N32-MPXH partition hermanar (P 888)** — Service to merge/unmerge partitions
  on N32-MPXH, exposed as a button or service.
- **Panel status sensors** — Read and expose panel status: AC power, battery
  voltage, tamper state, GSM signal strength (if communicator installed).
- **MQTT discovery** — If the user runs ESPHome without HA, provide MQTT
  discovery topics for manual integration.

### 15.3 Low Priority

- **GPIO binary sensors** — Optional sensors connected directly to ESP GPIOs
  (not on MPX bus) for siren output, tamper, AC fail, battery low.
- **Alarm memory text_sensor** — Track which zone triggered the last alarm,
  expose as a `text_sensor` that persists across reboots (stored in RTC
  memory or preferences).
- **Keypad feedback** — Some X-28 keypads echo the pressed key as a packet.
  Capture and expose as a `text_sensor` showing the last key pressed.
- **Bus diagnostics sensors** — Packet rate, CRC errors, noise level,
  bus utilization percentage.
- **RCSwitch decoder option** — Alternative receive path using the RCSwitch
  library (from `x28_sniffer`) for platforms where custom ISR timing is
  unreliable. Configurable via `decoder: rcswitch` or `decoder: native`.
- **OTA programming mode safety** — Auto-inhibit sabotage (P 774 1) before
  OTA reboot and restore after. Configurable via `ota_safety: true`.
- **Home Assistant energy dashboard** — If the ESP has an energy monitoring
  sensor (e.g., HLW8012), the ACP state can be used to trigger energy-saving
  automations when the alarm is armed in away mode.

### 15.4 Cross-Reference: Sniffing Features

All three reference repositories contribute to the sniffing capability:

| Feature | Source | Implementation |
|---------|--------|----------------|
| Raw packet dump to serial | `x28_sniffer`, `esphome-x28` | `ESP_LOGV` level debug logging |
| Packet field parsing | `x28_sniffer` | `MPXPacket` union/struct |
| Circular buffer storage | `x28-mpx-controller` | `CircularBuffer` class |
| ISR-based receive | All three | `IRAM_ATTR` interrupt handler |
| Keyboard code identification | `x28-mpx-controller`, `esphome-x28` | `KEYBOARD_CODES` lookup table |
| Packet throttle/logging | New | `sniffing.throttle_ms` configuration |
| Unknown code capture | New | Log + text_sensor output for manual learning |
| Zone code learning | New | Override zone codes via `zone_codes` config |

### 15.5 Cross-Reference: Virtual Zone Features

Virtual zone injection is entirely new (not present in any reference repo),
but builds on transmission primitives from `x28-mpx-controller`:

| Feature | Source | Implementation |
|---------|--------|----------------|
| Packet transmission | `x28-mpx-controller` | `send_packet()` |
| Zone code database | `x28-mpx-controller` | Event constants in §5.1 |
| HA sensor polling | New | `id(sensor_id)` in `loop()` |
| Packet injection on state change | New | `on_zone()` callback → `send_packet()` |
| Clear-on-close | New | Second packet when sensor resets |

## 16. Appendix: AH (Application Home) Integration

### 16.1 Entities Exposed to Home Assistant

| Domain | Entity | Purpose |
|--------|--------|---------|
| `alarm_control_panel` | `alarm_control_panel.x28_alarm` | Arm/disarm/trigger states |
| `binary_sensor` | `binary_sensor.x28_estoy` | Stay mode indicator |
| `binary_sensor` | `binary_sensor.x28_zone_N` | Zone trigger state (N=1..32, model-dependent) |
| `button` | `button.x28_panic` | Panic alarm |
| `button` | `button.x28_fire` | Fire alarm |
| `text_sensor` | `text_sensor.x28_sniffer` | Latest bus packet (when sniffing enabled) |
| `switch` | `switch.x28_sniffer` | Toggle sniffing on/off at runtime |
| `sensor` (future) | `sensor.x28_bus_*` | Diagnostics (when diagnostics enabled) |
| `number` (future) | `number.x28_entry_delay` | Entry delay seconds |
| `number` (future) | `number.x28_exit_delay` | Exit delay seconds |

### 16.2 Recommended Automations

**Notify on alarm trigger:**
```yaml
automation:
  - alias: "Alarm triggered notification"
    trigger:
      - platform: state
        entity_id: alarm_control_panel.x28_alarm
        to: "triggered"
    action:
      - service: notify.mobile_app
        data:
          title: "ALARM TRIGGERED"
          message: "The X-28 alarm has been triggered!"
```

**Arm away when everyone leaves:**
```yaml
automation:
  - alias: "Arm away when house empty"
    trigger:
      - platform: state
        entity_id: binary_sensor.all_clear  # from person tracking
        to: "on"
    action:
      - service: alarm_control_panel.alarm_arm_away
        target:
          entity_id: alarm_control_panel.x28_alarm
```

**Flash lights on panic:**
```yaml
automation:
  - alias: "Panic button flash lights"
    trigger:
      - platform: state
        entity_id: button.x28_panic
        to: "pressed"
    action:
      - service: light.turn_on
        target:
          area_id: living_room
        data:
          flash: long
```

### 16.3 Lovelace Dashboard Card

```yaml
type: alarm-panel
entity: alarm_control_panel.x28_alarm
states:
  - arm_home
  - arm_away
name: X-28 Alarm
```

### 16.4 Example ESPHome YAML (Complete)

```yaml
esphome:
  name: x28-alarm-gateway
  platformio_options:
    board_build.flash_mode: dio

esp32:
  board: nodemcu-32s

external_components:
  - source:
      type: local
      path: components

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  fast_connect: true

api:
  encryption:
    key: !secret api_key

logger:
  level: DEBUG

ota:
  password: !secret ota_password

x28_alarm:
  id: x28
  rx_pin: 22
  tx_pin: 23
  model: N8F-MPXH
  debug: true
  sniffing:
    enabled: true
    throttle_ms: 2000
  zone_debounce_ms: 300
  virtual_zones:
    - zone: 5
      sensor_id: binary_sensor.back_door
      trigger: ON
      zone_type: MPXH
      clear_on_close: true

alarm_control_panel:
  - platform: x28_alarm
    name: "X-28 Home Alarm"
    id: alarm_panel

binary_sensor:
  - platform: x28_alarm
    name: "Estoy Mode"
    zone: ESTOY
  - platform: x28_alarm
    name: "Zone 1"
    zone: 1
  - platform: x28_alarm
    name: "Zone 2"
    zone: 2
  - platform: x28_alarm
    name: "Zone 3"
    zone: 3
  - platform: x28_alarm
    name: "Zone 4"
    zone: 4

button:
  - platform: x28_alarm
    name: "Panic"
    action: PANIC
  - platform: x28_alarm
    name: "Fire"
    action: FIRE

text_sensor:
  - platform: x28_alarm
    name: "Bus Sniffer"

switch:
  - platform: x28_alarm
    name: "Sniffer Toggle"
```

## 17. Appendix: Glossary (Extended)

| Term | Definition |
|------|------------|
| MPX | Original 12 V bus protocol |
| MPXH | Enhanced 8 V bus variant (backward compatible) |
| Central | The main X-28 alarm panel (e.g., N8F-MPXH, 9004W-MPX) |
| Zone | A monitored input (sensor) on the bus |
| Partition | Virtual sub-system; the N8F-MPXH supports up to 8 |
| Estoy | "I'm staying" — Stay mode (interior zones excluded) |
| Me Voy | "I'm leaving" — Away mode (all zones active) |
| MODO | Panel key that toggles Estoy ↔ Me Voy |
| P-key | Programming key, used twice to enter basic programming |
| Long press | Holding a key (P, F, Panic, Fire) for ~1 second |
| P-code | Advanced programming function identified by a 3-digit number |
| CTS | Clear To Send — minimum bus idle before transmission |
| ISR | Interrupt Service Routine |
| IRAM | Instruction RAM (fast memory for interrupt handlers) |
| ACP | Alarm Control Panel (ESPHome/HA entity) |
| Popcount | Number of set (1) bits in a binary value; used for parity computation |
| Debounce | Time window during which a zone sensor state is held after the last packet |
| Sniffing | Passive monitoring of all bus packets |
| Virtual zone | An HA binary sensor bridged into the MPX bus as a native alarm zone |
| Model | The X-28 hardware variant (e.g., N4-MPXH, N8F-MPXH, N32-MPXH) that determines zone capacity and feature availability |
| AUTO | Default model setting; the component auto-detects the panel type by monitoring bus traffic |
