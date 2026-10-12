// ah4_adapter: Yaesu FT-891 TUN/LIN jack to Icom AH-4 antenna tuner, on a Teensy 2.0.
//
// The radio thinks it is talking to an FC-40. The adapter answers the Yaesu
// serial protocol and drives the AH-4's START/KEY lines. Everything it sees
// and does is logged over USB serial with microsecond timestamps so a host
// (the shack Raspberry Pi) can debug timing and power.
//
// Protocol facts are in docs/design-notes.md sections 8-10. The short version:
//   radio -> tuner: FF, ~50 ms gap, <op> <hi> <lo>; freq as BCD at 10 kHz
//     E0 xx xx  init at power-on        F0 <freq>  tuner on / frequency changed
//     F1 00 00  tuner off / pre-tune    F2 <freq>  tune
//   tuner -> radio: A0 ack (within 50 ms after F2!), A1 done, A2 after A1 = failed,
//     B0 before A1 = no carrier.
//   The radio waits <= 2.5 s for A1 after F1, keys 50 ms after it, sends F2
//   ~450 ms later and holds the carrier until A1 (>= 15 s). Before F2 it checks
//   the raw SWR and refuses to tune if that is too high; starting the AH-4
//   cycle already on F1 makes the radio look into the AH-4's bridge instead.
//
// Build: tools/build.sh ah4_adapter [flash]
//
// Wiring (Teensy 2.0 pin / silkscreen, mini-DIN TUN/LIN pin in brackets):
//   7  D2 RX1 <- [4] TX D     radio to tuner serial, 5 V TTL
//   8  D3 TX1 -> [5] RX D     tuner to radio serial, 270 ohm in series
//   0  B0     <- [2] TX GND   low while the radio transmits (internal pull-up)
//   1  B1     <- [7] RESET    high once the radio has registered a tuner (10 k series, pull-up)
//   2  B2     -> [8] TX INH   ACTIVE HIGH on the FT-891: drive high = RF off; input = released
//   GND       -- [3] GND and [6] SENSE. SENSE must be a hard strap to GND.
//   3  B3     -> BS170/2N7000 gate (100 k to GND), drain to AH-4 START (white), source GND
//   4  B7     <- AH-4 KEY (green), 10 k pull-up to 5 V
//   21 F0 A0  <- bridge FWD sense (blue), 1 k series + 68-100 nF to GND
//   20 F1 A1  <- bridge REFL sense (red), same
//   Power the Teensy from USB (no regulator on a Teensy 2.0). AH-4 from its own 13.8 V.
//
// Host interface (USB serial 115200, line based). Log lines are
//   "<micros> <EVENT> ..."  with EVENT one of RX TX CMD TXGND RESET KEY START INH
//   AH4 TUNE PWR STATUS NOTE. Commands: see handleHostLine().

#include <Arduino.h>

// ---------------------------------------------------------------- pins
static const uint8_t PIN_TXGND = 0;
static const uint8_t PIN_RESET = 1;
static const uint8_t PIN_TXINH = 2;
static const uint8_t PIN_START = 3;
static const uint8_t PIN_KEY   = 4;
static const uint8_t PIN_LED   = 11;
static const uint8_t PIN_FWD   = A0;   // Teensy 2.0 pin 21
static const uint8_t PIN_REFL  = A1;   // Teensy 2.0 pin 20

// ---------------------------------------------------------------- settings
struct Settings {
  bool     startOnF1   = true;   // start the AH-4 cycle on F1 (default) instead of F2
  uint16_t f1A1Delay   = 100;    // ms after F1 before A1 when startOnF1 (50-150 tested good)
  bool     bootSeq     = true;   // send A0/A1 when RESET rises (radio power-on)
  uint8_t  pwrMode     = 1;      // power sampling: 0 off, 1 while TX GND low, 2 always
  uint16_t pwrPeriod   = 10;     // ms between PWR samples
  // Pecab PE120-B forward sense, calibrated at 14 MHz into a dummy load:
  //   V = calK * (sqrt(P) - calC)   ->   P = (V / calK + calC)^2
  float    calK        = 0.146f;
  float    calC        = 1.18f;
} cfg;

static const float    ADC_VREF        = 2.56f;  // internal reference
static const uint16_t AH_RESET_MS     = 70;     // START pulse that puts the AH-4 in bypass
static const uint16_t AH_KEY_TIMEOUT  = 1500;   // ms to wait for KEY after START (AH-4 boots in ~330)
static const uint16_t AH_HOLD_MS      = 250;    // keep START low this long after KEY
static const uint32_t AH_HUNT_MAX_MS  = 16000;  // AH-4 spec max 15 s
static const uint16_t AH_SETTLE_MS    = 100;    // watch for the failure blip after KEY releases
static const uint16_t AH_FAIL_WAIT_MS = 2000;   // give up waiting for KEY to release after a blip
static const uint16_t KEY_DEBOUNCE_MS = 10;     // the KEY wire picks up RF

