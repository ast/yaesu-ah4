// tuner_emu: Yaesu FC-40 tuner emulator / protocol logger for Teensy 2.0 (ATmega32U4).
//
// Sits on the radio's TUN/LIN jack, logs everything the radio does on the serial
// and control lines with microsecond timestamps over USB serial, and answers the
// radio either automatically (configurable timings) or only when told to by the
// host.  Later the same board drives the Icom AH-4 START/KEY lines.
//
// Build: arduino-cli compile --fqbn teensy:avr:teensy2:usb=serial firmware/tuner_emu
//
// Wiring (Teensy 2.0 pin number / silkscreen, 8-pin mini-DIN TUN/LIN pin in brackets):
//   7  D2 RX1 <- [4] TX D     radio to tuner serial, 5 V TTL
//   8  D3 TX1 -> [5] RX D     tuner to radio serial, 5 V TTL, 270 ohm in series
//   0  B0     <- [2] TX GND   low while radio transmits (internal pull-up)
//   1  B1     <- [7] RESET    radio pulls low to reset, else floats: internal pull-up (10 k in series)
//   2  B2     -> [8] TX INH   open drain: INPUT = released, OUTPUT LOW = asserted
//   3  B3     -> AH-4 START   via 2N7000 (gate), not used yet
//   4  B7     <- AH-4 KEY     10 k pull-up to 5 V, not used yet
//   21 F0 A0  <- bridge FWD sense (blue), 1 k series + 100 nF to GND, 2.56 V ref
//   20 F1 A1  <- bridge REFL sense (red), same
//   GND       -- [3] GND and [6] SENSE (strap SENSE to GND so the radio sees a tuner)
//   Leave [1] +13.5 V unconnected; the Teensy 2.0 has no regulator, power it from USB.
//
// Host protocol (USB serial, 115200, line based):
//   log lines:   "<us> <EVENT> ..."   microseconds since boot
//   commands:    see handleHostLine()

#include <Arduino.h>

static const uint8_t PIN_TXGND = 0;
static const uint8_t PIN_RESET = 1;
static const uint8_t PIN_TXINH = 2;
static const uint8_t PIN_START = 3;
static const uint8_t PIN_KEY   = 4;
static const uint8_t PIN_LED   = 11;
static const uint8_t PIN_FWD   = A0;   // Teensy 2.0 pin 21
static const uint8_t PIN_REFL  = A1;   // Teensy 2.0 pin 20

// Pecab PE120-B forward sense calibration, see docs/design-notes.md section 7:
//   V = CAL_K * (sqrt(P) - CAL_C)   ->   P = (V / CAL_K + CAL_C)^2
static float calK = 0.146f;
static float calC = 1.18f;
static const float ADC_VREF = 2.56f;   // internal reference

// ---------------------------------------------------------------- settings
struct Settings {
  bool     autoReply   = true;   // answer commands automatically
  bool     bootSeq     = true;   // send A0/A1 when RESET rises (radio power-on)
  uint16_t a0Delay     = 10;     // ms after a complete command before A0
  uint16_t a1Delay     = 800;    // ms after A0 before A1 (non-tune commands)
  uint16_t tuneTime    = 3000;   // ms after A0 before A1 for F2 (simulated hunt)
  uint8_t  tuneResult  = 0;      // 0 = ok (A1), 1 = fail (A1 A2), 2 = no carrier (B0 .. A1)
  bool     inhDuringTune = false;// hold TX INH low from F2 until "inh 0" or tuneTime/2
  uint8_t  pwrMode     = 1;      // 0 = off, 1 = while TX GND low, 2 = always
  uint16_t pwrPeriod   = 10;     // ms between PWR samples
} cfg;

// ---------------------------------------------------------------- logging
static void logTs() {
  Serial.print(micros());
  Serial.print(' ');
}
static void logHex(uint8_t b) {
  if (b < 0x10) Serial.print('0');
  Serial.print(b, HEX);
}
static void logByteEvent(const char *tag, uint8_t b) {
  logTs();
  Serial.print(tag);
  Serial.print(' ');
  logHex(b);
  Serial.println();
}
static void logNote(const char *s) {
  logTs();
  Serial.print(F("NOTE "));
  Serial.println(s);
}

