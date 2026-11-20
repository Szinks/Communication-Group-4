# Before Optimization
# UNO R4 WiFi ↔ PYNQ UART Serial Link (inverted, slow return)

Round-trip verification and latency test for an Arduino UNO R4 WiFi talking to a PYNQ board over two separate, deliberately different serial paths.

**This README documents the Arduino side only.**

---

## What it does

Every cycle, the Arduino:

1. **Sends** `timestamp,payload,pastdelay\n` out of **D1** using the hardware UART.
2. **Listens** on **D2** for the PYNQ to echo `timestamp,payload` back, using the same UART framing, inverted and bit-banged slowly in software.
3. **Decodes** the reply to ASCII (undoing the inversion).
4. **Compares** it byte-for-byte with what it expects.
5. **Measures** the round-trip time in milliseconds.
6. **Reports** match/mismatch on the onboard LED, and carries the measured latency into the next outgoing message.

The link is asymmetric in **speed, not framing** — both directions are 8N1 UART. The Arduino transmits with its hardware UART at 9600; the PYNQ can't bit-bang that fast from Linux userspace, so its return path is the same UART framing, inverted and bit-banged ~100× slower in software and decoded in software on the Arduino.

---

## Hardware

- **Board:** Arduino UNO R4 WiFi (Renesas RA4M1)
- **Logic level:** 3.3 V on all GPIO. Pins are **not** 5 V tolerant on input.

### Pins

| Pin | Direction | Role |
|-----|-----------|------|
| D1 | output | `Serial1` TX → PYNQ UART RX |
| D0 | — | `Serial1` RX, claimed by `Serial1.begin()` but never read |
| D2 | input | software-UART RX ← PYNQ (inverted, slow) |
| D13 | output | `LED_BUILTIN`, match/mismatch status |
| USB-C | — | `Serial` debug to a PC |

> **The receive pin is D2, not D0.** `Serial1.begin()` takes ownership of D0 and D1, so D0 cannot be read with `digitalRead()`.

### Wiring

```
Arduino D1  ──────────────►  PYNQ UART RX
Arduino D2  ◄──────────────  PYNQ inverted-UART output
Arduino GND ◄────────────►  PYNQ GND        (mandatory)
```

No level shifter needed — both sides are 3.3 V. **Common ground is mandatory.**

D2 uses `INPUT_PULLDOWN` so an undriven pin reads LOW, which is the idle level for the inverted input format. If your core doesn't honour `INPUT_PULLDOWN`, fit an **external 10 kΩ resistor from D2 to GND**.

### Power (standalone)

The sketch is stored in flash and runs on power alone — no PC needed. Use **one** of:

- USB-C from a phone charger or power bank (5 V)
- regulated 5 V into the `5V` pin
- 6–24 V into `VIN` (onboard regulator)

Don't feed 5 V into `VIN` (regulator dropout). Don't connect two supplies at once.

---

## Configuration as coded

| Setting | Value |
|---------|-------|
| `Serial` (USB debug) | 115200 baud |
| `Serial1` (D1 out) | **9600** baud, 8N1 (fixed by the PYNQ bitstream) |
| D2 bit period | 10000 µs = 10 ms (~100 baud) |
| `RX_TIMEOUT_MS` | 5000 ms (per-byte START wait, reset each byte) |
| LED window | 5000 ms per cycle |
| `TIMESTAMP` | `"1748765334567"` |
| `PAYLOAD` | `"ABCDEFAB"` |

---

## Protocol

### Outbound — D1, standard UART

| Property | Value |
|----------|-------|
| Idle | HIGH |
| START | LOW, 1 bit |
| Data | 8 bits, **LSB-first** |
| Parity | none |
| STOP | HIGH, 1 bit |

Format: `timestamp,payload,pastdelay\n`

- Single comma separator, **no space**
- `print('\n')`, not `println()`, so **no `\r`**
- First cycle: `1748765334567,ABCDEFAB,0\n`
- `pastdelay` is the previous cycle's round-trip latency in ms; `0` until a reply arrives

### Inbound — D2, inverted 8N1 UART (software)

The **same framing as the outbound path**, reversed in polarity and bit-banged slowly in software. Wire levels (inverted relative to normal UART):

| Property | Value |
|----------|-------|
| Idle | **LOW** |
| START | **HIGH**, 1 bit |
| Data | 8 bits, **LSB-first**, each bit inverted |
| STOP | **LOW** (= idle), 1 bit |
| Bit period | 10 ms |

10 bit times per character (1 START + 8 data + 1 STOP) = 100 ms/char.

Format: `timestamp,payload\n` — expected content `1748765334567,ABCDEFAB` (22 chars; the `\n` ends the message and is stripped).

- **`\n`-terminated**, exactly like the outbound path (minus the delay field).
- Every byte has its own START and STOP, so the line returns to idle between bytes and the decoder **re-synchronises on each byte's START edge** — counting error cannot accumulate, so there is **no cumulative drift**.
- 23 chars × 100 ms ≈ **2.3 seconds** per reply, so `pastdelay` normally reads ~2300 ms or more. That's inherent to the slow return path, not a fault.