// Types used in function signatures must come before the first function:
// the Arduino preprocessor inserts prototypes right after the includes.
enum Ah4State : uint8_t {
  AH_IDLE,
  AH_RESET_PULSE,   // 70 ms START pulse, AH-4 goes to bypass
  AH_WAIT_KEY,      // START low, waiting for the AH-4 to boot and assert KEY
  AH_KEYED,         // KEY seen, START held low for AH_HOLD_MS more
  AH_HUNT,          // START released, AH-4 hunting, waiting for KEY to release
  AH_SETTLE,        // KEY released: watch AH_SETTLE_MS for the failure blip
  AH_FAIL_WAIT      // failure blip seen: wait for KEY to release for good
};
static const char *const AH_NAMES[] = {
  "IDLE", "RESET_PULSE", "WAIT_KEY", "KEYED", "HUNT", "SETTLE", "FAIL_WAIT"
};

enum TuneResult : int8_t { RES_NONE = -1, RES_OK = 0, RES_FAIL = 1, RES_NOKEY = 2 };

// ---------------------------------------------------------------- logging
static void logTs() {
  Serial.print(micros());
  Serial.print(' ');
}
static void logHex(uint8_t b) {
  if (b < 0x10) Serial.print('0');
  Serial.print(b, HEX);
}
static void logLine(const __FlashStringHelper *tag, long value) {
  logTs();
  Serial.print(tag);
  Serial.print(' ');
  Serial.println(value);
}
static void logNote(const char *s) {
  logTs();
  Serial.print(F("NOTE "));
  Serial.println(s);
}

// ---------------------------------------------------------------- radio-side lines
static void sendToRadio(uint8_t b) {
  Serial1.write(b);
  logTs();
  Serial.print(F("TX "));
  logHex(b);
  Serial.println();
}

// TX INH: 0 released (input, radio pulls it low), 1 driven low (no effect, for
// experiments), 2 driven high = RF inhibited while the radio stays in TX.
static uint8_t inhLevel = 0;
static void txInh(uint8_t level) {
  if (level == inhLevel) return;
  inhLevel = level;
  if (level == 0) {
    pinMode(PIN_TXINH, INPUT);
  } else {
    pinMode(PIN_TXINH, OUTPUT);
    digitalWrite(PIN_TXINH, level == 2 ? HIGH : LOW);
  }
  logLine(F("INH"), level);
}

