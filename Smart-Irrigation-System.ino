/* Irrigation controller
   - Short press mode button -> select mode
   - Long press mode button (~1500ms) -> enter Edit Mode for that mode
   - In Edit Mode:
       * Short press same mode button -> +100 ml
       * Short press ACT button -> -100 ml
       * Long press ACT button (~1500ms) -> save & exit Edit Mode
   - Press ACT (short) while NOT editing -> start pump for selected mode
   - Volumes persist in EEPROM
   - WiFiManager first-time setup; auto reconnect; web UI available after WiFi connects
*/

#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>

// ---------- CONFIG ----------
#define BTN_MODE1   D3   // careful: D3 = GPIO0 (must NOT be held LOW at boot)
#define BTN_MODE2   D4   // D4 = GPIO2 (should be HIGH at boot)
#define BTN_MODE3   D7
#define BTN_MODE4   D1
#define BTN_ACT     D5
#define PUMP_PIN    D6

// Set according to your relay module polarity:
// true  -> relay is active LOW (drive LOW to turn pump ON).
// false -> relay is active HIGH (drive HIGH to turn pump ON).
#define RELAY_ACTIVE_LOW true

const unsigned long LONG_PRESS_MS = 1500;   // long-press threshold (ms)
const unsigned long DEBOUNCE_MS   = 40;     // debounce time (ms)
const int INCREMENT = 100;                  // ml step when editing
const int MAX_VOLUME = 5000;
const int MIN_VOLUME = 100;

// EEPROM layout: store 4 ints starting at addr 0 (int is 4 bytes)
#define EEPROM_SIZE 64

// ---------- Globals ----------
LiquidCrystal_I2C lcd(0x27, 16, 2);
ESP8266WebServer server(80);

int modePins[4] = { BTN_MODE1, BTN_MODE2, BTN_MODE3, BTN_MODE4 };
int volumes[5]; // volumes[1..4] valid
const char* stageNames[] = {"", "Seedling", "Vegetative", "Flowering", "Fruit"};

bool editModeActive = false;
int editingMode = 0;

int selectedMode = 0;
bool pumpRunning = false;
unsigned long startTime = 0;
unsigned long runDuration = 0;
const float flowRate = 250.0; // mL/s used to compute runDuration

// per-button debounce & timing state
int lastReading[4];
int stableState[4];
unsigned long lastDebounceTime[4];
unsigned long pressStartTime[4];

// ACT button states
int lastReadingAct = HIGH;
int stableStateAct = HIGH;
unsigned long lastDebounceAct = 0;
unsigned long pressStartAct = 0;

bool wifiConnected = false;
bool serverStarted = false;
unsigned long lastReconnectAttempt = 0;

// ---------- EEPROM ----------
void saveVolumes() {
  for (int i = 1; i <= 4; ++i) {
    int addr = (i - 1) * sizeof(int);
    EEPROM.put(addr, volumes[i]);
  }
  EEPROM.commit();
  Serial.println("💾 Volumes saved to EEPROM");
}

void loadVolumes() {
  for (int i = 1; i <= 4; ++i) {
    int addr = (i - 1) * sizeof(int);
    EEPROM.get(addr, volumes[i]);
    if (volumes[i] < MIN_VOLUME || volumes[i] > 6000) { // invalid -> restore defaults
      if (i == 1) volumes[i] = 700;
      if (i == 2) volumes[i] = 1500;
      if (i == 3) volumes[i] = 3000;
      if (i == 4) volumes[i] = 5000;
    }
  }
  Serial.println("✅ Volumes loaded from EEPROM:");
  for (int i = 1; i <= 4; ++i) Serial.printf("  Mode %d = %d ml\n", i, volumes[i]);
}

// ---------- LCD ----------
void showHome() {
  lcd.clear();
  lcd.setCursor(1, 0);
  lcd.print("Irrigation Manual");
  lcd.setCursor(0, 1);
  if (wifiConnected) lcd.print("WiFi OK   ");
  else lcd.print("No WiFi   ");
}

// ---------- Web Handlers ----------
void handleRoot() {
  String page = "<!doctype html><html><head><meta charset='utf-8'><title>Irrigation</title></head><body>";
  page += "<h2>Irrigation Control</h2>";
  page += "<p>Selected Mode: " + String(selectedMode) + "</p>";
  for (int i = 1; i <= 4; ++i) {
    page += "<div>Mode " + String(i) + " (" + String(stageNames[i]) + "): " + String(volumes[i]) + " ml ";
    page += "<a href='/select?mode=" + String(i) + "'>Select</a> ";
    page += "<a href='/inc?mode=" + String(i) + "'>+100ml</a> ";
    page += "<a href='/dec?mode=" + String(i) + "'>-100ml</a></div>";
  }
  page += "<p><a href='/activate'>Activate Pump</a></p>";
  page += "</body></html>";
  server.send(200, "text/html", page);
}

