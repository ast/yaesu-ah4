# yaesu-ah4

Adapter that lets an Icom AH-4 automatic antenna tuner work natively with a
Yaesu FT-891: press TUNE on the radio, the AH-4 tunes, the radio shows the
result. The radio believes it is talking to a Yaesu FC-40 on its TUN/LIN
jack; the adapter speaks that protocol and drives the AH-4's START/KEY lines.

Status: working on the bench with a real antenna (2026-10-12). Everything
measured about the radio and the tuner is in `docs/design-notes.md`.

## How it works

The FT-891 refuses to tune if the raw SWR it measures before the tune is too
high, which would rule out antennas the AH-4 can match. The adapter therefore
starts the AH-4 cycle on the radio's first command (`F1`): by the time the
radio measures, the AH-4 has its matching bridge in line and the radio sees a
good match. The tune command (`F2`) arrives while the AH-4 is already hunting;
the adapter reports the result when the AH-4 releases KEY. Section 3 of the
design notes is the full specification.

## Hardware

Teensy 2.0 (ATmega32U4), one N-MOSFET, a few resistors. Pin numbers are the
Teensy's; the silkscreen prints the AVR port pin.

| Teensy | Silkscreen | Connects to | Note |
|---|---|---|---|
| 7 (RX1) | D2 | DIN 4, TX D | radio to tuner serial, 5 V TTL, direct |
| 8 (TX1) | D3 | DIN 5, RX D | tuner to radio serial, 270 ohm in series |
| 0 | B0 | DIN 2, TX GND | low while the radio transmits |
| 1 | B1 | DIN 7, RESET | 10 k in series; high once the radio has registered the tuner |
| 2 | B2 | DIN 8, TX INH | active high on the FT-891: driven high = RF off. Only used in the classic sequence |
| GND | GND | DIN 3 GND, DIN 6 SENSE | SENSE must be a hard strap to ground |
| 3 | B3 | BS170 / 2N7000 gate | 100 k gate to GND; source GND; drain to AH-4 START (white) |
| 4 | B7 | AH-4 KEY (green) | 10 k pull-up to 5 V. Add an RC (1 k, 10 nF) against RF pickup |
| 21 (A0) | F0 | bridge FWD sense | optional power logging; 1 k series, 68-100 nF to GND |
| 20 (A1) | F1 | bridge REFL sense | same |

DIN numbering as in the FT-891 manual page 111. Do not connect DIN pin 1
(+13.5 V) to the Teensy: it has no regulator and runs from USB. The AH-4 gets
its own 13.8 V with a 1.1 A polyfuse; grounds common.

Radio: menu 16-15 TUNER SELECT = EXTERNAL, then power the radio off and on
with the adapter connected. Set the CW power to 10 W (menu 16-03): the tune
carrier follows the per-mode power setting and the AH-4 wants 5-15 W.

## Firmware

```sh
direnv allow                          # nix shell: arduino-cli, teensy-loader-cli, python+pyserial
arduino-cli core install teensy:avr   # once; lands in .arduino15/ (gitignored)
tools/build.sh                        # compile firmware/ah4_adapter
tools/build.sh ah4_adapter flash      # compile and flash (press the Teensy button, or
                                      # teensy_loader_cli -s soft-reboots a running sketch)
```

On the shack Pi: `teensy_loader_cli --mcu=atmega32u4 -s -w -v build/ah4_adapter/ah4_adapter.ino.hex`
with the Teensy udev rules installed.

## Debugging from the Raspberry Pi

The adapter logs every event over USB serial (115200) as
`<micros> <EVENT> ...`:

| Event | Meaning |
|---|---|
| `RX xx gap=us` | byte from the radio, gap since the previous byte |
| `CMD ff op hi lo <name> [freq]` | decoded radio command |
| `TX xx` | byte sent to the radio |
| `TXGND 0/1`, `RESET 0/1` | radio control lines (0 = low) |
| `KEY 0/1`, `START 0/1`, `INH 0/1/2` | AH-4 KEY (0 = asserted), START (1 = asserted), TX INH level |
| `AH4 <STATE>` | tune state machine transitions |
| `TUNE result=ok|fail|nokey key_ms= hunt_ms= f2_ms= total_ms=` | one line per tune cycle |
| `PWR fwdV reflV fwdW swr` | bridge sample every 10 ms while transmitting |
| `STATUS ...`, `NOTE ...` | settings and line states; free text |

Commands (one per line): `?`, `f1start 0|1` (start the AH-4 on F1, default 1,
or classic on F2), `f1a1 <ms>` (A1 delay after F1, default 100), `boot 0|1`,
`pwr 0|1|2 [ms]`, `cal <k> <c>`, `adc`, `inh 0|1|2`, `start 0|1`, `reset`,
`s <hex..>`, `abort`, `note <text>`.

`tools/session.py` owns the Teensy port and the radio's CAT port, merges both
into one timestamped `logs/session.log`, and takes commands from a FIFO:

```sh
tools/session.py --emu /dev/serial/by-id/usb-Teensyduino_USB_Serial_12345-if00 \
                 --cat /dev/serial/by-id/usb-Silicon_Labs_CP2105_*-if00-port0 --cat-baud 4800
echo 'cat AC002;' > logs/cmd     # tune via CAT
echo 'emu ?'      > logs/cmd     # adapter status
echo 'note 20 m'  > logs/cmd     # marker
tail -f logs/session.log
```

It reopens either port after a USB hiccup, and reopens the CAT port when a
command gets no reply for 3 s (RF into the CP2105 stalls it). `PS1;` over
USB does not wake an FT-891, so leave the radio on.

## Power calibration

`coupler.csv` is the Pecab PE120-B forward sense voltage against power at
14 MHz into a dummy load; the fit `V = 0.146 (sqrt(P) - 1.18)` is built into
the firmware (`cal` changes it at runtime). The detector response scales with
frequency, so the watt numbers are only right near 14 MHz until a per-band
table exists. The AH-4's own power check is the number that matters.