// Bytes to the radio scheduled for later (A1 after a delay, A2 after A1, ...).
struct Pending {
  uint32_t at;
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

// ---------------------------------------------------------------- AH-4 lines
// START goes through an N-MOSFET: pin HIGH = transistor on = START pulled low (asserted).
static bool startAsserted = false;
static void ah4Start(bool assert) {
  if (assert == startAsserted) return;
  startAsserted = assert;
  digitalWrite(PIN_START, assert ? HIGH : LOW);
  logLine(F("START"), assert ? 1 : 0);
}

static bool     keyAsserted  = false;   // debounced
static int      keyRawLast   = HIGH;
static uint32_t keyRawSince  = 0;

static void serviceKey() {
  int v = digitalRead(PIN_KEY);
  uint32_t now = millis();
  if (v != keyRawLast) {
    keyRawLast = v;
    keyRawSince = now;
  }
  bool asserted = (v == LOW);
  if (asserted != keyAsserted && now - keyRawSince >= KEY_DEBOUNCE_MS) {
    keyAsserted = asserted;
    logLine(F("KEY"), asserted ? 0 : 1);
  }
}

// ---------------------------------------------------------------- AH-4 tune cycle

static Ah4State   ahState     = AH_IDLE;
static uint32_t   ahStateAt   = 0;      // when the current state was entered
static uint32_t   cycleStart  = 0;      // when START went low for this cycle
static uint32_t   cycleKeyAt  = 0;      // when KEY asserted
static uint32_t   cycleF2At   = 0;      // when F2 arrived (0 = not yet)
static bool       resetSendsA1 = false; // RESET_PULSE triggered by F1 (classic mode): answer A1 after it
static TuneResult parkedResult = RES_NONE; // result finished before F2 arrived (startOnF1)

static void ahEnter(Ah4State st) {
  ahState = st;
  ahStateAt = millis();
  logTs();
  Serial.print(F("AH4 "));
  Serial.println(AH_NAMES[st]);
}

static void ah4Abort() {
  ah4Start(false);
  txInh(0);
  if (ahState != AH_IDLE) ahEnter(AH_IDLE);
}

static void ah4BeginCycle() {
  cycleStart = millis();
  cycleKeyAt = 0;
  cycleF2At = 0;
  parkedResult = RES_NONE;
  ah4Start(true);
  ahEnter(AH_WAIT_KEY);
}

// Summary line for the host, then tell the radio. If the AH-4 finished before
// F2 arrived (possible in startOnF1 mode), park the result until F2's A0 is out.
static void ah4Report(TuneResult res) {
  uint32_t now = millis();
  logTs();
  Serial.print(F("TUNE result="));
  Serial.print(res == RES_OK ? "ok" : res == RES_FAIL ? "fail" : "nokey");
  Serial.print(F(" key_ms="));
  Serial.print(cycleKeyAt ? (long)(cycleKeyAt - cycleStart) : -1L);
  Serial.print(F(" hunt_ms="));
  Serial.print(cycleKeyAt ? (long)(now - cycleKeyAt) : -1L);
  Serial.print(F(" f2_ms="));
  Serial.print(cycleF2At ? (long)(cycleF2At - cycleStart) : -1L);
  Serial.print(F(" total_ms="));
  Serial.println((long)(now - cycleStart));

  if (cfg.startOnF1 && !cycleF2At) {
    parkedResult = res;
    logNote("result parked until F2");
    return;
  }
  switch (res) {
    case RES_OK:   sendToRadio(0xA1); break;
    case RES_FAIL: sendToRadio(0xA1); schedule(now + 20, 0xA2); break;
    default:       sendToRadio(0xB0); schedule(now + 300, 0xA1); break;
  }
  parkedResult = RES_NONE;
}

static void serviceAh4() {
  if (ahState == AH_IDLE) return;
  uint32_t now = millis();
  uint32_t inState = now - ahStateAt;

  switch (ahState) {
    case AH_RESET_PULSE:
      if (inState >= AH_RESET_MS) {
        ah4Start(false);
        ahEnter(AH_IDLE);
        if (resetSendsA1) {
          resetSendsA1 = false;
          sendToRadio(0xA1);
        }
      }
      break;

    case AH_WAIT_KEY:
      if (keyAsserted) {
        cycleKeyAt = now;
        txInh(0);                       // classic mode: RF may reach the AH-4 now
        ahEnter(AH_KEYED);
      } else if (inState >= AH_KEY_TIMEOUT) {
        ah4Start(false);
        txInh(0);
        ahEnter(AH_IDLE);
        ah4Report(RES_NOKEY);
      }
      break;

    case AH_KEYED:
      if (inState >= AH_HOLD_MS) {
        ah4Start(false);
        ahEnter(AH_HUNT);
      }
      break;

    case AH_HUNT:
      if (!keyAsserted) {
        ahEnter(AH_SETTLE);
      } else if (now - cycleStart >= AH_HUNT_MAX_MS) {
        ah4Start(true);                 // reset pulse: force bypass
        ahEnter(AH_RESET_PULSE);
        ah4Report(RES_FAIL);
      }
      break;

    case AH_SETTLE:
      if (keyAsserted) {
        ahEnter(AH_FAIL_WAIT);
      } else if (inState >= AH_SETTLE_MS) {
        ahEnter(AH_IDLE);
        ah4Report(RES_OK);
      }
      break;

    case AH_FAIL_WAIT:
      if (!keyAsserted || inState >= AH_FAIL_WAIT_MS) {
        ahEnter(AH_IDLE);
        ah4Report(RES_FAIL);
      }
      break;

    default:
      ahEnter(AH_IDLE);
  }
}

// ---------------------------------------------------------------- radio commands
static void onF1() {
  // Tuner off, or the first step of a tune. In startOnF1 mode we cannot tell
  // the two apart, so we always start a cycle: without a carrier the AH-4
  // aborts into bypass by itself, which is what "tuner off" wants anyway.
  ah4Abort();
  if (cfg.startOnF1) {
    ah4BeginCycle();
    schedule(millis() + cfg.f1A1Delay, 0xA1);
  } else {
    ah4Start(true);
    resetSendsA1 = true;
    ahEnter(AH_RESET_PULSE);
  }
}

static void onF2() {
  if (cfg.startOnF1 && ahState != AH_IDLE) {
    cycleF2At = millis();               // cycle already running, just note it
    return;
  }
  if (cfg.startOnF1 && parkedResult != RES_NONE) {
    cycleF2At = millis();
    ah4Report(parkedResult);
    return;
  }
  // Classic: start now, keep RF off the AH-4 until it has booted and keyed.
  ah4Abort();
  txInh(2);
  ah4BeginCycle();
  cycleF2At = cycleStart;
}

static void onCommand(const uint8_t *f) {
  uint8_t op = f[1];
  logTs();
  Serial.print(F("CMD "));
  for (uint8_t i = 0; i < 4; i++) {
    logHex(f[i]);
    Serial.print(' ');
  }
  switch (op) {
    case 0xE0: Serial.print(F("init")); break;
    case 0xF0: Serial.print(F("enable/freq")); break;
    case 0xF1: Serial.print(F("bypass")); break;
    case 0xF2: Serial.print(F("TUNE")); break;
    default:   Serial.print(F("unknown")); break;
  }
  if (op == 0xF0 || op == 0xF2) {
    Serial.print(' ');
    Serial.print((f[2] >> 4) * 10 + (f[2] & 0x0F));
    Serial.print('.');
    Serial.print(f[3] >> 4);
    Serial.print(f[3] & 0x0F);
    Serial.print(F(" MHz"));
  }
  Serial.println();

  clearPending();
  sendToRadio(0xA0);                    // always, at once: after F2 the radio allows ~50 ms
  switch (op) {
    case 0xF1: onF1(); break;
    case 0xF2: onF2(); break;
    default:   schedule(millis() + 300, 0xA1); break;   // E0, F0: nothing to do for the AH-4
  }
}

// Frame: FF, ~50 ms gap, op, hi, lo. Resync on every FF.
static uint8_t  frame[4];
static uint8_t  frameLen = 0;
static uint32_t lastRxUs = 0;

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