// ---------------------------------------------------------------- line I/O
static void txInh(bool assert) {
  if (assert) {
    pinMode(PIN_TXINH, OUTPUT);
    digitalWrite(PIN_TXINH, LOW);
  } else {
    pinMode(PIN_TXINH, INPUT);
  }
  logTs();
  Serial.print(F("INH "));
  Serial.println(assert ? 1 : 0);
}

static void sendToRadio(uint8_t b) {
  Serial1.write(b);
  logByteEvent("TX", b);
}

// Scheduled replies: up to 4 pending bytes with absolute send times.
struct Pending {
  uint32_t at;     // millis()
  uint8_t  b;
  bool     used;
};
static Pending pend[4];

static void schedule(uint32_t atMs, uint8_t b) {
  for (auto &p : pend) {
    if (!p.used) {
      p.used = true;
      p.at = atMs;
      p.b = b;
      return;
    }
  }
  logNote("pending queue full, byte dropped");
}
static void clearPending() {
  for (auto &p : pend) p.used = false;
}
static void servicePending() {
  uint32_t now = millis();
  for (auto &p : pend) {
    if (p.used && (int32_t)(now - p.at) >= 0) {
      p.used = false;
      sendToRadio(p.b);
    }
  }
}

// ---------------------------------------------------------------- radio RX
// Frame: FF, ~50 ms gap, op, hi, lo.  We resync on every FF.
static uint8_t  frame[4];
static uint8_t  frameLen = 0;
static uint32_t lastRxUs = 0;

static void onCommand(const uint8_t *f) {
  logTs();
  Serial.print(F("CMD "));
  for (uint8_t i = 0; i < 4; i++) {
    logHex(f[i]);
    Serial.print(' ');
  }
  uint8_t op = f[1];
  switch (op) {
    case 0xE0: Serial.print(F("init")); break;
    case 0xF0: Serial.print(F("enable/freq")); break;
    case 0xF1: Serial.print(F("bypass")); break;
    case 0xF2: Serial.print(F("TUNE")); break;
    default:   Serial.print(F("unknown")); break;
  }
  if (op == 0xF0 || op == 0xF2) {
    // BCD: hi = MHz tens/ones, lo = 100 kHz / 10 kHz
    Serial.print(' ');
    Serial.print((f[2] >> 4) * 10 + (f[2] & 0x0F));
    Serial.print('.');
    Serial.print(f[3] >> 4);
    Serial.print(f[3] & 0x0F);
    Serial.print(F(" MHz"));
  }
  Serial.println();

  if (!cfg.autoReply) return;

  uint32_t t0 = millis() + cfg.a0Delay;
  if (op == 0xF2) {
    if (cfg.inhDuringTune) txInh(true);
    switch (cfg.tuneResult) {
      case 0:
        schedule(t0, 0xA0);
        schedule(t0 + cfg.tuneTime, 0xA1);
        break;
      case 1:
        schedule(t0, 0xA0);
        schedule(t0 + cfg.tuneTime, 0xA1);
        schedule(t0 + cfg.tuneTime + 20, 0xA2);
        break;
      case 2:
        schedule(t0, 0xA0);
        schedule(t0 + 100, 0xB0);
        schedule(t0 + 800, 0xA1);
        break;
    }
    if (cfg.inhDuringTune) {
      // release half way, see what the radio does
      // (handled in loop via inhReleaseAt)
    }
  } else {
    schedule(t0, 0xA0);
    schedule(t0 + cfg.a1Delay, 0xA1);
  }
}

static void serviceRadioRx() {
  while (Serial1.available()) {
    uint8_t b = Serial1.read();
    uint32_t now = micros();
    logTs();
    Serial.print(F("RX "));
    logHex(b);
    Serial.print(F(" gap="));
    Serial.println(now - lastRxUs);
    lastRxUs = now;

    if (b == 0xFF) {
      frameLen = 0;
    }
    if (frameLen < 4) {
      frame[frameLen++] = b;
      if (frameLen == 4) {
        onCommand(frame);
        frameLen = 0;
      }
    }
  }
}

// ---------------------------------------------------------------- control lines
static int lastTxGnd = -1, lastReset = -1, lastKey = -1;