---

## LED status

| Pattern | Meaning |
|---------|---------|
| 4 blinks/s (125 ms half-period) | reply received and matched exactly |
| 0.5 blinks/s (1000 ms half-period) | mismatch **or** no reply at all |

The LED isn't driven while `receiveMessage()` waits, so you'll see a dark gap, then a 5 s blink burst, repeating.

---

## Serial debug

Open the Serial Monitor at **115200**. One line per cycle:

```
n=22 rx=[1748765334567,ABCDEFAB] matched=1
```

| Field | Meaning |
|-------|---------|
| `n` | characters decoded; `0` = timeout before any byte, or overflow guard |
| `rx` | the decoded string |
| `matched` | `1` only if length **and** content match exactly |

`Serial` (USB) and `Serial1` (D1) are independent peripherals, so USB debugging doesn't disturb the link.

> `receiveMessage()` always null-terminates `buf`, so an `n=0` return prints `rx=[]` rather than stale bytes.

---

## Code structure

| Function | Inputs | Output | Role |
|----------|--------|--------|------|
| `sampleLogical()` | — (reads `RX_PIN`) | `int` 0 or 1 | Reads D2 and **inverts** it. The single point where reversed polarity is undone. |
| `receiveMessage(buf, maxLen)` | destination array, capacity | `int` chars written, or `0` on timeout/overflow | Per byte: idle → START edge → 8 data bits LSB-first → STOP; ends on `\n`. Null-terminates `buf`. |
| `blinkStatus(matched, durationMs)` | rate selector, duration | — (drives `LED_PIN`) | Blocking blink; doubles as the inter-cycle gap. |
| `setup()` | — | — | Starts both serial ports, configures D2 and the LED. |
| `loop()` | — | — | send+flush → receive → verify+measure → debug → indicate. |

`pastDelay` is the only state that survives between iterations. It's written **only** inside the `if (n > 0)` branch, which is what keeps it `0` until the first real reply.

### Why `Serial1.flush()` matters

`print()` is non-blocking and returns while D1 is **still transmitting**. D1 and D2 are adjacent header pins, so without `flush()` the outgoing transmission can capacitively couple into D2 and be misread as an incoming frame. `flush()` blocks until the last bit is physically out.

---

## What both ends must agree on

**Outbound (D1 → PYNQ)**
- Baud rate exactly (9600 as coded)
- 8N1, LSB-first, standard non-inverted polarity
- Comma separator, no space
- `\n` terminator, no `\r`

**Inbound (PYNQ → D2)**
- Bit period exactly 10 ms
- Inverted polarity: idle LOW, START HIGH, data bits flipped, STOP LOW
- 8 data bits, **LSB-first**
- `\n` terminator (no CONTROL bit)
- Content exactly `timestamp,payload`, same separator

**Both** — 3.3 V logic, common ground, crossed wiring.

---

## Behaviour with nothing connected

`INPUT_PULLDOWN` holds D2 LOW = logical idle. The idle check passes immediately, no START is ever seen, the 5 s timeout expires, `receiveMessage()` returns `0`, `matched` stays false, and the LED shows the mismatch pattern. **This is correct and expected with no PYNQ attached.**

---

## Known issues

1. **No CRC or parity.** Integrity rests entirely on the exact string compare; a corrupted reply is indistinguishable from a wrong one.
2. **Message framing depends on `\n`.** If the terminating `\n` is lost, `receiveMessage()` keeps waiting for the next byte's START until the per-byte 5 s timeout expires, then returns what it has. The overflow guard also returns `0`.
3. **No automatic retransmit.** A mismatched or missing reply is just reported on the LED and USB; the next cycle retries.

*(Resolved vs the pre-UART version: the old 7-bit MSB-first CONTROL-bit frame counted ~198 bit periods from a single START and drifted; the per-byte START+STOP UART framing now resyncs every byte. Baud comments and blink-rate comments were also corrected.)*

---

## Build and upload

1. Open the `.ino` in the Arduino IDE.
2. Select **Arduino UNO R4 WiFi** as the board and the correct port.
3. Upload.
4. Unplug from the PC and power from a charger if you want standalone operation — there is no `while (!Serial)` wait, so the sketch runs without a host.

---

## Modification notes

See `HANDOFF.txt` section 7 for full detail on:

- **Removing the delay field** — drop the global, the two `print` calls, and the `pastDelay = recvTime - sendTime` assignment. (The return path already omits it.)
- **7-bit data bytes** — both paths now use 8 data bits, LSB-first. To go 7-bit, set the transmit side to `Serial1.begin(9600, SERIAL_7N1)`, narrow the D2 decoder's data loop to 7 bits, and change the PYNQ sender's loop to 7 bits to match.
- **Back-to-back bytes / messages** — fully supported. Every byte has its own START+STOP, so the line idles for one bit between bytes and the decoder resyncs on each START edge; no minimum inter-message idle is required.
