# Yaesu TUN/LIN to Icom AH-4 adapter: design notes

Status: research done, nothing built. Last updated 2026-10-10.

Goal: press TUNE on the FT-891 and have the Icom AH-4 tune, with the radio
showing tuner status as if an FC-40 were attached. The adapter sits on the
radio's TUN/LIN jack and the AH-4's 4-wire control cable. No modification of
either unit.

## 1. Radio side: Yaesu FC-40/FC-50 protocol

Sources: Albin's 2019 gist (FC-30 on FT-891), K1QLN's 2026 confirmation on an
FC-40 (gist comment and https://wiki.millerjs.org/yaesufc-40control), FC-50
technical supplement page 6.

### TUN/LIN jack, 8-pin mini-DIN

| Pin | Name | Dir (radio) | Notes |
|---|---|---|---|
| 1 | +13.5 V | out | unfused, powers the tuner (FC-40 relays run off it) |
| 2 | TX GND | out | pulled low by radio while transmitting |
| 3 | GND | | |
| 4 | TX D | out | 5 V TTL serial, radio to tuner (tuner "DATA IN") |
| 5 | RX D | in | 5 V TTL serial, tuner to radio (tuner "DATA OUT") |
| 6 | SENSE | in | tuner grounds it; radio detects tuner presence |
| 7 | RESET | out | high while radio is on |
| 8 | TX INH | in | tuner pulls low to inhibit TX |

FC-50 interface circuit (CNTL unit, J1002): every line has a 100 uH choke,
270 ohm series resistor and 1 nF to ground. DATA IN, TX GND and RESET have
47 k pull-ups to +5 V inside the tuner. TX INH is driven by an inverter
through a Schottky diode. Measured on the FT-891 the line is ACTIVE HIGH:
the radio holds it low, the tuner drives it high to inhibit RF (see
section 8). TX GND also enters through a Schottky. The cable is straight through.

The radio must have menu 16-15 TUNER SELECT = EXTERNAL, set with the radio
off and the tuner connected before power-on.

### Serial framing

- 4800 baud, 8N1, 5 V TTL, idle high.
- Radio command = `FF`, gap of about 50 ms, then `<op> <hi> <lo>`.
  The gap matters; rapid back-to-back bytes get sluggish or no response from
  a real FC-40. The adapter must accept the gap but needs no delay itself.
- Frequency = two BCD bytes at 10 kHz resolution: `07 16` = 7.16 MHz,
  `14 25` = 14.25 MHz.

### Commands from the radio

| Bytes | Meaning | Seen when |
|---|---|---|
| `FF E0 01 00` then `FF E0 00 01` | init handshake, purpose unknown | radio power-on |
| `FF F1 00 00` | tuner disable / bypass | power-on with tuner off, TUNE pressed (first step), tuner turned off |
| `FF F0 <bcd>` | tuner enable, frequency | power-on with tuner on, every 10 kHz of VFO movement while enabled |
| `FF F2 <bcd>` | start tuning | TUNE pressed (second step) |

### Replies from the tuner

Every transaction is bracketed `A0` (ack) ... `A1` (done). About 800 ms
apart for trivial commands, the full hunt time for a tune. The result of a
tune is carried by what else appears:

| Pattern | Meaning |
|---|---|
| `A0 ... A1` | success, nothing trailing. Radio then sends a test carrier and checks SWR itself |
| `A0 ... A1 A2` | tune failed (radio shows Hi-SWR, does not store) |
| `A0 B0 ... A1` | no or too little carrier, early abort |
| `A4`, `A8`, `E4` | seen on a noisy line, unconfirmed |

Power-on, tuner side: about 200 ms after power the FC-30 sent `A0`, then
`A1` 800 ms later, before the radio sent anything.

### Tune sequence as seen from the radio (gist)

1. Radio sends `F1 00 00`, tuner bypasses. Radio sends a carrier and checks
   raw SWR. If it is too high the radio does not continue.
2. Radio sends `F2 <freq>` with a (lower) carrier. K1QLN found the carrier
   must already be on when `F2` arrives. Carrier stays on until `A1`.
3. After `A1` the radio sends a test carrier and measures SWR.

"WAIT" blinks on the display while the radio waits for `A1`. The manual says
the carrier transmits continuously while tuning. The radio can cancel with a
button press (what it sends then is unknown).

### Unknowns on the radio side

- Tune carrier power. FC-40 accepts 4-60 W; the AH-4 needs 5-15 W. Does the
  FT-891 use the RF PWR setting, a fixed level, or two levels (gist step 1 vs
  2)? Measure with a wattmeter. If it is above 15 W the AH-4 aborts.
- Radio timeout while waiting for `A1`. AH-4 can take up to 15 s.
- Whether the radio resumes the carrier after TX INH is released mid-tune.
- Whether the radio needs the `A0`/`A1` on `E0`/`F0`/`F1` at all, and how
  soon. A real FC-40 answers each.
- What the radio sends on cancel and on band change.

## 2. Tuner side: Icom AH-4 START/KEY interface

Sources: AH-4 instruction manual, AH-4 service manual section 3-6 and block
diagram, K9EQ "AH-4 Design and Operation"
(https://www.hamoperator.com/HF/AH-4_Design_and_Operation.pdf), SimonAnnetts'
FT-847 sequencer (https://github.com/SimonAnnetts/yaesu_icom_atu_seq_v2).

### Cable (J2, 4-pin Molex)

| Pin | Colour | Name | Notes |
|---|---|---|---|
| 1 | black | GND | |
| 2 | red | DC 13 V | 13.8 V +-15 %, under 1 A peak, about 300 mA typical |
| 3 | white | START | radio pulls to GND to assert. Pull-up to 13.8 V is in the radio. Goes to Q4 2SA1576 "CPU CTRL" in the tuner |
| 4 | green | KEY | tuner pulls to GND to assert via Q5 2SC4081 open collector. Tuner has a weak pull-up to 5 V (22 k + diode); Icom radios add a pull-up to 13.8 V |

12 V inverted logic, open collector both ways. Pull-ups to 5 V on the
adapter side are fine for both lines.

### Tune sequence (K9EQ figure 4, confirmed by SimonAnnetts' measurements)

```
START  ----\__________/-------------------------------   low ~560 ms
KEY    ----------\_______________________/------------   low 310 ms after START
RF     ------------/---------------------\------------   10 W (5-15 W) carrier
                   ^ radio keys when KEY asserts        ^ KEY released = done
```

1. Radio asserts START (low). The AH-4 CPU is reset and boots in about 300 ms,
   switches RL1/RL2 so RF goes through the 10:1 divider and bridge, then
   asserts KEY.
2. KEY low means "send carrier". Radio transmits about 10 W. The AH-4 checks
   5-15 W, otherwise it aborts.
3. Radio releases START about 250 ms after KEY asserted (total 560 ms).
4. AH-4 hunts, 1-3 s typical, 15 s max, about 1 s when recalling one of its
   45 memories (it measures frequency itself with a divider into the CPU, so
   it needs no frequency from the radio).
5. Success: KEY goes high and stays high. Relays latch, CPU halts (no digital
   noise). The match holds across key cycles and power-off... until reset.
6. Failure: KEY goes high for 20 ms, low again for 200 ms, then high. The
   tuner goes to bypass.
7. Reset / bypass: a START pulse of 70 ms with no carrier puts the tuner in
   pass-through. Icom radios do this on band change.

Practical findings from SimonAnnetts (EDX-2/AH-4 type tuners): keying the
radio at START rather than at KEY gave reliable tunes, because a radio's
carrier overshoot (2.5-3x for ~700 ms) has settled by the time the tuner
measures power 35 ms after START is released. Keying only at KEY gave
incomplete tunes with early KEY release at ~350 ms. The AH-4 tolerates
carrier present before START: its RL1/RL2 engage when input power is
present.

The AH-4 does not start on RF alone. Without START it stays in whatever
state it latched.

## 3. Adapter behaviour (updated 2026-10-12 after the bench, see section 8)

The adapter plays "FC-40" towards the radio and "Icom radio" towards the
AH-4. Mapping:

| Radio sends | Adapter does | Adapter replies |
|---|---|---|
| power-on | wait 200 ms, send `A0`, 800 ms, `A1` | |
| `E0 xx xx` | nothing | `A0`, then `A1` |
| `F0 <freq>` | nothing (AH-4 keeps its latched match; it measures frequency itself) | `A0`, then `A1` |
| `F1 00 00` | 70 ms START pulse: AH-4 to bypass. Matches the radio's intent (bypass, then measure raw SWR) | `A0`, then `A1` after the pulse |
| `F2 <freq>` | `A0` within 50 ms, TX INH high, START low; on KEY low release TX INH (RF reaches the AH-4 only now), release START 250 ms later; run the state machine below | `A0` at once, `A1` on KEY release, `A2` after `A1` on failure, `B0` before `A1` on timeout waiting for KEY |

Tune state machine on `F2`:

```
IDLE --F2--> ACK: send A0; START=low; t=0
ACK --KEY low--> KEYED: (radio is already transmitting; optional: release TX_INH here)
ACK --t>1000ms--> FAIL_NOKEY: START=high; send B0, A1; -> IDLE
KEYED --t>=560ms or 250ms after KEY--> HUNT: START=high
HUNT --KEY high--> SETTLE: t2=0
SETTLE --KEY low within 100ms--> FAILING: wait for KEY high; send A1 then A2; -> IDLE
SETTLE --t2>100ms, KEY still high--> OK: send A1; -> IDLE
HUNT --t>16s--> TIMEOUT: START 70ms pulse (bypass); send A1, A2; -> IDLE
```

Notes:

- TX INH is ACTIVE HIGH on the FT-891 and gates only the RF (section 8).
  Drive it high from `F2` until the AH-4 asserts KEY, then release. The
  radio keeps its sequence either way, so this is optional but keeps RF off
  the AH-4 while its relays switch, as an Icom radio would.
- TX GND (pin 2) is worth reading as an input anyway: it tells the adapter
  when the radio is actually transmitting, useful for the B0 decision and for
  a sniffer/debug log.
- The radio decides tune result by its own SWR measurement after `A1`, so
  `A2` matters only for the Hi-SWR indication and memory, not for safety.
- Cancel: if the radio drops the carrier (TX GND high) during HUNT, the AH-4
  aborts on its own (no power). Report `A1 A2`.

## 4. Hardware

### Signals the micro needs

| Signal | Dir | Interface |
|---|---|---|
| RADIO_RX (TX D, pin 4) | in | 5 V TTL, 270 ohm series, direct |
| RADIO_TX (RX D, pin 5) | out | 5 V TTL, 270 ohm series, direct |
| TX_INH (pin 8) | out, open drain | Schottky from pin like Yaesu, or 2N7000. Optional in v1 |
| TX_GND (pin 2) | in | 5 V pull-up in adapter, optional |
| SENSE (pin 6) | | hard-wire to GND |
| START (AH-4 white) | out, open drain | 2N7000 or NPN; pull-up 10 k to 5 V in adapter (tuner input is a PNP base, it only needs to be pulled low) |
| KEY (AH-4 green) | in | 10 k pull-up to 5 V in adapter, no level shift needed (open collector) |
| +13.5 V (pin 1) | power | to 5 V regulator and to the AH-4 red wire through a 1 A fuse |

Minimum is 4 GPIO (RX, TX, START, KEY). Full is 6 (plus TX_INH, TX_GND).

### Digispark rev3 (ATtiny85) assessment

Workable for a first prototype, with real limitations:

- No hardware UART. 4800 baud is slow enough to bit-bang reliably
  (208 us per bit) or to use the USI. Half-duplex is enough: the adapter
  only talks after the radio has finished a command.
- 6 I/O pins, only P0, P1, P2 are clean. P3 and P4 are the USB data lines
  (P3 has a 1.5 k pull-up to 5 V, both have 68 ohm series resistors). They
  are usable once the sketch is running since the bootloader releases USB,
  and P3's pull-up even suits the KEY input. P5 is RESET unless the fuse is
  changed, which then blocks further ISP programming. P1 has the on-board
  LED.
- 8 KB flash minus about 2 KB Micronucleus bootloader, 512 B RAM. The state
  machine plus soft serial fits easily in C with avr-gcc. Avoid DigiCDC/V-USB
  at runtime: its interrupts would break bit-banged serial timing.
- No debug console while running. Log on a spare pin through a USB-serial
  dongle, or use a logic analyzer on the lines.
- Powers from the radio: VIN (78M05) takes 13.5 V. The AH-4 relay current
  should not go through the Digispark; wire pin 1 directly to the AH-4 with
  a fuse.

Better final target: ATtiny1614/ATtiny404 (hardware UART, UPDI, 5 V, SOIC)
or an Arduino Pro Micro / Pi Pico if size does not matter. Same firmware,
fewer workarounds.

### Nice to have

- 2-colour LED mirroring tune state.
- A manual TUNE button so the adapter also works with the LAMP-jack hack.

## 5. Plan

1. Build a passive sniffer first if an FC-30/FC-40 is still around: USB-serial
   dongle on TX D and another on RX D, logic analyzer on TX GND and TX INH.
   Capture power-on, F0 stream, a full TUNE, a failed TUNE, cancel, band
   change. This resolves most unknowns in section 1.
2. Without a Yaesu tuner: bench the adapter against the radio with a dummy
   load and no AH-4, logging what the radio sends and how it reacts to the
   replies and their timing. Measure tune carrier power.
3. Bench the AH-4 side with the Digispark alone: START pulse, watch KEY,
   drive RF from the radio keyed manually. Reproduce K9EQ figures 4-6.
4. Combine.

## 6. Other projects for reference

- SimonAnnetts yaesu_icom_atu_seq_v2: FT-847 via CAT, Arduino Mega,
  opto-isolated START/KEY, measured timings.
- HB9DUT TCI_ICOM_Tuner_Interface: ESP32 driving an AH-4 from an SDR.
- CommsWG Icom AH4 to SGC protocol converter (Arduino Pro Micro).
- IRYS "Yaesu to Icom/LDG tuner active converter" on Tindie: a commercial
  version of exactly this adapter. Tested with FT-891 and AH-4. Closed design,
  but proof that the approach works with native TUNE and on-screen status.
- YO3HJV AH-4 tuning aid: opto-coupled START/KEY box for any radio.

## 7. Measuring tune power: Pecab PE120-B bridge

Photos in `docs/images/pecab-pe120b-*.jpg`. Label: Power/SWR Bridge, 1-150 MHz,
1-100 W, directivity >= 30 dB, insertion loss <= 0.2 dB, Z0 = 50 ohm, SO-239
both ends, arrow TX/RX to Antenna. Inside: "Dual directional coupler
091A/1.13-0.8", coupling 15 dB, max 100 W. Two sense boards, FWD-Sense (J1)
and REFL-Sense (J2), each with a diode detector, RF bypass caps (1 nF) and a
trimmer, with "+" marked DC outputs. The grey cable carries FWD (blue) and
REFL (red) DC out, originally to a meter unit.

So the sense outputs are rectified DC, not RF. Expected levels: at 100 W the
coupled port sees 100 W - 15 dB = 3.2 W, about 18 V peak; at 10 W about 5.6 V
peak; at 1 W about 1.8 V peak. Diode detectors are square-law below roughly
1 V and linear above, so the curve must be calibrated, not computed.

Plan:
1. Calibrate: radio into dummy load through the bridge, CW carrier at
   PC005/010/020/050/100, read FWD DC with a multimeter. Cross-check against
   the radio's PO meter (`RM5;`). Five points on each band of interest.
2. Feed FWD into a Teensy ADC pin through a 47 k / 10 k divider (18 V -> 3.2 V)
   with a 5.1 V zener or Schottky clamp to +5 V. Sample at 1 kHz during
   TX GND low and log min/max/mean every 10 ms. That gives the tune carrier
   power versus time, including any overshoot at key-on.
3. REFL into a second ADC pin the same way gives SWR during the tune when
   the AH-4 is connected later.

### Measured forward calibration (2026-10-11, into dummy load, ~14 MHz)

`coupler.csv`, FWD sense (blue wire) DC voltage with a multimeter, radio set
to each power level by its PC setting:

| W | 5 | 10 | 20 | 30 | 40 | 50 | 60 | 70 | 80 | 90 | 100 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| V | 0.154 | 0.290 | 0.490 | 0.616 | 0.751 | 0.870 | 0.953 | 1.044 | 1.127 | 1.217 | 1.299 |

Far lower than the raw detector estimate, so the trimmer divides the detected
voltage down for the original meter. The curve fits a linear detector with a
diode-drop offset to about 1 % rms:

    V = 0.146 * (sqrt(P) - 1.18)        P = (V / 0.146 + 1.18)^2

A pure power law or a square-law blend fit worse. The model is only valid
above about 2 W (it reaches 0 V at 1.4 W); below that the detector is
square-law, which does not matter for the 5-15 W question. Accuracy is
limited by the radio's own power setting, not the fit.

Consequences:
- No divider. FWD and REFL go straight to Teensy ADC pins with the internal
  2.56 V reference: 1 count = 2.5 mV, 5 W = 61 counts, 10 W = 116, 15 W = 152,
  100 W = 519. Around 10 W one count is about 0.1 W.
- Add 100 nF from each ADC pin to ground (the detector output impedance is
  unknown and probably high) and a 1 k series resistor. The 5 V clamp diode is
  moot at these levels; the AVR's internal diodes cover any surprise.
- Firmware samples both channels every 10 ms whenever TX GND is low (or when
  forced on) and logs `PWR <fwdV> <reflV> <fwdW> <swr>`.

## 8. Bench results, FT-891 with the Teensy emulator (2026-10-11)

Setup: Teensy 2.0 emulator on TUN/LIN, radio driven over CAT at 4800
(`PS1;` over USB does not wake the FT-891, `PS0;` does turn it off), dummy
load through the Pecab bridge, CW mode, 14.02 MHz. Raw logs in
`logs/session.log` on shack.

### Power-on (menu 16-15 = EXTERNAL, SENSE grounded)

- RESET (DIN 7) is low while the radio is off and also while it is on but
  has not registered a tuner. It goes high at power-up when SENSE is
  grounded. The radio refuses `AC001;` with `?` until that power-up.
- Emulator sent `A0` 200 ms and `A1` 1000 ms after RESET rose. 14 ms after
  that `A1` the radio began: `FF E0 01 00`, `FF E0 00 01`, `FF F1 00 00`,
  each `FF` 50 ms before its three bytes, commands 100 ms apart. It did not
  wait for `A0`/`A1` between them.

### Tuner on/off and frequency

- `AC001;` -> `FF F0 <bcd>`, `AC000;` -> `FF F1 00 00`.
- Frequency is truncated to 10 kHz (14.02894 MHz -> `14 02`). `F0` is
  re-sent only when the 10 kHz value changes, within ~130 ms of the change.

### Tune (`AC002;`), emulator replying A0 after 10 ms

```
t=0        radio: FF F1 00 00
+10 ms     emu:   A0
+800 ms    emu:   A1
+850 ms    TX GND low, carrier on            <- radio waits for A1 before keying
+1300 ms   radio: FF F2 <bcd>                <- ~480 ms after key-on
+1310 ms   emu:   A0
+1310+T    emu:   A1  (T = simulated hunt)
+100 ms    TX GND high, carrier off
```

- The carrier is continuous from before `F2` until 100 ms after `A1`. There
  is no separate SWR test carrier afterwards; the radio measures during the
  last part of the carrier. `AC;` reads `AC001` afterwards.
- Tune carrier power follows the PC setting: PC005 gave 5.1 W steady, 5.7 W
  in the first 30 ms; PC010 gave 10.2 W, 11.2 W peak. Settled within
  100 ms. PC010 is right in the AH-4's 5-15 W window.
- `A1 A2` (fail) and `A0 B0 .. A1` (no carrier): radio unkeys 100 ms after
  `A1` either way, `AC` stays 001. Display not observed yet.
- No reply at all: radio sent `F1` and never keyed. A late `A0 A1` after
  21 s was ignored, the radio had given up silently; `AC` still read 001.

### Timing limits (measured with the emulator)

| What | Result |
|---|---|
| `A1` after `F1` | accepted at 2.5 s, rejected at 3 s: radio cancels the tune, `AC` reads 000, no carrier |
| `A0` after `F1` | not needed (A1 alone works) |
| `A0` after `F2` | must arrive within ~50 ms. At 100 ms and later the radio drops the carrier 54 ms after `F2` but still accepts the later `A1` |
| `A1` after `F2` | 15 s hunt accepted, carrier held the whole time (15.6 s). Limit not found |
| `F2` after key-on | 440-500 ms |
| unkey after `A1` | 100-110 ms |
| cancel (`AC000;` mid-tune) | unkeys within 70 ms, sends `FF F1 00 00`, tuner off. Nothing else |
| TX INH | ACTIVE HIGH. Idles at 0 V (radio pull-down). Driving it low does nothing. Driving it HIGH cuts the RF output to zero while the radio stays in TX (TX GND low, `TX;` = TX1); RF returns as soon as the line is released. Same in EXTERNAL and LAMP mode. Measured with an FM carrier at PC005 (exp21) |
| CAT during tune | `AC;` -> `AC002`, `TX;` -> `TX2` |

### What this means for the adapter

- Reply `A0` to every command immediately (well under 50 ms). Hardware UART,
  not a slow poll loop.
- On `F1`: 70 ms START pulse to put the AH-4 in bypass, then `A1`. Budget
  is 2.5 s, so there is room to wait for the pulse and settle.
- On `F2`: `A0` at once, then START low. The carrier is already on (since
  ~0.5 s before `F2`), so the AH-4 sees RF when it boots and asserts KEY
  ~300 ms later; release START 250 ms after KEY. Wait for KEY to go high,
  classify (clean = ok, 20 ms blip + 200 ms = fail), send `A1` (+`A2`). The
  radio will hold the carrier for at least 15 s, which covers the AH-4's
  worst case.
- The AH-4 measures its own carrier window; the radio's 5.7 W / 11 W
  key-on peak settles in 100 ms, long before the AH-4 looks. Tune with the
  radio's PC set to 10 W; the tune carrier follows the PC setting.
- TX INH (drive HIGH) cuts RF without ending the TX state, and the radio's
  tune sequence is unaffected by it (exp22: inhibited for 1 s or 2.5 s after
  `F2`, or raised before `F1`, the radio still keyed, sent `F2` 0.5 s later,
  waited for `A1` and unkeyed 100 ms after it). So the adapter can hold RF
  off from `F2` until the AH-4 has booted and asserted KEY, which is exactly
  the Icom sequence: the AH-4 only sees RF once its bridge is switched in.
- If the AH-4 never asserts KEY, send `B0` then `A1` within a couple of
  seconds; the radio unkeys on `A1`.
- Cancel from the radio arrives as `F1`: pulse START (reset) and `A1`.

### LAMP mode (menu 16-15 = 3), measured 2026-10-12

With the Teensy floating DIN 4-7 (pull-ups) the radio drives them as band
data, matching the standard Yaesu 4-bit code on the TUN/LIN jack:

| DIN | Signal in LAMP mode |
|---|---|
| 4 (TX D) | BAND A, bit 0 |
| 5 (RX D) | BAND B, bit 1 |
| 6 (SENSE) | BAND C, bit 2 |
| 7 (RESET) | BAND D, bit 3 |

| Band | 160 | 80 | 40 | 30 | 20 | 17 | 15 | 12 | 10 | 6 m |
|---|---|---|---|---|---|---|---|---|---|---|
| code | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |

The code updates about 350 ms after the CAT frequency change, with a brief
intermediate code visible on some transitions.

TX INH in LAMP mode: idles at 0 V; pulling it low does nothing; driving it
high cuts the RF carrier (FM, 5 W -> 0 W within one 10 ms sample) while TX
GND stays low, and RF returns on release. Identical in EXTERNAL mode. So the
FT-891's TX INH is an active-high RF inhibit, not a PTT inhibit.

### TX INH during a tune (exp22)

Inhibit driven high at `F2` for 1 s and for 2.5 s, and raised before `F1`:
the radio keyed on schedule, sent `F2` 0.5 s after keying, waited for `A1`
and unkeyed 100 ms after it in every case. RF after release is back at full
level within one 10 ms sample, no overshoot (5.3 W peak, settling to 5.0 W
over 100 ms at PC005). So the adapter can gate RF freely during the tune.