static void serviceLines() {
  int v;
  v = digitalRead(PIN_TXGND);
  if (v != lastTxGnd) {
    lastTxGnd = v;
    logTs();
    Serial.print(F("TXGND "));
    Serial.println(v);
    digitalWrite(PIN_LED, v == LOW);
  }
  v = digitalRead(PIN_RESET);
  if (v != lastReset) {
    bool rose = (lastReset == LOW && v == HIGH);
    lastReset = v;
    logTs();
    Serial.print(F("RESET "));
    Serial.println(v);
    if (rose && cfg.bootSeq) {
      uint32_t t = millis();
      clearPending();
      schedule(t + 200, 0xA0);
      schedule(t + 1000, 0xA1);
      logNote("radio on: boot sequence scheduled");
    }
  }
  v = digitalRead(PIN_KEY);
  if (v != lastKey) {
    lastKey = v;
    logTs();
    Serial.print(F("KEY "));
    Serial.println(v);
  }
}

// ---------------------------------------------------------------- power
static uint32_t lastPwrMs = 0;

static float adcVolts(uint8_t pin) {
  (void)analogRead(pin);                // settle mux after channel change
  return analogRead(pin) * (ADC_VREF / 1023.0f);
}
static float voltsToWatts(float v) {
  if (v < 0.02f) return 0;          // below the detector knee: call it zero
  float s = v / calK + calC;
  return s > 0 ? s * s : 0;
}

static void servicePower() {
  if (cfg.pwrMode == 0) return;
  if (cfg.pwrMode == 1 && digitalRead(PIN_TXGND) == HIGH) return;
  uint32_t now = millis();
  if (now - lastPwrMs < cfg.pwrPeriod) return;
  lastPwrMs = now;
  float vf = adcVolts(PIN_FWD);
  float vr = adcVolts(PIN_REFL);
  float pf = voltsToWatts(vf);
  float pr = voltsToWatts(vr);
  float swr = 0;
  if (pf > 0.5f && pr > 0) {
    float rho = sqrt(pr / pf);
    if (rho < 0.999f) swr = (1 + rho) / (1 - rho);
    else swr = 99;
  }
  logTs();
  Serial.print(F("PWR "));
  Serial.print(vf, 4); Serial.print(' ');
  Serial.print(vr, 4); Serial.print(' ');
  Serial.print(pf, 2); Serial.print(' ');
  Serial.println(swr, 2);
}

// ---------------------------------------------------------------- host commands
static char hostLine[64];
static uint8_t hostLen = 0;
static uint32_t hostLastMs = 0;

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void printStatus() {
  logTs();
  Serial.print(F("STATUS auto="));
  Serial.print(cfg.autoReply);
  Serial.print(F(" boot="));
  Serial.print(cfg.bootSeq);
  Serial.print(F(" a0="));
  Serial.print(cfg.a0Delay);
  Serial.print(F(" a1="));
  Serial.print(cfg.a1Delay);
  Serial.print(F(" tune="));
  Serial.print(cfg.tuneTime);
  Serial.print(F(" result="));
  Serial.print(cfg.tuneResult);
  Serial.print(F(" inhtune="));
  Serial.print(cfg.inhDuringTune);
  Serial.print(F(" pwr="));
  Serial.print(cfg.pwrMode);
  Serial.print(F("/"));
  Serial.print(cfg.pwrPeriod);
  Serial.print(F(" txgnd="));
  Serial.print(digitalRead(PIN_TXGND));
  Serial.print(F(" reset="));
  Serial.print(digitalRead(PIN_RESET));
  Serial.print(F(" key="));
  Serial.println(digitalRead(PIN_KEY));
}