void handleSelect() {
  if (server.hasArg("mode")) {
    int m = server.arg("mode").toInt();
    if (m >= 1 && m <= 4) {
      selectedMode = m;
      Serial.printf("WEB: Selected Mode %d | %s | %dml\n", m, stageNames[m], volumes[m]);
    }
  }
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleInc() {
  if (server.hasArg("mode")) {
    int m = server.arg("mode").toInt();
    if (m >= 1 && m <= 4) {
      volumes[m] += INCREMENT;
      if (volumes[m] > MAX_VOLUME) volumes[m] = MIN_VOLUME;
      saveVolumes();
      Serial.printf("WEB: Mode %d increased to %d ml\n", m, volumes[m]);
    }
  }
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleDec() {
  if (server.hasArg("mode")) {
    int m = server.arg("mode").toInt();
    if (m >= 1 && m <= 4) {
      volumes[m] -= INCREMENT;
      if (volumes[m] < MIN_VOLUME) volumes[m] = MAX_VOLUME;
      saveVolumes();
      Serial.printf("WEB: Mode %d decreased to %d ml\n", m, volumes[m]);
    }
  }
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleActivate() {
  if (selectedMode > 0 && !editModeActive) {
    // start pump
    float secs = (float)volumes[selectedMode] / flowRate;
    runDuration = (unsigned long)(secs * 1000.0);
    startTime = millis();
    pumpRunning = true;
    if (RELAY_ACTIVE_LOW) digitalWrite(PUMP_PIN, LOW); else digitalWrite(PUMP_PIN, HIGH);
    lcd.clear(); lcd.setCursor(0,0); lcd.printf("Mode %d Running", selectedMode);
    lcd.setCursor(0,1); lcd.print("Pump ON");
    Serial.printf("WEB: Pump STARTED | Mode %d | %d ml | %.1f s\n", selectedMode, volumes[selectedMode], runDuration/1000.0);
  }
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

// ---------- UI helpers ----------
void showModeScreen(int mode) {
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Mode ");
  lcd.print(mode);
  lcd.print(": ");
  lcd.print(stageNames[mode]);
  lcd.setCursor(0,1);
  lcd.print("Vol ");
  lcd.print(volumes[mode]);
  lcd.print("ml");
}

void showEditScreen(int mode) {
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Edit Mode ");
  lcd.print(mode);
  lcd.setCursor(0,1);
  lcd.print("Vol ");
  lcd.print(volumes[mode]);
  lcd.print("ml");
}

// ---------- Button actions ----------
void handleShortPressMode(int mode) {
  if (editModeActive) {
    if (editingMode == mode) {
      // increase
      volumes[mode] += INCREMENT;
      if (volumes[mode] > MAX_VOLUME) volumes[mode] = MIN_VOLUME;
      showEditScreen(mode);
      Serial.printf("Edit: Mode %d increased -> %d ml\n", mode, volumes[mode]);
    } else {
      // ignore pressing other mode while editing
    }
  } else {
    // normal select
    selectedMode = mode;
    showModeScreen(mode);
    Serial.printf("Selected Mode %d | %s | %d ml\n", mode, stageNames[mode], volumes[mode]);
  }
}

void handleLongPressMode(int mode) {
  // enter edit mode for this mode
  editModeActive = true;
  editingMode = mode;
  showEditScreen(mode);
  Serial.printf("Entered Edit Mode for %d\n", mode);
}

void handleShortPressAct() {
  if (editModeActive) {
    // decrease editing value
    volumes[editingMode] -= INCREMENT;
    if (volumes[editingMode] < MIN_VOLUME) volumes[editingMode] = MAX_VOLUME;
    showEditScreen(editingMode);
    Serial.printf("Edit: Mode %d decreased -> %d ml\n", editingMode, volumes[editingMode]);
  } else {
    // activate pump if a mode selected
    if (selectedMode > 0) {
      float secs = (float)volumes[selectedMode] / flowRate;
      runDuration = (unsigned long)(secs * 1000.0);
      startTime = millis();
      pumpRunning = true;
      if (RELAY_ACTIVE_LOW)
       digitalWrite(PUMP_PIN, LOW); 
      else digitalWrite(PUMP_PIN, HIGH);
      lcd.clear(); 
      lcd.setCursor(0,0); 
      lcd.printf("Mode %d Running", selectedMode);
      lcd.setCursor(0,1); 
      lcd.print("Pump ON");
      Serial.printf("Pump STARTED | Mode %d | %d ml | %.1f s\n", selectedMode, volumes[selectedMode], runDuration/1000.0);
    } else {
      Serial.println("ACT pressed but no mode selected");
    }
  }
}

void handleLongPressAct() {
  if (editModeActive) {
    // Save and exit edit mode
    saveVolumes();
    editModeActive = false;
    editingMode = 0;
    showHome();
    Serial.println("Exited Edit Mode (saved)");
  } else {
    // not editing - we could implement other long-press ACT behavior if desired
  }
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("==== Irrigation controller boot ====");

  // EEPROM
  EEPROM.begin(EEPROM_SIZE);
  loadVolumes();

  // LCD
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0,0);
  lcd.print("Checking WiFi...");

  // Buttons init
  for (int i = 0; i < 4; ++i) {
    pinMode(modePins[i], INPUT_PULLUP);
    lastReading[i] = digitalRead(modePins[i]);
    stableState[i] = lastReading[i];
    lastDebounceTime[i] = millis();
    pressStartTime[i] = 0;
  }
  pinMode(BTN_ACT, INPUT_PULLUP);
  lastReadingAct = digitalRead(BTN_ACT);
  stableStateAct = lastReadingAct;
  lastDebounceAct = millis();

  // Pump pin initial OFF
  pinMode(PUMP_PIN, OUTPUT);
  if (RELAY_ACTIVE_LOW) digitalWrite(PUMP_PIN, HIGH); else digitalWrite(PUMP_PIN, LOW);

  // WiFiManager (first time config)
  WiFiManager wm;
  wm.setConfigPortalTimeout(30); // seconds for config portal
  if (wm.autoConnect("TomatoIrrigation-Setup")) {
    wifiConnected = true;
    Serial.println("✅ WiFi connected");
    Serial.print("IP: "); Serial.println(WiFi.localIP());
  } else {
    wifiConnected = false;
    Serial.println("⚠ WiFi not connected (manual only)");
  }

  // Web endpoints
  server.on("/", handleRoot);
  server.on("/select", handleSelect);
  server.on("/inc", handleInc);
  server.on("/dec", handleDec);
  server.on("/activate", handleActivate);

  // Start server if already connected
  if (WiFi.status() == WL_CONNECTED) {
    server.begin();
    serverStarted = true;
    Serial.println("🌐 Web server started");
  }

  showHome();
}

// ---------- Loop ----------
void loop() {
  unsigned long now = millis();

  // 1) Read & debounce mode buttons
  for (int i = 0; i < 4; ++i) {
    int reading = digitalRead(modePins[i]);
    if (reading != lastReading[i]) {
      lastDebounceTime[i] = now;
      lastReading[i] = reading;
    }
    if ((now - lastDebounceTime[i]) > DEBOUNCE_MS) {
      if (reading != stableState[i]) {
        stableState[i] = reading;
        if (stableState[i] == LOW) {
          // pressed
          pressStartTime[i] = now;
        } else {
          // released
          unsigned long duration = now - pressStartTime[i];
          if (duration >= LONG_PRESS_MS) {
            handleLongPressMode(i + 1);
          } else {
            handleShortPressMode(i + 1);
          }
          pressStartTime[i] = 0;
        }
      }
    }
  }

  // 2) ACT button debounce/time
  int readingAct = digitalRead(BTN_ACT);
  if (readingAct != lastReadingAct) {
    lastDebounceAct = now;
    lastReadingAct = readingAct;
  }
  if ((now - lastDebounceAct) > DEBOUNCE_MS) {
    if (readingAct != stableStateAct) {
      stableStateAct = readingAct;
      if (stableStateAct == LOW) {
        // pressed
        pressStartAct = now;
      } else {
        // released
        unsigned long duration = now - pressStartAct;
        if (duration >= LONG_PRESS_MS) {
          handleLongPressAct();
        } else {
          handleShortPressAct();
        }
        pressStartAct = 0;
      }
    }
  }

  // 3) Pump timing
  if (pumpRunning) {
    unsigned long elapsed = now - startTime;
    if (elapsed >= runDuration) {
      // stop pump
      if (RELAY_ACTIVE_LOW) digitalWrite(PUMP_PIN, HIGH); else digitalWrite(PUMP_PIN, LOW);
      pumpRunning = false;
      lcd.clear();
      lcd.setCursor(0,0);
      lcd.print("Mode ");
      lcd.print(selectedMode);
      lcd.print(" Done");
      lcd.setCursor(0,1);
      lcd.print("Pump OFF");
      Serial.printf("Pump STOPPED | Mode %d | %d ml\n", selectedMode, volumes[selectedMode]);
      delay(1200);
      showHome();
    } else {
      unsigned long remaining = (runDuration - elapsed) / 1000;
      lcd.setCursor(0,1);
      lcd.print("Left ");
      lcd.print(remaining);
      lcd.print("s Pump ON ");
    }
  }

  // 4) WiFi reconnect without blocking manual operation
  if (WiFi.status() != WL_CONNECTED && (now - lastReconnectAttempt > 5000)) {
    lastReconnectAttempt = now;
    Serial.println("Trying to reconnect WiFi...");
    WiFi.begin(); // uses saved creds from WiFiManager
  }

  if (WiFi.status() == WL_CONNECTED && !serverStarted) {
    server.begin();
    serverStarted = true;
    wifiConnected = true;
    Serial.println("🌐 Web server started (after reconnect)");
    Serial.print("IP: "); Serial.println(WiFi.localIP());
    showHome();
  } else if (WiFi.status() != WL_CONNECTED && serverStarted) {
    serverStarted = false;
    wifiConnected = false;
    Serial.println("⚠ WiFi lost");
    showHome();
  }

  if (serverStarted) server.handleClient();

  delay(10); // small yield
}
