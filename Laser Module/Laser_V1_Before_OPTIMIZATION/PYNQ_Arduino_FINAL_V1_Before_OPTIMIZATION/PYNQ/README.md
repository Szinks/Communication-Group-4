# BEFORE OPTIMIZATION
# PYNQ_Arduino_V1_Before_OPTIMIZATION - PYNQ side

Operator console and link tester for the Group 4 laser communication link,
running on a PYNQ-Z2 with libpynq. Four LCD pages walk the user from "no link"
to a live debug screen that shows what came in and what was echoed back.

Everything in this README is from the PYNQ's point of view.

## Files

| File | What it is |
|---|---|
| `PYNQCode.txt` | The working source, as it runs on the board |
| `PYNQCode_Commented.txt` | Same program, fully commented. Equivalent in code |
| `HANDOFF_PYNQ.txt` | Full handoff: setup, block-by-block walkthrough, modification guides |
| `../Arduino/ArduinoCode.txt` | The peer sketch (in the sibling Arduino folder), for reference |

On the board the source lives at
`/home/student/libpynq-5EWC0-2023-v0.2.6/applications/myleds/main.c`.
Build and run with `make` then `./main` from that folder.

## Wiring

| Signal | PYNQ pin | Dir | Peer pin |
|---|---|---|---|
| Serial data in | `AR0` | in | Arduino `D1` (Serial1 TX) |
| Inverted reply out | `A0` | out | Arduino `D2` (software-UART in) |
| Ground | `GND` | - | Arduino `GND` (mandatory) |

A common ground is required. A 10 kOhm pull-down on `AR0` is recommended so the
pin is defined when the peer is unplugged. Header pins are 3.3 V and **not**
5 V tolerant.

## The link (UART both ways, asymmetric in speed)

**In (Arduino -> PYNQ)** - hardware UART0, **9600 baud, 8N1**, normal polarity.
ASCII, comma separated, newline terminated:

```
timestamp,payload,pastdelay\n      e.g.  1748765334567,ABCDEFAB,0
```

> The baud rate is fixed at 9600 inside the FPGA bitstream. There is no C call
> to change it, which is why grepping libpynq for "baud" finds nothing. The peer
> must match exactly; anything else shows up as unprintable glyphs, instability,
> or a program stuck on "Connecting...".

**Out (PYNQ -> Arduino)** - an **inverted 8N1 UART**, bit-banged in software on
`A0`, 10 ms per bit (~100 baud). Same framing as the inbound UART, reversed in
polarity, 10 bits per character:

```
START = HIGH | 8 data bits, LSB first, each INVERTED | STOP = LOW (idle)
idle = LOW
```

Content is `timestamp,payload\n` - the delay field is deliberately not echoed.
Each byte carries its own START and STOP, so the peer re-synchronises on every
byte and the slow link does not drift. The message ends at `\n`.

## Page flow

| Page | Shows | Leaves on |
|---|---|---|
| 1 | `Hello Universe.` / `Ready for Connection? (Y)` | **Y** -> page 2 |
| 2 | `Connecting` with animated red dots | first received byte -> page 3 |
| 3 | `Ready for Communication` / `Proceed to Communication? (Y/A)` | **Y** -> page 4, **A** -> page 2 |
| 4 | Communication debug screen | **N** -> page 1 |

Buttons: **Y** = BTN3 (LD3), **A** = BTN1 (LD1), **N** = BTN2 (LD2) and is a
universal EXIT on every page. LEDs blink while an answer is awaited and go solid
while the button is held.

Page 4 layout:

```
Communication                      <- green title
        Received Data:             <- red, centred
Timestamp: ...
Payload: ...
Bits: Data Bytes | Idle
      Transmitted Data:            <- red, centred
Timestamp: ...
Payload: ...
Bits: Data Bytes | Idle
EXIT (N)   Last Delay:0000      4  <- bottom row
```

## How it works, in six lines

1. `main()` is a four-state machine; state changes come only from buttons or
   from the arrival of serial data.
2. The link is declared up the moment `uart_has_data(UART0)` is true - the
   hardware FIFO catches a 27 ms burst no matter how slowly the loop polls.
3. Bytes are accumulated into a line buffer until `'\n'`.
4. `split_fields()` splits the line on commas; the fields are printed as text.
5. `make_reply()` copies the raw line up to the second comma, a `'\n'` is
   appended, and `uart_send_inverted()` clocks it out of `A0` as inverted 8N1
   UART, one GPIO level per 10 ms.
6. Every LCD write goes through `safe_draw()`, which clips. libpynq's
   `displayDrawChar` calls `abort()` on an off-screen glyph, so this wrapper is
   the difference between a clipped string and a dead process.

## Key constants

| Constant | Value | Meaning |
|---|---|---|
| `RX_PIN` / `TX_PIN` | `IO_AR0` / `IO_A0` | data in / reply out |
| `BIT_PERIOD_MS` | `10` | outbound bit time - **the peer's decoder must match** |
| `HOLD_MS` | `4000` | how long a finished message stays frozen on screen |
| `BLINK_MS` | `600` | LED blink half-period |
| `DISPLAY_W` / `DISPLAY_H` | `240` / `240` | enforced safe drawing area |
| `FONT_W` / `FONT_H` | `8` / `16` | glyph cell, ~30 chars per row |
| `Y_FOOTER` | `222` | bottom row: EXIT, Last Delay, page number |

Baud is not a constant here - it is 9600, fixed in the bitstream.
`REQUIRED_TRANSITIONS` is dead code from an abandoned edge-counting detector.

## Timing

| Item | Duration |
|---|---|
| inbound message, 25-28 chars @ 9600 | ~26-29 ms |
| outbound reply, 23 chars x 10 bits x 10 ms | ~2.3 s |
| on-screen freeze (`HOLD_MS`) | 4 s |
| peer reply timeout | 5 s |
| peer idle before resending | 5 s |

The ~2.3 s reply must fit the peer's 5 s listen window, and the PYNQ must be back
polling (after its 4 s hold) before the peer resends. It fits, with thin margin.
If the peer times out, lengthen *its* timeout rather than shortening `HOLD_MS`.

## Known limitations

- LD2 stops blinking and the LCD freezes for ~2 s during a reply. Nothing may
  interrupt the bit timing.
- Nothing is received during the ~2.3 s reply plus the 4 s hold, so a peer that
  streams without pausing will overflow the FIFO. See section 8.3 of
  `HANDOFF_PYNQ.txt`.
- `BIT_PERIOD_MS` cannot be pushed much below a few ms: this is Linux
  userspace, and the scheduler corrupts faster waveforms. A genuinely fast
  outbound link has to live in the FPGA fabric.
- If `'\n'` never arrives the line buffer saturates at 127 bytes and the screen
  silently stops updating. A resync guard is suggested in the handoff.

## Common modifications

Full recipes with exact edits are in `HANDOFF_PYNQ.txt` section 8:

- **8.1** remove the delay field, displayed and received
- **8.2** 7-bit data bytes (outbound is now 8-bit LSB-first UART; inbound is
  fixed at 8N1 in the bitstream, so mask with `c &= 0x7F`)
- **8.3** cope with an input that has no gap between bytes or between messages
