# Yaesu TUN/LIN tuner protocol and Icom AH-4 interface: implementer's reference

This document describes, in enough detail to implement, the interface
between a Yaesu FT-891 and its external automatic antenna tuner (FC-40 /
FC-50 protocol on the TUN/LIN jack), the control interface of the Icom AH-4
tuner, and how to bridge the two. Everything marked **measured** was observed
on an FT-891 (firmware as of 2026) with a Teensy 2.0 standing in for the
tuner, and on a real AH-4, using logic-level logging at 1 ms resolution and
a 10 ms power sampler. Items marked **reported** come from other people's
observations (Albin's 2019 FC-30 captures, K1QLN's 2026 FC-40 work, K9EQ's
AH-4 notes, SimonAnnetts' FT-847 sequencer). The detailed lab log with the
raw experiments is `design-notes.md`.

Conventions: bytes in hex. "Low" means near 0 V. Times are from the end of
the triggering event unless stated. Timing figures are typically good to
±10 ms.

---

## Part 1: the Yaesu side (FT-891 TUN/LIN jack)

### 1.1 Connector and electrical

8-pin mini-DIN on the radio, numbered as in the FT-891 Advance Manual
page 111 (looking into the radio's jack). A tuner cable is straight through.

| Pin | Name | Direction | Electrical (measured unless noted) |
|---|---|---|---|
| 1 | +13.5 V | out | battery voltage, reported unfused. Powers the FC-40. |
| 2 | TX GND | out | low while the radio is in transmit, otherwise high. Open collector style; the FC-50 pulls it up with 47 k to +5 V. |
| 3 | GND | | |
| 4 | TX D | out | serial, radio to tuner, 5 V TTL, idle high |
| 5 | RX D | in | serial, tuner to radio, 5 V TTL, idle high |
| 6 | SENSE | in | tuner strap to GND. The radio samples it at power-up to decide a tuner is present. |
| 7 | RESET | out | low when the radio is off and while no tuner is registered; high once the radio has registered a tuner at power-up. Weak drive: an input with a 10 k series resistor and an AVR pull-up read it correctly. |
| 8 | TX INH | in | **active high**. Idles at 0 V, held down inside the radio. Driving it to 5 V stops the RF output while the radio stays in transmit (TX GND stays low, CAT reports TX). RF stops within 10 ms and returns within 10 ms of release, at full level, no overshoot. Pulling it low does nothing. Works identically in EXTERNAL and LAMP tuner-select modes. |

Inside the FC-50 (reference for what Yaesu considers safe): every line has a
100 µH choke, 270 Ω in series and 1 nF to ground; RESET, TX GND and TX D
have 47 k pull-ups to +5 V; TX INH is driven through a Schottky diode so the
tuner only drives it high. Implement the same: 270 Ω in series with your
serial TX, a 10 k in series with your RESET input.

Serial: 4800 baud, 8 data bits, no parity, 1 stop bit, 5 V TTL. One byte
takes 2.08 ms.

### 1.2 Registration (power-up handshake)

Radio menu 16-15 TUNER SELECT must be EXTERNAL. The radio only looks at
SENSE at power-up. Sequence, measured:

1. Radio powers up with SENSE grounded. RESET goes high.
2. The real tuner (FC-30, reported) sends `A0` about 200 ms after power and
   `A1` about 800 ms after that, unprompted. The adapter replicates this
   from the RESET rising edge: `A0` at +200 ms, `A1` at +1000 ms.
3. 14 ms after that `A1`, the radio sends three commands 100 ms apart:
   `FF E0 01 00`, `FF E0 00 01`, `FF F1 00 00` (or `FF F0 <freq>` if the
   tuner function was on when the radio was switched off). It does not
   wait for replies between them.

Consequences:

- Until this has happened, the CAT command `AC001;` (tuner on) is refused
  with `?;` and RESET stays low.
- If SENSE floats even briefly while the radio is on (for example a
  microcontroller pin that goes high-impedance during a reset), the radio
  un-registers the tuner: RESET drops, `AC002;` is refused, and only a
  radio power cycle brings it back. **SENSE must be a hard wire to ground.**
- The radio does not re-check SENSE while running, so a tuner can be
  unplugged without the radio noticing.

### 1.3 Framing

Every command from the radio is four bytes:

```
FF   <gap ~50 ms>   <op>  <hi>  <lo>
```

Measured: the gap between `FF` and `<op>` is 49.5 to 50.1 ms; the three
command bytes follow back to back (2.2 to 2.3 ms apart). A real FC-40 is
reported to respond sluggishly or not at all if the gap is missing; the
adapter simply treats `FF` as a resync marker and collects the next three
bytes.

Frequency is two BCD bytes giving MHz and 10 kHz digits, truncated:
14.02894 MHz → `14 02`, 7.1 MHz → `07 10`, 1.85 MHz → `01 85`, 50.1 MHz →
`50 10`.

### 1.4 Commands from the radio

| Bytes | Name | When the radio sends it (measured) |
|---|---|---|
| `FF E0 01 00` then `FF E0 00 01` | init | power-up only, after the tuner's boot `A1` |
| `FF F0 hh ll` | enable / frequency | tuner function switched on (`AC001;` or front panel); every time the 10 kHz value of the VFO changes while the tuner is on (within ~130 ms); after a mode change; at power-up if the tuner was on |
| `FF F1 00 00` | bypass | tuner function switched off; **first step of every tune**; when a tune is cancelled; at power-up if the tuner was off |
| `FF F2 hh ll` | tune | second step of a tune, with the carrier already on |

Unknown opcodes have not been seen.

### 1.5 Replies from the tuner

Single bytes, no framing:

| Byte | Meaning |
|---|---|
| `A0` | acknowledge. After `F2` it must arrive within about 50 ms (measured: 40 ms accepted, 100 ms too late) or the radio drops the carrier, though it still accepts a later `A1`. After `F1`, `E0`, `F0` it is not required at all (an `A1` alone works). |
| `A1` | done. After `F1`: accepted up to 2.5 s, at 3 s the radio has already cancelled the tune and switched the tuner function off. After `F2`: the radio waits at least 15 s (longest tested), holding the carrier. |
| `A2` | sent 20 ms after `A1`: tune failed. Reported from a real FC-40. The radio's observable behaviour over CAT is identical to success (it unkeys 100 ms after `A1`, tuner stays on); the radio presumably shows Hi-SWR and does not store the result. |
| `B0` | sent after `A0` and before `A1`: no or insufficient carrier. Reported from a real FC-40. Same observable behaviour as `A2` over CAT. |
| `A4`, `A8`, `E4` | reported on a noisy line, meaning unknown |

The radio measures SWR itself during the last part of the carrier, after
`A1`; it does not rely on the tuner's verdict for its own SWR display.

### 1.6 Tune sequence and timing

Triggered by the TUNE function on the front panel or by CAT `AC002;`. All
times measured, referenced to the end of the `F1` command.

```
F1 complete .............................. 0
  tuner: A0 (optional), A1 ............... A1 at T1 (adapter uses 100 ms; max ~2.5 s)
radio keys (TX GND low, carrier on) ...... T1 + 50 ms
radio: FF F2 <freq> ...................... key-on + 440..500 ms
  tuner: A0 ............................... within 50 ms of F2
  tuner hunts ............................. carrier held by radio, >= 15 s allowed
  tuner: A1 (+ A2) ........................ when done
radio unkeys ............................. A1 + 100..110 ms
```

The carrier is continuous from key-on until unkey. There is no separate
test carrier afterwards. Carrier power: see 1.8.

**The raw-SWR pre-check.** Between key-on and `F2` (the 440 to 500 ms
window) the radio looks at its own SWR bridge with the tuner supposedly in
bypass. If the SWR is above its limit it unkeys at about key-on + 455 ms,
never sends `F2`, and switches the tuner function off (`AC;` reads `AC000`).
Measured on a short wire at 1.85 MHz (SWR meter at full scale). All HF
bands of the same wire passed, including ones with an estimated raw SWR
around 7:1 to 8:1, so the limit is somewhere between that and full scale.
This check is the main limitation of the interface: a tuner that could
match the antenna never gets to try unless it is already presenting a good
match during the window. See Part 3 for the workaround.

**Cancel.** Pressing the tuner button or sending `AC000;` mid-tune: the
radio unkeys within 70 ms and sends `FF F1 00 00`. Nothing else. Tuner
function ends up off.

**No reply at all.** After `F1` with no `A1` the radio never keys. It gives
up silently somewhere under 20 s; a late `A0 A1` after 21 s was ignored.
`AC;` still read `AC001` in that state.

**CAT during a tune.** `AC;` returns `AC002`, `TX;` returns `TX2`. The
radio does not answer other CAT commands reliably while tuning; meter reads
(`RM6;`) come back `000` or not at all until the tune ends.

### 1.7 TX INH

Measured with a 5 W FM carrier and a directional coupler, in both
EXTERNAL and LAMP modes:

| Line driven | Radio in TX? | RF output |
|---|---|---|
| released (0 V) | yes | normal |
| low | yes | normal (no change) |
| high (5 V) | yes (TX GND low, `TX;` = TX1) | zero within one 10 ms sample |
| high → released | yes | full power within 10 ms, no overshoot, settles ~6 % down over 100 ms |

During a tune the radio's sequence is unaffected by TX INH: raised before
`F1`, or after `F2` for 1 s or 2.5 s, the radio still keyed on schedule,
sent `F2` 0.5 s after keying, waited for `A1` and unkeyed normally. So a
tuner may gate the RF freely while the radio waits.

This is also usable as a transmit sequencer for an amplifier or SDR switch:
key on TX GND going low, do the relay work, then drop TX INH.

### 1.8 Tune carrier power

The FT-891 keeps a power setting per mode and band (menus 16-01 HF SSB,
16-02 HF AM, 16-03 HF CW, 16-04..06 the same for 50 MHz). CAT `PC;` reads
and writes the one for the current mode.

Measured at 14 MHz into a dummy load:

| Mode | setting | tune carrier |
|---|---|---|
| CW | 5 W | 5.1 W steady, 5.7 W for the first 30 ms |
| CW | 10 W | 10.2 W steady, 11.2 W peak, settled within 100 ms |
| USB | 10 W | 5.4 W (does not follow the SSB setting; uses one of the others, not separated) |

Into a mismatch the radio folds back (7.9 W at a raw SWR of several to one
with the 10 W setting). For an AH-4 (5 to 15 W window) set the CW power to
10 W and tune in CW.

Changing the power setting over CAT did not disturb a registered tuner in
CW. One un-registration was observed once after a power change in USB,
not reproduced.

### 1.9 LAMP mode (for completeness)

With menu 16-15 = LAMP the same jack carries band data for an amplifier on
pins 4 (bit 0), 5 (bit 1), 6 (bit 2) and 7 (bit 3), updated about 350 ms
after a frequency change: 160 m = 1, 80 m = 2, 40 m = 3, 30 m = 4,
20 m = 5, 17 m = 6, 15 m = 7, 12 m = 8, 10 m = 9, 6 m = 10. Pin 8 is the
same active-high TX INH. Do not switch to LAMP with a tuner emulator
connected unless pins 4 to 7 are floated: the radio drives them.

### 1.10 Things not known

- The exact raw-SWR threshold of the pre-check.
- Whether `A2` and `B0` differ on the display (Hi-SWR icon, memory
  storage). Not visible over CAT.
- The exact upper limit of the wait for `A1` after `F2` (15 s works).
- What `E0 01 00` / `E0 00 01` convey.
- Which setting the SSB-mode tune carrier follows.

### 1.11 Useful CAT commands (4800 baud default, menu 05-06)

`ID;` → `ID0650;`. `EX1615;` reads tuner select (0 off, 1 external, 2 ATAS,
3 LAMP), `EX16151;` sets it. `AC;` / `AC000;` / `AC001;` / `AC002;` tuner
state, off, on, start tune. `PC;` power. `FA;` frequency. `MD0;` mode
(3 = CW). `RM6;` SWR meter 0..255, `RM5;` power meter. `TX;` → `TX0`/
`TX1`/`TX2`. `PS0;` turns the radio off; `PS1;` does **not** turn an FT-891
back on over USB.

---

## Part 2: the Icom AH-4 side

### 2.1 Connector and electrical

4-pin Molex on the tuner (J2), control cable OPC-136:

| Pin | Colour | Name | Electrical |
|---|---|---|---|
| 1 | black | GND | |
| 2 | red | 13.8 V | ±15 %, under 1 A peak with relays pulling in, about 300 mA typical. Fuse it (1 A polyfuse is fine). |
| 3 | white | START | input. Base of a PNP ("CPU CTRL", Q4) inside the tuner. Pull to ground to assert. An Icom radio pulls it up to 13.8 V; a 5 V pull-up or an open-drain driver with no pull-up at all works. |
| 4 | green | KEY | output, open collector NPN (Q5) to ground, with a weak internal pull-up to 5 V (22 k + diode). An Icom radio adds a pull-up to 13.8 V; 10 k to 5 V is fine. Low = asserted. |

Logic is inverted on both lines: asserted = low. Any small N-MOSFET
(2N7000, BS170) or NPN can drive START. Mind the TO-92 pinout: 2N7000 is
S-G-D, BS170 is D-G-S.

The KEY wire picks up RF when the antenna is hot: hundreds of transitions
within 100 ms were logged on 17 m at 10 W. Put an RC on it (1 k series,
10 nF to ground at the input) and debounce in software (10 ms).

### 2.2 RF and tuning characteristics

- 3.5 to 54 MHz with a wire of 7 m or more (7 to 54 MHz with the AH-2b
  whip). 120 W PEP through. Matching range about 10 to 5000 Ω.
- Tuning power 5 to 15 W, nominally 10 W. The tuner measures forward power
  itself right after it asserts KEY; outside the window it aborts.
- During a tune the RF goes through a 10:1 power divider and a bridge; the
  radio sees about 1.1:1 regardless of the antenna. Only ~350 mW reaches
  the matching network while hunting. Relays are never switched with full
  RF on them.
- The tuner measures its own frequency (divider into the CPU) and keeps 45
  memories; recalling one takes about 1 s. It does not need the frequency
  from the radio.
- After a successful tune the relays stay latched and the CPU halts (no
  digital noise). The match survives key cycles; a reset pulse (2.4) or
  power loss clears it to bypass.
- The CPU runs only during a cycle. Between cycles the tuner is passive.

### 2.3 Tune cycle, measured timings

Reference: START asserted (falling edge) at 0.

| Event | Time | Notes |
|---|---|---|
| KEY asserts | 300 to 351 ms (typ. 331) | tuner has booted and switched RL1/RL2 to the bridge path |
| tuner checks power | immediately after KEY | needs 5 to 15 W **already present**. Carrier arriving at the same moment as KEY fails the check; carrier on 200 to 300 ms before KEY works. Carrier on since before START is fine. |
| radio releases START | KEY + 250 ms (about 560 ms total) | Icom timing; the AH-4 does not care much, it also worked with START released later |
| hunt | 0.3 to 3.3 s measured; 2 to 3 s typical, 15 s max per spec; ~1 s for a memory recall | |
| success | KEY releases and stays released | relays latch, CPU halts |
| failure | KEY releases, re-asserts 18 to 40 ms later, stays asserted ~200 ms, releases | the tuner ends in bypass. With no carrier the whole thing happens about 400 to 440 ms after KEY asserted. |

At KEY release on success the tuner switches RL1/RL2 from the bridge path
to the matched network. A directional coupler ahead of the tuner showed a
brief reflected burst of 100 to 200 ms around that moment; the FT-891's own
SWR check still passed, but a radio that samples SWR right at that instant
could see it.

### 2.4 Reset to bypass

A START pulse of 70 ms with no carrier resets the tuner into pass-through.
Measured: no KEY activity at all, a couple of relay clicks. Icom radios do
this on a band change. A START held longer with no carrier instead runs a
full cycle that aborts with the failure pattern (same end state, bypass,
but 700 ms of clicking).

### 2.5 Behaviour matrix

| START | carrier | result |
|---|---|---|
| 70 ms pulse | none | reset to bypass, no KEY |
| held ≥ 350 ms | none | KEY at ~330 ms, abort pattern ~400 ms later, bypass |
| held ≥ 350 ms | on ≥ 200 ms before KEY, 5..15 W | KEY, hunt, success (KEY high) or failure pattern |
| none | any | nothing; the tuner never starts on RF alone |

---

## Part 3: bridging the two (reference implementation)

### 3.1 Mapping

| Radio → | Adapter | → Radio |
|---|---|---|
| RESET rises | | `A0` +200 ms, `A1` +1000 ms |
| `E0 xx xx`, `F0 freq` | nothing | `A0`, `A1` +300 ms |
| `F1 00 00` | START low, cycle begins | `A0`, `A1` +100 ms |
| `F2 freq` | note that F2 arrived; cycle continues | `A0` immediately; result later |
| cycle finishes | | `A1` (ok), `A1 A2` (fail), `B0 … A1` (no KEY) |

This is the **F1-start** sequence. The AH-4 cycle starts on `F1`, so that
when the radio keys (150 ms after `F1`) and measures its raw SWR (until
`F2` at about 600 ms), the AH-4 has keyed (at about 345 ms) and its bridge
is in line for most of the window and at the moment the radio decides.
Measured outcome: the pre-check passes on an antenna the radio refused in
the classic sequence, the AH-4 gets to hunt, and both success and failure
report correctly.

Why `A1` at 100 ms: the carrier must be on 200 to 300 ms before the AH-4
asserts KEY (2.3). `A1` at 300 ms after `F1` put the carrier edge at the
same moment as KEY and the AH-4 aborted every time; 50 and 150 ms both
worked. The window is bounded above by the AH-4's power check and below by
how much raw-SWR time the radio tolerates before its decision; 100 ms is
in the middle of what was tested.

The **classic** sequence (what an FC-40 does, kept as an option):
`F1` → 70 ms START pulse, then `A1`. `F2` → `A0` at once, TX INH high,
START low; on KEY release TX INH (RF reaches the AH-4 only now), START
released 250 ms later; result as above. Works wherever the radio's
pre-check passes. TX INH is not needed in the F1-start sequence because
the carrier must be present early anyway.

### 3.2 State machine

```
IDLE
  F1 ─────────────────────► START low, t0 = now, F2seen = false, parked = none
                            schedule A1 at t0 + F1_A1_MS            → WAIT_KEY
  F2 (no cycle running)    (classic fallback) A0, TX INH high, START low → WAIT_KEY
  F2 (parked result)       A0, then send the parked result           → IDLE

WAIT_KEY
  KEY asserted, and KEY has been seen released since START went low
                           tKey = now, TX INH release                → KEYED
                           (a KEY still low from an aborted previous cycle must not count)
  now - entered ≥ 1500 ms  START high, TX INH release, report NOKEY  → IDLE

KEYED
  now - entered ≥ 250 ms   START high                                → HUNT

HUNT
  KEY released                                                        → SETTLE
  now - t0 ≥ 16000 ms      START low (70 ms reset pulse), report FAIL → RESET_PULSE

SETTLE
  KEY asserted again       (failure blip)                             → FAIL_WAIT
  now - entered ≥ 100 ms   report OK                                  → IDLE

FAIL_WAIT
  KEY released, or 2000 ms report FAIL                                → IDLE

RESET_PULSE
  now - entered ≥ 70 ms    START high; (classic F1: send A1)          → IDLE

any state
  F2                       A0 immediately; F2seen = true
  F1                       abort (START high, TX INH release) and start over
  host "abort"             abort

report(result):
  if F1-start mode and not F2seen: parked = result   (send after F2's A0)
  else OK → A1;  FAIL → A1, A2 after 20 ms;  NOKEY → B0, A1 after 300 ms
```

Constants: `F1_A1_MS` 100 (50..150 tested), KEY timeout 1500 ms (KEY
normally at 330), START hold after KEY 250 ms, hunt limit 16 s (spec 15),
settle 100 ms (blip comes at 18..40 ms), fail wait 2 s, reset pulse 70 ms,
`A1` after `E0`/`F0` 300 ms (anything under 2.5 s), boot `A0`/`A1` 200 /
1000 ms after RESET rises, KEY debounce 10 ms.

Hard real-time requirement: `A0` within 50 ms of the last byte of `F2`.
Everything else has at least hundreds of milliseconds of slack. A hardware
UART and a main loop that never blocks for more than a few milliseconds
are enough; the Teensy 2.0 implementation answers in under 1 ms. Send the
`A0` before doing anything that could block, such as writing a log line to
a USB host that may have stalled.

### 3.3 Edge cases

- **Tuner off (`AC000;`)** also arrives as `F1`. In F1-start mode this
  begins a cycle with no carrier; the AH-4 aborts into bypass by itself
  after about 700 ms, which is the state "tuner off" wants. Harmless.
- **Cancel** arrives as `F1` after the radio unkeyed: same as above. The
  AH-4 may still be in its own abort sequence (KEY low) when the new cycle
  begins, which is why WAIT_KEY needs the release-then-assert
  qualification. Tested: cancel 1.5 s into a hunt, retune 1 s later,
  correct result.
- **Radio refuses even with the bridge in line** (very bad antenna, or no
  AH-4 connected): the radio unkeys without `F2`; the cycle continues with
  no carrier, the AH-4 aborts, the result is parked and discarded at the
  next `F1`.
- **AH-4 finishes before `F2`** (fast memory recall): result is parked and
  sent right after `F2`'s `A0`. `F2` can also arrive before START has been
  released; that is fine.
- **No AH-4 or no power to it**: KEY never comes; after 1.5 s the adapter
  reports `B0`, `A1`. The radio unkeys. Tuner function stays on.
- **Hunt timeout**: reset pulse to force bypass, report `A1 A2`.
- **Radio power-up**: RESET rises, boot bytes, then the radio's `E0` pair
  and `F1`/`F0`. In F1-start mode that `F1` runs a no-carrier cycle too.

### 3.4 Hardware

- Microcontroller with a hardware UART at 5 V logic, or 3.3 V with level
  shifting (the radio's TX D is 5 V; its RX D input threshold is not
  characterised, use 5 V to be safe). ATmega32U4 (Teensy 2.0) used here;
  an ATtiny1614-class part or a Pi Pico with a level shifter would do.
- 270 Ω in series with the UART TX to the radio. 10 k in series with the
  RESET input, with a pull-up. TX GND input with a pull-up.
- SENSE: wire to ground. Not a GPIO.
- TX INH: GPIO driven high to inhibit, high-impedance otherwise. Optional.
- START: N-MOSFET, gate to GPIO with 100 k to ground, drain to START,
  source to ground. GPIO high = START asserted.
- KEY: GPIO input with 10 k pull-up to the MCU supply, 1 k series and
  10 nF to ground at the pin. Low = asserted.
- Power: the radio's pin 1 can feed a 5 V regulator for the adapter. Feed
  the AH-4 from its own 13.8 V or from pin 1 through a 1 A polyfuse;
  pin 1 is reported unfused in the radio.
- Ferrites on the control cables; the AH-4 cable runs near the antenna.

### 3.5 Verification checklist (with CAT into a dummy load, then an antenna)

1. Power the radio with the adapter connected, menu 16-15 = EXTERNAL. Log
   shows RESET high, the boot bytes, `E0 01 00`, `E0 00 01`, `F1 00 00`.
   `AC001;` is accepted and produces `F0 <freq>`.
2. QSY by 10 kHz: one `F0`. QSY by 5 kHz within the same 10 kHz: nothing.
3. `AC002;` on a dummy load through the AH-4: `F1`, `A1`, TX GND low, KEY
   within ~350 ms of START, `F2` about 600 ms after `F1`, `A0` within
   50 ms, KEY releases, `A1`, TX GND high 100 ms later, `AC;` = `AC001`.
4. Same with the AH-4 unpowered: `B0`, `A1` after 1.5 s, radio unkeys.
5. Antenna the AH-4 cannot match: failure blip, `A1 A2`, radio unkeys, AH-4
   in bypass.
6. Antenna the radio would refuse raw (e.g. a short wire on 160 m): with
   F1-start the radio must send `F2`; with the classic sequence it unkeys
   after ~455 ms and sends nothing.
7. Cancel mid-tune with `AC000;`: radio unkeys, sends `F1`, adapter aborts
   cleanly.
8. Flash or reset the adapter while the radio is on: RESET must stay high
   (proves SENSE is hard-wired).

---

## Sources

- Albin Stigö, "Reverse engineering the Yaesu FC-30 antenna tuner
  protocol", 2019, gist cc62dff65d35c857c3a2bd8dfb40051f, with comments by
  WA2FZW and K1QLN (2026).
- John K1QLN, wiki.millerjs.org/yaesufc-40control (FC-40 standalone
  control, timing, response codes).
- K9EQ, "AH-4 Design and Operation" (hamoperator.com): AH-4 interface
  timing diagrams and electrical description.
- Simon Annetts, yaesu_icom_atu_seq_v2 (GitHub): FT-847 to AH-4-type
  sequencer, carrier-before-KEY finding.
- Icom AH-4 instruction and service manuals; Yaesu FC-50 technical
  supplement; FT-891 Advance Manual and CAT reference.
- Measurements in this repository, `design-notes.md` sections 8 to 10,
  October 2026.
