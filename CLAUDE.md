# yaesu-ah4

Adapter that lets an Icom AH-4 automatic antenna tuner work natively with a
Yaesu FT-891 (and other radios with the FC-40/FC-50 TUN/LIN interface). The
adapter plugs into the radio's 8-pin mini-DIN TUN/LIN jack, speaks Yaesu's
proprietary 4800 8N1 tuner protocol to the radio, and drives the AH-4's
START/KEY lines.

## Searching the manuals

`docs/` holds the reference PDFs. Search them with ripgrep-all first:

```sh
rga -i 'TX INH' docs/              # all PDFs
rga -i --no-heading -n 'START' 'docs/AH-4_Instructions.pdf'
```

Only fall back to other tools when rga cannot do the job:

- `Icom AH-4 Service Manual.pdf` is a scan with no text layer. rga finds
  nothing in it. Render pages with `pdftoppm -r 110 -png` and read the images.
  Sections: 3 = circuit description (p6-7), 4-1 = cable pinout (p8),
  9 = block diagram (p19), 10 = voltage/schematic (p20-21).
- `Yaesu_FC-50_serv.pdf` has text but the schematics are vector drawings.
  Page 6 (CNTL Unit circuit diagram, top-left) shows the radio-side CONTROL
  connector circuit. Crop at 300 dpi to read component values.
- `pdftotext -layout -f N -l N file.pdf -` for a single page of the FT-891
  manuals. Tuner menu 16-15 is on p99, FC-50/FC-40 operation p104-107,
  TUN/LIN pinout drawing p111. CAT manual: `AC` tuner command on p5.

Shell is zsh: never start an `echo` argument with `=` (zsh expands `=word`
as a command path).

## Documents

| File | What it is |
|---|---|
| `AH-4_Instructions.pdf` | User manual: specs, cable colours, tuning power 5-15 W |
| `Icom AH-4 Service Manual.pdf` | Scanned service manual (circuits, schematic) |
| `AH-4 Brochure.pdf` | Marketing sheet |
| `Yaesu_FC-50_serv.pdf` | FC-50 technical supplement (interface circuit, block diagram) |
| `FT-891_Advance_Manual_ENG_1806-F.pdf` | FT-891 advance manual |
| `FT-891_CAT_OM_ENG_1909-C (1).pdf` | FT-891 CAT reference |

`docs/design-notes.md` is the working design document: both protocols,
the adapter state machine, hardware, and open questions. Keep it updated
when something is measured or decided.
