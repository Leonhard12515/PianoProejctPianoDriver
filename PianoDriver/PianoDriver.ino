#include <Wire.h>

// Port 1 is the lowest octave. Bit 0 enables port 1, bit 7 enables port 8.
constexpr uint8_t MUX_CHANNEL[8] = {0, 1, 2, 3, 4, 5, 6, 7};
constexpr uint8_t POPULATED_PORTS = 0b00000011;
constexpr uint8_t MUX_ADDRESS = 0x70;
constexpr uint8_t PCA_ADDRESS = 0x40;  // Same address on each populated branch.
constexpr uint16_t PWM_HZ = 1000;
constexpr uint16_t MAX_HIT_MS = 100;   // Bench setting. Match to the coil rating.

constexpr uint32_t PWM_DIVISOR = 4096UL * PWM_HZ;
constexpr uint32_t PRESCALE = (25000000UL + PWM_DIVISOR / 2) / PWM_DIVISOR - 1;
static_assert(PRESCALE >= 3 && PRESCALE <= 255, "PWM_HZ outside PCA9685 range");
constexpr uint8_t LED0_ON_L = 0x06, ALL_LED_OFF_H = 0xFD, FULL = 0x10;
bool ready[8], needsOff[8];
uint16_t active[8];         // One bit per physical output.
uint32_t deadline[8][16];   // Indexed by zero-based port and physical output.

bool writeRegisters(uint8_t p, uint8_t reg, const uint8_t *data, uint8_t count) {
  Wire.clearWireTimeoutFlag();
  Wire.beginTransmission(MUX_ADDRESS);
  Wire.write(1U << MUX_CHANNEL[p]);
  if (Wire.endTransmission() != 0 || Wire.getWireTimeoutFlag()) return false;

  Wire.beginTransmission(PCA_ADDRESS);
  Wire.write(reg);
  Wire.write(data, count);
  return Wire.endTransmission() == 0 && !Wire.getWireTimeoutFlag();
}

bool writeRegister(uint8_t p, uint8_t reg, uint8_t value) {
  return writeRegisters(p, reg, &value, 1);
}

bool stopPort(uint8_t p) {
  needsOff[p] = !writeRegister(p, ALL_LED_OFF_H, FULL);
  if (!needsOff[p]) active[p] = 0;
  return !needsOff[p];
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  Wire.setClock(400000);
  Wire.setWireTimeout(3000, true);
  for (uint8_t p = 0; p < 8; ++p) {
    if (!(POPULATED_PORTS & (1U << p))) continue;
    ready[p] = stopPort(p)
        && writeRegister(p, 0x00, 0x30)  // MODE1: sleep, auto-increment, All Call off
        && writeRegister(p, 0xFE, PRESCALE)
        && writeRegister(p, 0x01, 0x04)  // MODE2: active-high, push-pull, latch on STOP
        && writeRegister(p, 0x00, 0x20); // Wake with auto-increment enabled.
  }
  delayMicroseconds(500);  // Oscillator startup, before accepting commands.
}

void serviceTimers() {
  static uint32_t lastScan, lastRetry;
  const uint32_t now = millis();
  if (now == lastScan) return;
  lastScan = now;
  const bool retry = now - lastRetry >= 10;
  if (retry) lastRetry = now;

  for (uint8_t p = 0; p < 8; ++p) {
    if (needsOff[p]) {
      if (retry) stopPort(p);
      continue;
    }
    if (!active[p]) continue;
    for (uint8_t output = 0; output < 16; ++output) {
      const uint16_t bit = 1U << output;
      if (!(active[p] & bit) || int32_t(now - deadline[p][output]) < 0) continue;
      if (!writeRegister(p, LED0_ON_L + 4 * output + 3, FULL)) {
        needsOff[p] = true;  // Uncertain state. Retry a board-wide off.
        break;
      }
      active[p] &= ~bit;
    }
  }
}

bool startHit(uint8_t p, uint8_t output, uint16_t duration, uint8_t duty) {
  const uint16_t bit = 1U << output;
  if (!ready[p] || needsOff[p] || (active[p] & bit)) return false;
  const uint16_t ticks = (4096UL * duty + 50) / 100;
  uint8_t pwm[4] = {0, 0, uint8_t(ticks), uint8_t(ticks >> 8)};
  if (duty == 100) {  // PCA9685 has a dedicated full-on bit in ON_H.
    pwm[1] = FULL;
    pwm[3] = 0;
  }
  if (!writeRegisters(p, LED0_ON_L + 4 * output, pwm, sizeof(pwm))) {
    needsOff[p] = true;
    return false;
  }
  deadline[p][output] = millis() + duration;
  active[p] |= bit;
  return true;
}

// strtok is scoped to one complete line. Reject signs, junk and overflow.
bool nextNumber(uint16_t maximum, uint16_t &value) {
  const char *token = strtok(NULL, " \t");
  if (!token) return false;
  uint32_t n = 0;
  while (*token) {
    if (*token < '0' || *token > '9') return false;
    n = n * 10 + (*token++ - '0');
    if (n > maximum) return false;
  }
  value = n;
  return true;
}

bool executeLine(char *line) {
  const char *command = strtok(line, " \t");
  if (!command) return false;
  if (strcmp(command, "STOP") == 0) {
    if (strtok(NULL, " \t")) return false;
    bool ok = true;
    for (uint8_t p = 0; p < 8; ++p) {
      if ((POPULATED_PORTS & (1U << p)) && !stopPort(p)) ok = false;
    }
    return ok;
  }
  uint16_t port, output, duration, duty;
  if (strcmp(command, "HIT") != 0 || !nextNumber(8, port) || port == 0
      || !nextNumber(15, output) || !nextNumber(MAX_HIT_MS, duration) || duration == 0
      || !nextNumber(100, duty) || duty == 0 || strtok(NULL, " \t")) return false;
  return startHit(port - 1, output, duration, duty);
}

void loop() {
  static char line[48];
  static uint8_t used;
  static bool invalid;
  serviceTimers();
  if (!Serial.available() || Serial.availableForWrite() < 5) return;
  const char ch = Serial.read();
  if (ch == '\n') {
    if (used && line[used - 1] == '\r') --used;  // Also accept CRLF.
    line[used] = '\0';
    Serial.println(!invalid && executeLine(line) ? F("OK") : F("ERR"));
    used = 0;
    invalid = false;
  } else if (ch == '\0' || used == sizeof(line) - 1) {
    invalid = true;  // Discard through newline, including any valid-looking suffix.
  } else if (!invalid) {
    line[used++] = ch;
  }
}
