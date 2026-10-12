# yaesu-ah4

Adapter that lets an Icom AH-4 automatic antenna tuner work natively with a
Yaesu FT-891 (and other radios with the FC-40/FC-50 TUN/LIN interface). A
Teensy 2.0 plugs into the radio's 8-pin mini-DIN TUN/LIN jack, speaks Yaesu's
4800 8N1 tuner protocol to the radio, and drives the AH-4's START/KEY lines.
It works; see README.md for wiring and use, `docs/design-notes.md` for every
measured fact (sections 8-10 are the bench results, section 3 the spec).

## Layout

- `firmware/ah4_adapter/` the adapter sketch (Teensy 2.0, arduino-cli with the
  Teensy core, `tools/build.sh`).
- `tools/session.py` bench logger: merges the adapter's USB log and the radio's
  CAT port into `logs/session.log`, commands via the FIFO `logs/cmd`.
- `docs/` manuals and the design notes; `coupler.csv` bridge calibration.

## Bench

The hardware is on the Raspberry Pi `shack` (ssh shack), repo copy in
`~/src/yaesu-ah4` there (rsync from here, excluding .arduino15/.direnv/logs).
Radio CAT: CP2105 "if00" port, 4800 baud. Teensy: Teensyduino USB serial.
Flash on the Pi with `teensy_loader_cli --mcu=atmega32u4 -s -w -v <hex>`
after stopping the session logger (`echo quit > logs/cmd`), since it holds
the port. Do not send `PS0;` to the radio: `PS1;` cannot wake it over USB.
Keep transmissions short; the tune carrier is bounded by the 16 s hunt
timeout.

## Searching the manuals

`docs/` holds the reference PDFs. Search them with ripgrep-all first:

```sh
rga -i 'TX INH' docs/
rga -i --no-heading -n 'START' 'docs/AH-4_Instructions.pdf'
```

Only fall back to other tools when rga cannot do the job:

- `Icom AH-4 Service Manual.pdf` is a scan with no text layer. Render pages
  with `pdftoppm -r 110 -png` and read the images. Sections: 3 = circuit
  description (p6-7), 4-1 = cable pinout (p8), 9 = block diagram (p19),
  10 = schematic (p20-21).
- `Yaesu_FC-50_serv.pdf` has text but the schematics are vector drawings.
  Page 6 (CNTL Unit, top-left) is the radio-side CONTROL connector circuit.
- `pdftotext -layout -f N -l N file.pdf -` for a single page of the FT-891
  manuals. Tuner menu 16-15 on p99, FC-50/FC-40 operation p104-107, TUN/LIN
  pinout drawing p111. CAT manual: `AC` tuner command on p5, menu table p10.

Shell is zsh: never start an `echo` argument with `=` (zsh expands `=word`).
`pkill -f` with a pattern that also appears in your own command line kills
your shell; use `echo quit > logs/cmd` to stop the logger instead.