    if (b == 0xFF) frameLen = 0;
    if (frameLen < 4) {
      frame[frameLen++] = b;
      if (frameLen == 4) {
        onCommand(frame);
        frameLen = 0;
      }
    }
  }
}

// ---------------------------------------------------------------- TX GND / RESET
static int lastTxGnd = -1, lastReset = -1;

static void serviceLines() {
  int v = digitalRead(PIN_TXGND);
  if (v != lastTxGnd) {
    lastTxGnd = v;
    logLine(F("TXGND"), v);
    digitalWrite(PIN_LED, v == LOW);
  }
  v = digitalRead(PIN_RESET);
  if (v != lastReset) {
    bool rose = (lastReset == LOW && v == HIGH);
    lastReset = v;
    logLine(F("RESET"), v);
    if (rose && cfg.bootSeq) {
      // Radio just registered the tuner: the FC-30 sends A0 and A1 before the
      // radio's E0 handshake.
      clearPending();
      schedule(millis() + 200, 0xA0);
      schedule(millis() + 1000, 0xA1);
      logNote("radio on: boot sequence scheduled");
    }
  }
}

// ---------------------------------------------------------------- power
static uint32_t lastPwrMs = 0;

static float adcVolts(uint8_t pin) {
  (void)analogRead(pin);                // settle the mux after a channel change
  return analogRead(pin) * (ADC_VREF / 1023.0f);
}
static float voltsToWatts(float v) {
  if (v < 0.02f) return 0;              // below the detector knee
  float s = v / cfg.calK + cfg.calC;
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
    swr = rho < 0.999f ? (1 + rho) / (1 - rho) : 99;
  }
  logTs();
  Serial.print(F("PWR "));
  Serial.print(vf, 4); Serial.print(' ');
  Serial.print(vr, 4); Serial.print(' ');
  Serial.print(pf, 2); Serial.print(' ');
  Serial.println(swr, 2);
}

// ---------------------------------------------------------------- host commands
static char     hostLine[64];
static uint8_t  hostLen = 0;
static uint32_t hostLastMs = 0;

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void printStatus() {
  logTs();
  Serial.print(F("STATUS f1start="));  Serial.print(cfg.startOnF1);
  Serial.print(F(" f1a1="));           Serial.print(cfg.f1A1Delay);
  Serial.print(F(" boot="));           Serial.print(cfg.bootSeq);
  Serial.print(F(" pwr="));            Serial.print(cfg.pwrMode);
  Serial.print('/');                   Serial.print(cfg.pwrPeriod);
  Serial.print(F(" cal="));            Serial.print(cfg.calK, 4);
  Serial.print('/');                   Serial.print(cfg.calC, 3);
  Serial.print(F(" state="));          Serial.print(AH_NAMES[ahState]);
  Serial.print(F(" txgnd="));          Serial.print(digitalRead(PIN_TXGND));
  Serial.print(F(" reset="));          Serial.print(digitalRead(PIN_RESET));
  Serial.print(F(" key="));            Serial.print(keyAsserted ? 0 : 1);
  Serial.print(F(" inh="));            Serial.println(digitalRead(PIN_TXINH));
}

