
/*
 * HomePilot AI — ESP32-S3 firmware (Arduino core for ESP32, v3.x)
 *
 * Role: "hands and eyes" only. It reads the sensors, reports them to the
 * SK-AM62, and switches LEDs/fan ONLY when told to. No automation rules live
 * here — the Python rule engine on the AM62 decides.
 *
 * TWO OUTPUT MODES
 *   Readable mode (default at power-on): a clear status panel every 2 s for
 *     the Arduino Serial Monitor, and simple typed commands:
 *       help | status | living on | kitchen off | bedroom on | all off | fan 50 | fan off
 *   JSON mode: one JSON object per line, for the AM62 Python app.
 *     The app switches this on automatically when it connects
 *     ({"cmd":"mode","json":true}). You can also type json or pretty.
 *
 * Libraries (Arduino Library Manager):
 *   - "DHT sensor library" by Adafruit (+ "Adafruit Unified Sensor")
 *   - "ArduinoJson" by Benoit Blanchon (v7)
 *
 * Board settings (Arduino IDE → Tools):
 *   Board: "ESP32S3 Dev Module"
 *   USB CDC On Boot: "Enabled"
 *   Serial Monitor: 115200 baud, line ending "Newline"
 *
 * JSON protocol
 *   ESP32 → AM62
 *     {"type":"telemetry","uptime_s":12,"temp_c":24.6,"humidity":41.2,
 *      "light":63,"motion":false,"leds":{"living":true,...},"fan":0,"dht_ok":true}
 *     {"type":"event","event":"motion","value":true}
 *     {"type":"ack","cmd":"set","ok":true}
 *   AM62 → ESP32
 *     {"cmd":"set","dev":"living","on":true}
 *     {"cmd":"all","on":false}
 *     {"cmd":"fan","duty":70}
 *     {"cmd":"ping"}
 *     {"cmd":"mode","json":true}
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DHT.h>

// ---------------- Pin map ----------------
constexpr int PIN_PIR = 4;   // PIR OUT (3.3 V logic)
constexpr int PIN_DHT = 5;   // DHT22 DATA, 10 kΩ pull-up to 3.3 V
constexpr int PIN_LDR = 6;   // LDR divider midpoint (ADC1_CH5)
constexpr int PIN_FAN = 15;  // MOSFET gate via 100 Ω

struct Led { const char *id; int pin; };
Led LEDS[] = {
  {"living", 10},
  {"kitchen", 11},
  {"bathroom", 12},
  // Add more rooms here and in am62/config.py LIGHTS.
};
constexpr int NUM_LEDS = sizeof(LEDS) / sizeof(LEDS[0]);

// ---------------- Settings ----------------
constexpr uint32_t TELEMETRY_MS = 2000;
constexpr uint32_t DHT_MS = 2500;
constexpr uint32_t FAN_PWM_HZ = 25000;
constexpr uint8_t FAN_PWM_BITS = 8;

// Soft start: ramp the fan up instead of switching it fully on at once.
constexpr uint32_t FAN_RAMP_MS = 20;
constexpr int FAN_RAMP_STEP = 3;
constexpr int DARK_BELOW = 30;
constexpr uint32_t HOST_TIMEOUT_MS = 60000;

// When the AM62 disappears, hold current outputs unless enabled.
constexpr bool FAILSAFE_ALL_OFF = false;

DHT dht(PIN_DHT, DHT22);

bool jsonMode = false;
bool ledOn[NUM_LEDS] = {};
uint8_t fanDuty = 0;
int fanNow = 0;
uint32_t lastFanStep = 0;
float tempC = NAN, humidity = NAN;
bool motion = false;

uint32_t lastDht = 0;
uint32_t lastTelemetry = 0;
uint32_t lastHostMsg = 0;

bool hostOk = false;
bool dhtTried = false;
String rxLine;

// ---------------- Outputs ----------------
void setLed(int i, bool on) {
  ledOn[i] = on;
  digitalWrite(LEDS[i].pin, on ? HIGH : LOW);
}

void writeFan(int pct) {
  fanNow = pct;
  ledcWrite(
    PIN_FAN,
    map(pct, 0, 100, 0, (1 << FAN_PWM_BITS) - 1)
  );
}

void setFan(int pct) {
  fanDuty = constrain(pct, 0, 100);
  if (fanDuty < fanNow) writeFan(fanDuty);
}

void rampFan(uint32_t now) {
  if (fanNow >= fanDuty || now - lastFanStep < FAN_RAMP_MS) return;

  lastFanStep = now;
  int next = fanNow + FAN_RAMP_STEP;
  writeFan(next > fanDuty ? fanDuty : next);
}

int findLed(const char *id) {
  for (int i = 0; i < NUM_LEDS; i++) {
    if (strcmp(LEDS[i].id, id) == 0) return i;
  }
  return -1;
}

// ---------------- Sensors ----------------
int readLightPercent() {
  uint32_t mv = 0;

  for (int i = 0; i < 8; i++) {
    mv += analogReadMilliVolts(PIN_LDR);
  }

  mv /= 8;

  return constrain((int)(mv * 100 / 3300), 0, 100);
}

// ---------------- Readable output ----------------
void printHelp() {
  Serial.println();
  Serial.println("=============== HomePilot commands ===============");
  Serial.println("  status              show sensor readings now");
  Serial.println("  living on / off     (also: kitchen, bedroom)");
  Serial.println("  all on / all off    every light");
  Serial.println("  fan 0..100          fan speed in %  (fan on / fan off)");
  Serial.println("  json / pretty       switch output format");
  Serial.println("==================================================");
}

void printStatus() {
  int light = readLightPercent();

  char bar[21];
  int n = light / 5;

  for (int i = 0; i < 20; i++) {
    bar[i] = i < n ? '#' : '.';
  }
  bar[20] = '\0';

  Serial.println();
  Serial.printf(
    "------------- HomePilot sensors  (running %lu s) -------------\n",
    (unsigned long)(millis() / 1000)
  );

  if (!dhtTried) {
    Serial.println(" Temperature : waiting for first reading...");
  } else if (isnan(tempC)) {
    Serial.println(" Temperature : ERROR - check DHT22 wiring and 10k pull-up");
  } else {
    Serial.printf(
      " Temperature : %.1f C        Humidity : %.1f %%\n",
      tempC,
      humidity
    );
  }

  Serial.printf(
    " Light       : %3d %%  [%s]  %s\n",
    light,
    bar,
    light < DARK_BELOW ? "dark" : (light < 70 ? "dim" : "bright")
  );

  Serial.println(
    digitalRead(PIN_PIR) == HIGH
      ? " Motion      : MOTION"
      : " Motion      : NO MOTION"
  );

  Serial.print(" Lights      : ");

  for (int i = 0; i < NUM_LEDS; i++) {
    Serial.printf(
      "%s %s%s",
      LEDS[i].id,
      ledOn[i] ? "ON" : "off",
      i < NUM_LEDS - 1 ? "  |  " : "\n"
    );
  }

  Serial.printf(
    " Fan         : %d %%%s\n",
    fanDuty,
    fanNow < fanDuty ? "  (ramping up)" : ""
  );

  Serial.println(" (type  help  for commands)");
}

// ---------------- JSON output (AM62) ----------------
void sendTelemetry() {
  lastTelemetry = millis();

  if (!jsonMode) {
    printStatus();
    return;
  }

  JsonDocument doc;

  doc["type"] = "telemetry";
  doc["uptime_s"] = millis() / 1000;

  if (isnan(tempC)) {
    doc["temp_c"] = nullptr;
  } else {
    doc["temp_c"] = roundf(tempC * 10) / 10;
  }

  if (isnan(humidity)) {
    doc["humidity"] = nullptr;
  } else {
    doc["humidity"] = roundf(humidity * 10) / 10;
  }

  doc["dht_ok"] = !isnan(tempC);
  doc["light"] = readLightPercent();
  doc["motion"] = motion;

  JsonObject leds = doc["leds"].to<JsonObject>();

  for (int i = 0; i < NUM_LEDS; i++) {
    leds[LEDS[i].id] = ledOn[i];
  }

  doc["fan"] = fanDuty;
  doc["host_ok"] = hostOk;

  serializeJson(doc, Serial);
  Serial.println();
}

// ---------------- Command replies ----------------
void reply(const char *cmd, bool ok, const String &msg) {
  if (jsonMode) {
    JsonDocument doc;

    doc["type"] = "ack";
    doc["cmd"] = cmd;
    doc["ok"] = ok;

    if (!ok) {
      doc["error"] = msg.c_str();
    }

    serializeJson(doc, Serial);
    Serial.println();
  } else {
    Serial.print(ok ? ">>> OK: " : ">>> ERROR: ");
    Serial.println(msg);
  }
}

void sendEvent(const char *name, bool value) {
  if (!jsonMode) {
    if (strcmp(name, "motion") == 0) {
      Serial.println(value ? "\n>>> MOTION" : "\n>>> NO MOTION");
    }
    return;
  }

  JsonDocument doc;

  doc["type"] = "event";
  doc["event"] = name;
  doc["value"] = value;

  serializeJson(doc, Serial);
  Serial.println();
}

// ---------------- JSON commands ----------------
void handleJson(const String &line) {
  JsonDocument doc;

  if (deserializeJson(doc, line)) {
    reply("?", false, "bad_json");
    return;
  }

  const char *cmd = doc["cmd"] | "";

  lastHostMsg = millis();
  hostOk = true;

  if (strcmp(cmd, "set") == 0) {
    const char *dev = doc["dev"] | "";
    int i = findLed(dev);

    if (i < 0 || !doc["on"].is<bool>()) {
      reply(cmd, false, "unknown_device_or_value");
      return;
    }

    setLed(i, doc["on"].as<bool>());

    reply(
      cmd,
      true,
      String(dev) + " light " + (ledOn[i] ? "ON" : "OFF")
    );

    sendTelemetry();

  } else if (strcmp(cmd, "all") == 0) {
    if (!doc["on"].is<bool>()) {
      reply(cmd, false, "missing_on");
      return;
    }

    for (int i = 0; i < NUM_LEDS; i++) {
      setLed(i, doc["on"].as<bool>());
    }

    reply(
      cmd,
      true,
      String("all lights ") + (doc["on"].as<bool>() ? "ON" : "OFF")
    );

    sendTelemetry();

  } else if (strcmp(cmd, "fan") == 0) {
    if (!doc["duty"].is<int>()) {
      reply(cmd, false, "missing_duty");
      return;
    }

    setFan(doc["duty"].as<int>());

    reply(cmd, true, String("fan set to ") + fanDuty + " %");
    sendTelemetry();

  } else if (strcmp(cmd, "mode") == 0) {
    jsonMode = doc["json"] | true;
    sendTelemetry();

  } else if (strcmp(cmd, "ping") == 0) {
    sendTelemetry();

  } else {
    reply(cmd, false, "unknown_cmd");
  }
}

// ---------------- Plain-text commands ----------------
void handleText(String s) {
  s.toLowerCase();

  if (s == "help" || s == "?") {
    printHelp();
    return;
  }

  if (s == "status") {
    printStatus();
    return;
  }

  if (s == "json") {
    jsonMode = true;
    sendTelemetry();
    return;
  }

  if (s == "pretty") {
    jsonMode = false;
    printStatus();
    return;
  }

  int sp = s.indexOf(' ');

  String what = sp < 0 ? s : s.substring(0, sp);
  String arg = sp < 0 ? String("") : s.substring(sp + 1);

  arg.trim();

  if (what == "fan") {
    int v;

    if (arg == "on") {
      v = 100;
    } else if (arg == "off") {
      v = 0;
    } else if (arg.length() && isDigit(arg[0])) {
      v = arg.toInt();
    } else {
      reply("fan", false, "use: fan 0..100, fan on, fan off");
      return;
    }

    setFan(v);
    reply("fan", true, String("fan set to ") + fanDuty + " %");
    return;
  }

  bool on;

  if (arg == "on") {
    on = true;
  } else if (arg == "off") {
    on = false;
  } else {
    reply("?", false, "unknown command '" + s + "' - type help");
    return;
  }

  if (what == "all") {
    for (int i = 0; i < NUM_LEDS; i++) {
      setLed(i, on);
    }

    reply("all", true, String("all lights ") + (on ? "ON" : "OFF"));
    return;
  }

  int i = findLed(what.c_str());

  if (i < 0) {
    reply("set", false, "no light called '" + what + "' - type help");
    return;
  }

  setLed(i, on);

  reply(
    "set",
    true,
    what + " light " + (on ? "ON" : "OFF")
  );
}

void handleLine(String line) {
  line.trim();

  if (!line.length()) return;

  if (line[0] == '{') {
    handleJson(line);
  } else {
    handleText(line);
  }
}

// ---------------- Arduino setup ----------------
void setup() {
  Serial.begin(115200);

  for (int i = 0; i < NUM_LEDS; i++) {
    pinMode(LEDS[i].pin, OUTPUT);
    setLed(i, false);
  }

  pinMode(PIN_PIR, INPUT);

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_LDR, ADC_11db);

  ledcAttach(PIN_FAN, FAN_PWM_HZ, FAN_PWM_BITS);

  setFan(0);
  dht.begin();
}

// ---------------- Arduino loop ----------------
void loop() {
  uint32_t now = millis();

  // Serial input
  while (Serial.available()) {
    char c = Serial.read();

    if (c == '\n') {
      handleLine(rxLine);
      rxLine = "";
    } else if (rxLine.length() < 512) {
      rxLine += c;
    }
  }

  // PIR: HIGH = MOTION, LOW = NO MOTION.
  // No software delay or debounce.
  bool raw = digitalRead(PIN_PIR) == HIGH;

  if (raw != motion) {
    motion = raw;
    sendEvent("motion", motion);
    sendTelemetry();
  }

  // DHT22
  if (now - lastDht >= DHT_MS) {
    lastDht = now;

    float t = dht.readTemperature();
    float h = dht.readHumidity();

    tempC = t;
    humidity = h;
    dhtTried = true;
  }

  // Host watchdog
  if (hostOk && now - lastHostMsg > HOST_TIMEOUT_MS) {
    hostOk = false;

    sendEvent("host_lost", true);

    if (FAILSAFE_ALL_OFF) {
      for (int i = 0; i < NUM_LEDS; i++) {
        setLed(i, false);
      }

      setFan(0);
    }
  }

  rampFan(now);

  if (now - lastTelemetry >= TELEMETRY_MS) {
    sendTelemetry();
  }
}
