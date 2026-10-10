#!/usr/bin/env sh
# Build (and optionally flash) a sketch for the Teensy 2.0.
#   tools/build.sh tuner_emu          # compile only
#   tools/build.sh tuner_emu flash    # compile, then press the Teensy button to flash
set -e
cd "$(dirname "$0")/.."
sketch=${1:-tuner_emu}
arduino-cli compile --fqbn teensy:avr:teensy2:usb=serial \
  --output-dir "build/$sketch" "firmware/$sketch"
if [ "$2" = "flash" ]; then
  echo "Press the button on the Teensy..."
  teensy-loader-cli --mcu=atmega32u4 -w -v "build/$sketch/$sketch.ino.hex"
fi
