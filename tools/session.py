#!/usr/bin/env python3
"""Bench session: owns the Teensy adapter port and the FT-891 CAT port,
merges everything into one timestamped log, and takes commands from a FIFO
so that other shells (or Claude) can drive the test without fighting over
the serial ports.

    tools/session.py --emu /dev/ttyACM0 --cat /dev/ttyUSB1 [--cat-baud 38400]

Then from another shell:

    echo 'cat FA;'      > logs/cmd     # send a CAT command, reply shows in log
    echo 'emu ?'        > logs/cmd     # send a line to the adapter
    echo 'note tune 1'  > logs/cmd     # free-text marker in the log
    echo 'quit'         > logs/cmd
    tail -f logs/session.log

Log line format:  <unix time>  <SRC>  <text>
  EMU   line from the Teensy (its own micros() timestamp is inside)
  CAT<  CAT reply bytes from the radio
  CAT>  what we sent to the radio
  NOTE  marker
"""

import argparse
import os
import threading
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emu", help="Teensy USB serial port")
    ap.add_argument("--cat", help="FT-891 CAT port (CP210x enhanced port)")
    ap.add_argument("--cat-baud", type=int, default=38400,
                    help="menu 05-06 CAT RATE; 38400 recommended")
    ap.add_argument("--log", default="logs/session.log")
    ap.add_argument("--fifo", default="logs/cmd")
    args = ap.parse_args()

    os.makedirs(os.path.dirname(args.log), exist_ok=True)
    if not os.path.exists(args.fifo):
        os.mkfifo(args.fifo)

    logf = open(args.log, "a", buffering=1)
    lock = threading.Lock()

    def log(src, text):
        line = f"{time.time():.6f} {src:5s} {text}"
        with lock:
            logf.write(line + "\n")
            print(line, flush=True)

    emu = serial.Serial(args.emu, 115200, timeout=0.05) if args.emu else None
    cat = serial.Serial(args.cat, args.cat_baud, timeout=0.05) if args.cat else None

    stop = threading.Event()

    def reopen(port, name):
        """Block until the port can be opened again (USB re-enumeration)."""
        while not stop.is_set():
            try:
                port.close()
                port.open()
                log("NOTE", f"{name} port reopened")
                return
            except serial.SerialException:
                time.sleep(1)

    def emu_reader():
        assert emu is not None
        buf = b""
        while not stop.is_set():
            try:
                data = emu.read(256)
            except (serial.SerialException, OSError) as e:
                log("NOTE", f"emu port error: {e}")
                reopen(emu, "emu")
                continue
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("ascii", "replace").rstrip()
                # The radio ignores CAT while it transmits; tell the watchdog.
                if text.endswith(" TXGND 0"):
                    cat_state["tx"] = True
                elif text.endswith(" TXGND 1"):
                    cat_state["tx"] = False
                    cat_state["sent"] = 0.0
                log("EMU", text)

    cat_state = {"sent": 0.0, "got": 0.0, "tx": False}

    def cat_reader():
        assert cat is not None
        buf = b""
        while not stop.is_set():
            # Watchdog: a CAT command with no reply for 3 s means the CP2105
            # endpoint stalled (RF into USB); reopening the port clears it.
            if (not cat_state["tx"] and cat_state["sent"] > cat_state["got"]
                    and time.time() - cat_state["sent"] > 3):
                log("NOTE", "cat watchdog: no reply for 3 s, reopening port")
                cat_state["sent"] = 0.0
                reopen(cat, "cat")
                continue
            try:
                data = cat.read(256)
            except (serial.SerialException, OSError) as e:
                log("NOTE", f"cat port error: {e}")
                reopen(cat, "cat")
                continue
            if not data:
                continue
            buf += data
            while b";" in buf:
                msg, buf = buf.split(b";", 1)
                cat_state["got"] = time.time()
                cat_state["sent"] = 0.0
                log("CAT<", msg.decode("ascii", "replace") + ";")

    threads = []
    if emu:
        threads.append(threading.Thread(target=emu_reader, daemon=True))
    if cat:
        threads.append(threading.Thread(target=cat_reader, daemon=True))
    for t in threads:
        t.start()

    log("NOTE", f"session start emu={args.emu} cat={args.cat}@{args.cat_baud}")
    if cat:
        cat.write(b"ID;")
        log("CAT>", "ID;")

    # FIFO command loop. Reopen the FIFO after each writer closes it.
    while not stop.is_set():
        with open(args.fifo) as f:
            for raw in f:
                line = raw.strip()
                if not line:
                    continue
                verb, _, rest = line.partition(" ")
                try:
                    if verb == "cat" and cat:
                        cat.write(rest.encode("ascii"))
                        if not cat_state["sent"]:
                            cat_state["sent"] = time.time()
                        log("CAT>", rest)
                    elif verb == "emu" and emu:
                        emu.write((rest + "\n").encode("ascii"))
                        log("EMU>", rest)
                except (serial.SerialException, OSError) as e:
                    log("NOTE", f"write failed ({verb}): {e}")
                    continue
                if verb in ("cat", "emu"):
                    continue
                elif verb == "note":
                    log("NOTE", rest)
                elif verb == "sleep":
                    time.sleep(float(rest))
                elif verb == "quit":
                    stop.set()
                    break
                else:
                    log("NOTE", f"unknown command: {line}")
        time.sleep(0.05)

    log("NOTE", "session end")


if __name__ == "__main__":
    main()