static void handleHostLine(char *line) {
  // strip trailing CR/LF
  char *cmd = strtok(line, " \r\n");
  if (!cmd) return;
  char *arg = strtok(nullptr, " \r\n");

  if (!strcmp(cmd, "?") || !strcmp(cmd, "status")) {
    printStatus();
  } else if (!strcmp(cmd, "send") || !strcmp(cmd, "s")) {
    // send <hex bytes...> immediately, e.g. "s a0 a1"
    clearPending();
    while (arg) {
      if (strlen(arg) == 2 && hexVal(arg[0]) >= 0 && hexVal(arg[1]) >= 0) {
        sendToRadio((hexVal(arg[0]) << 4) | hexVal(arg[1]));
      }
      arg = strtok(nullptr, " \r\n");
    }
  } else if (!strcmp(cmd, "auto")) {
    cfg.autoReply = arg && atoi(arg);
    if (!cfg.autoReply) clearPending();
    printStatus();
  } else if (!strcmp(cmd, "boot")) {
    if (arg) cfg.bootSeq = atoi(arg);
    else { schedule(millis() + 200, 0xA0); schedule(millis() + 1000, 0xA1); }
    printStatus();
  } else if (!strcmp(cmd, "a0") && arg) {
    cfg.a0Delay = atoi(arg); printStatus();
  } else if (!strcmp(cmd, "a1") && arg) {
    cfg.a1Delay = atoi(arg); printStatus();
  } else if (!strcmp(cmd, "tune") && arg) {
    cfg.tuneTime = atoi(arg); printStatus();
  } else if (!strcmp(cmd, "result") && arg) {
    if (!strcmp(arg, "ok")) cfg.tuneResult = 0;
    else if (!strcmp(arg, "fail")) cfg.tuneResult = 1;
    else if (!strcmp(arg, "b0")) cfg.tuneResult = 2;
    else cfg.tuneResult = atoi(arg);
    printStatus();
  } else if (!strcmp(cmd, "inhtune") && arg) {
    cfg.inhDuringTune = atoi(arg); printStatus();
  } else if (!strcmp(cmd, "inh") && arg) {
    txInh(atoi(arg));
  } else if (!strcmp(cmd, "start") && arg) {
    // drive AH-4 START via 2N7000: pin HIGH = line pulled low (asserted)
    digitalWrite(PIN_START, atoi(arg) ? HIGH : LOW);
    logTs(); Serial.print(F("START ")); Serial.println(atoi(arg));
  } else if (!strcmp(cmd, "pwr") && arg) {
    // pwr <0|1|2> [period_ms]
    cfg.pwrMode = atoi(arg);
    char *per = strtok(nullptr, " \r\n");
    if (per) cfg.pwrPeriod = atoi(per);
    printStatus();
  } else if (!strcmp(cmd, "cal") && arg) {
    // cal <k> <c>
    char *c2 = strtok(nullptr, " \r\n");
    calK = atof(arg);
    if (c2) calC = atof(c2);
    logTs(); Serial.print(F("CAL k=")); Serial.print(calK, 4);
    Serial.print(F(" c=")); Serial.println(calC, 3);
  } else if (!strcmp(cmd, "adc")) {
    // one-shot raw reading
    logTs(); Serial.print(F("ADC fwd=")); Serial.print(adcVolts(PIN_FWD), 4);
    Serial.print(F(" refl=")); Serial.println(adcVolts(PIN_REFL), 4);
  } else if (!strcmp(cmd, "clear")) {
    clearPending();
    logNote("pending cleared");
  } else if (!strcmp(cmd, "note")) {
    logTs(); Serial.print(F("NOTE "));
    while (arg) { Serial.print(arg); Serial.print(' '); arg = strtok(nullptr, " \r\n"); }
    Serial.println();
  } else {
    logNote("commands: ? | s <hex..> | auto 0/1 | boot [0/1] | a0 <ms> | a1 <ms> | tune <ms> | result ok/fail/b0 | inhtune 0/1 | inh 0/1 | start 0/1 | pwr 0/1/2 [ms] | cal <k> <c> | adc | clear | note ...");
  }
}

static void serviceHost() {
  if (hostLen && millis() - hostLastMs > 1000) hostLen = 0;   // stale partial line
  while (Serial.available()) {
    char c = Serial.read();
    hostLastMs = millis();
    if (c == '\n' || c == '\r') {
      if (hostLen) {
        hostLine[hostLen] = 0;
        handleHostLine(hostLine);
        hostLen = 0;
      }
    } else if (hostLen < sizeof(hostLine) - 1) {
      hostLine[hostLen++] = c;
    }
  }
}

// ---------------------------------------------------------------- setup/loop
void setup() {
  pinMode(PIN_TXGND, INPUT_PULLUP);
  pinMode(PIN_RESET, INPUT_PULLUP);  // radio only pulls it low; FC-50 has a 47 k pull-up
  pinMode(PIN_TXINH, INPUT);        // released
  pinMode(PIN_START, OUTPUT);
  digitalWrite(PIN_START, LOW);     // 2N7000 off = START released
  pinMode(PIN_KEY, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  analogReference(INTERNAL);        // 2.56 V on ATmega32U4

  Serial.begin(115200);
  Serial1.begin(4800);              // 8N1 default

  uint32_t t = millis();
  while (!Serial && millis() - t < 3000) {}
  logNote("tuner_emu ready (FC-40 emulator). '?' for status.");
  printStatus();
}

void loop() {
  serviceRadioRx();
  serviceLines();
  servicePending();
  servicePower();
  serviceHost();
}