static void handleHostLine(char *line) {
  char *cmd = strtok(line, " \r\n");
  if (!cmd) return;
  char *arg = strtok(nullptr, " \r\n");

  if (!strcmp(cmd, "?") || !strcmp(cmd, "status")) {
    printStatus();
  } else if (!strcmp(cmd, "f1start") && arg) {
    cfg.startOnF1 = atoi(arg);
    printStatus();
  } else if (!strcmp(cmd, "f1a1") && arg) {
    cfg.f1A1Delay = atoi(arg);
    printStatus();
  } else if (!strcmp(cmd, "boot") && arg) {
    cfg.bootSeq = atoi(arg);
    printStatus();
  } else if (!strcmp(cmd, "pwr") && arg) {          // pwr <0|1|2> [period_ms]
    cfg.pwrMode = atoi(arg);
    char *per = strtok(nullptr, " \r\n");
    if (per) cfg.pwrPeriod = atoi(per);
    printStatus();
  } else if (!strcmp(cmd, "cal") && arg) {          // cal <k> <c>
    char *c = strtok(nullptr, " \r\n");
    if (c) {
      cfg.calK = atof(arg);
      cfg.calC = atof(c);
    }
    printStatus();
  } else if (!strcmp(cmd, "adc")) {                 // one-shot raw reading
    logTs();
    Serial.print(F("ADC fwd=")); Serial.print(adcVolts(PIN_FWD), 4);
    Serial.print(F(" refl="));   Serial.println(adcVolts(PIN_REFL), 4);
  } else if (!strcmp(cmd, "inh") && arg) {          // inh 0|1|2 (released / low / high = RF off)
    txInh((uint8_t)atoi(arg));
  } else if (!strcmp(cmd, "start") && arg) {        // start 0|1: drive START by hand
    ah4Start(atoi(arg));
  } else if (!strcmp(cmd, "reset")) {               // 70 ms START pulse: AH-4 to bypass
    ah4Abort();
    ah4Start(true);
    ahEnter(AH_RESET_PULSE);
  } else if (!strcmp(cmd, "s") && arg) {            // s <hex bytes..>: raw bytes to the radio
    clearPending();
    while (arg) {
      if (strlen(arg) == 2 && hexVal(arg[0]) >= 0 && hexVal(arg[1]) >= 0) {
        sendToRadio((hexVal(arg[0]) << 4) | hexVal(arg[1]));
      }
      arg = strtok(nullptr, " \r\n");
    }
  } else if (!strcmp(cmd, "abort")) {
    clearPending();
    ah4Abort();
    logNote("aborted");
  } else if (!strcmp(cmd, "note")) {
    logTs();
    Serial.print(F("NOTE "));
    while (arg) { Serial.print(arg); Serial.print(' '); arg = strtok(nullptr, " \r\n"); }
    Serial.println();
  } else {
    logNote("commands: ? | f1start 0/1 | f1a1 <ms> | boot 0/1 | pwr 0/1/2 [ms] | cal <k> <c> | adc | inh 0/1/2 | start 0/1 | reset | s <hex..> | abort | note ...");
  }
}

static void serviceHost() {
  if (hostLen && millis() - hostLastMs > 1000) hostLen = 0;   // drop a stale partial line
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

// ---------------------------------------------------------------- setup / loop
void setup() {
  pinMode(PIN_TXGND, INPUT_PULLUP);
  pinMode(PIN_RESET, INPUT_PULLUP);    // the radio only drives it high once a tuner is registered
  pinMode(PIN_TXINH, INPUT);           // released
  pinMode(PIN_START, OUTPUT);
  digitalWrite(PIN_START, LOW);        // transistor off = START released
  pinMode(PIN_KEY, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  analogReference(INTERNAL);           // 2.56 V on the ATmega32U4

  Serial.begin(115200);
  Serial1.begin(4800);                 // 8N1

  uint32_t t = millis();
  while (!Serial && millis() - t < 3000) {}
  logNote("ah4_adapter ready. '?' for status.");
  printStatus();
}

void loop() {
  serviceRadioRx();
  serviceKey();
  serviceLines();
  servicePending();
  serviceAh4();
  servicePower();
  serviceHost();
}
