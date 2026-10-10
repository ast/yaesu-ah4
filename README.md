# yaesu-ah4

Adapter to run an Icom AH-4 antenna tuner from a Yaesu FT-891's TUN/LIN jack
as if it were a Yaesu FC-40. See `docs/design-notes.md` for the protocols and
the plan.

## Current stage: protocol bench

A Teensy 2.0 on the TUN/LIN jack pretends to be an FC-40 and logs everything
the radio does, while the radio is driven over CAT into a dummy load.

### Toolchain

```sh
direnv allow                 # nix shell: arduino-cli, teensy-loader-cli, python+pyserial
arduino-cli core install teensy:avr   # once; lands in .arduino15/ (gitignored)
tools/build.sh tuner_emu              # compile
tools/build.sh tuner_emu flash        # compile + flash (press the Teensy button)
```

NixOS needs the Teensy udev rules for flashing without root:
`services.udev.packages = [ pkgs.teensy-udev-rules ];` and your user in
`dialout`.

### Wiring Teensy 2.0 to the TUN/LIN jack (8-pin mini-DIN)

| Teensy pin | DIN pin | Signal | Notes |
|---|---|---|---|
| 7 (RX1) | 4 | TX D | radio to tuner serial, 5 V TTL |
| 8 (TX1) | 5 | RX D | tuner to radio serial, 270 ohm in series |
| 0 | 2 | TX GND | low while radio transmits |
| 1 | 7 | RESET | high while radio is on, 10 k in series |
| 2 | 8 | TX INH | open drain from the Teensy, released at boot |
| GND | 3, 6 | GND, SENSE | SENSE strapped to GND = "tuner present" |
| VIN | 1 | +13.5 V | optional, else power the Teensy from USB |

Mini-DIN pin numbering as in the FT-891 manual page 111 and the FC-50
schematic. Radio menu 16-15 TUNER SELECT must be EXTERNAL and the radio
powered up with SENSE grounded.

CAT: FT-891 USB jack, CP210x "Enhanced" port shows up as `/dev/ttyUSB*`.
Set menu 05-06 CAT RATE to 38400 (3) or pass `--cat-baud`.

### Running a session

```sh
tools/session.py --emu /dev/ttyACM0 --cat /dev/ttyUSB1   # keeps running
# other shell:
echo 'cat ID;'        > logs/cmd      # expect CAT< ID0650;
echo 'cat EX1615;'    > logs/cmd      # tuner select, expect EX16151;
echo 'cat PC010;'     > logs/cmd      # 10 W
echo 'cat AC002;'     > logs/cmd      # start tuning
echo 'emu ?'          > logs/cmd      # emulator status
echo 'emu result fail'> logs/cmd      # next tune answers A1 A2
tail -f logs/session.log
```

Emulator commands (send with `emu ...`): `?`, `s a0 a1` (raw bytes to the
radio), `auto 0|1`, `boot [0|1]`, `a0 <ms>`, `a1 <ms>`, `tune <ms>`,
`result ok|fail|b0`, `inhtune 0|1`, `inh 0|1`, `start 0|1`, `clear`,
`note <text>`.

### Experiments to run

1. Power-on: radio off, Teensy running, turn radio on. Capture RESET edge,
   boot `A0`/`A1`, the `E0` pair and `F0`/`F1`.
2. `AC001;` / `AC000;` tuner on/off: which commands, and does the radio wait
   for `A0`/`A1`?
3. VFO steps via `FA...;`: confirm `F0` every 10 kHz, only when enabled.
4. `AC002;` tune, result ok: full byte timing, TX GND edges, how long the
   radio keeps the carrier, when it measures SWR. Read `RM6;` during.
5. Same with `result fail`, `result b0`, and with no reply at all
   (`auto 0`): radio timeout and display.
6. Tune carrier power vs `PC005;` `PC010;` `PC050;` `PC100;` with a
   wattmeter on the dummy load.
7. `inhtune 1`: does TX INH stop the carrier, and does it resume on release?
8. Long `tune 15000`: radio patience.
9. Cancel a tune (press MULTI or `AC000;` mid-tune): what is sent.
10. Band change with tuner enabled: what is sent.
