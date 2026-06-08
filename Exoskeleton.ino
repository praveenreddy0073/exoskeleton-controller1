#include <Wire.h>
#include <ESP32Servo.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>

// --- Pin Definitions ---
const int SERVO_PIN          = 18;
const int I2C_SDA            = 21;
const int I2C_SCL            = 22;
const int BUTTON_PIN         = 4;
const int RED_LED_BUZZER_PIN = 19;
const int BLUE_LED_PIN       = 25;
const int GREEN_LED_PIN      = 26;
const int BATTERY_PIN        = 34;

// --- Battery Calibration ---
float BATTERY_CALIBRATION = 3.0;  // Confirmed by ADC: pin 2.51V × 3.0 = 7.53V

// --- BLE Nordic UART Service (NUS) UUIDs ---
#define NUS_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID       "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID       "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

BLEServer*         pServer          = nullptr;
BLECharacteristic* pTxCharacteristic = nullptr;
bool bleConnected   = false;
bool bleAdvertising = false;

// --- OLED Config ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Preferences preferences;

// --- Objects ---
Servo kneeServo;
byte activeMpuAddress = 0;

// --- State ---
enum Mode { AUTO_MODE, MANUAL_MODE };
Mode currentMode = AUTO_MODE;

unsigned long buttonPressTime = 0;
bool buttonPressed     = false;
bool longPressHandled  = false;

const unsigned long LONG_PRESS_TIME = 2000; // Increased to 2s to prevent ghosting
const unsigned long DEBOUNCE_TIME   = 50;

int servoRelaxedAngle  = 0;
int servoAssistAngle   = 90;
bool manualServoActive = false;
float currentServoAngle  = 0;
float targetServoAngle   = 0;
bool lowBatteryAlert     = false;
float globalBatteryVoltage = 0.0;

int servoSpeedDelay     = 10;   // ms per degree (lower = faster)
float currentPitch      = 0.0;
float tiltUpThreshold   = 32.0;
float tiltDownThreshold = 28.0;

// --- Filter Variables ---
float gyroBias          = 0.0;  // To be calibrated if needed
unsigned long lastFiltTime = 0;
float filteredAngle     = 0.0;
const float alpha       = 0.96; // Complementary filter constant

// --- Smart Sit/Stand State ---
enum AutoState { STATE_IDLE, STATE_WALKING, STATE_SITTING };
AutoState currentAutoState = STATE_IDLE;
unsigned long sitTimer = 0;
bool standUpBoost = false;

// --- Forward Declarations ---
void beep(int duration);
void updateModeLEDs();
void sendBLEStatus();
void handleBLECommand(String cmd);
void updateDisplay();

// =========================================================
// BLE Callbacks
// =========================================================
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    bleConnected = true;
    Serial.println("BLE: Phone connected!");
    beep(100);
  }
  void onDisconnect(BLEServer* pServer) override {
    bleConnected = false;
    Serial.println("BLE: Phone disconnected.");
    // Only restart advertising in MANUAL_MODE to prevent
    // unwanted reconnect flicker when in AUTO_MODE
    if (currentMode == MANUAL_MODE) {
      delay(500); // Short delay before restarting to avoid rapid connect/disconnect loop
      BLEDevice::startAdvertising();
      Serial.println("BLE: Advertising restarted.");
    }
  }
};

class MyRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    String val = String(pChar->getValue().c_str());
    if (val.length() > 0) {
      handleBLECommand(val);
    }
  }
};

// =========================================================
// SETUP
// =========================================================
void setup() {
  Serial.begin(115200);

  // 1. LOAD SETTINGS FIRST
  preferences.begin("exo-settings", false);
  currentMode = (Mode)preferences.getInt("mode", AUTO_MODE);
  servoSpeedDelay = preferences.getInt("speed", 10);
  Serial.print(F("Booting... Loaded Mode: ")); Serial.println(currentMode == AUTO_MODE ? "AUTO" : "MANUAL");

  // 2. Pins
  pinMode(BUTTON_PIN,         INPUT_PULLUP);
  pinMode(RED_LED_BUZZER_PIN, OUTPUT);
  pinMode(BLUE_LED_PIN,       OUTPUT);
  pinMode(GREEN_LED_PIN,      OUTPUT);

  digitalWrite(RED_LED_BUZZER_PIN, LOW);
  
  // Hardware Test: Blink Red 3 times at boot
  Serial.println(F("Testing Alarm Hardware..."));
  for(int i=0; i<3; i++) {
    digitalWrite(RED_LED_BUZZER_PIN, HIGH); delay(100);
    digitalWrite(RED_LED_BUZZER_PIN, LOW);  delay(100);
  }
  
  updateModeLEDs();

  // Servo
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  kneeServo.setPeriodHertz(50);
  kneeServo.attach(SERVO_PIN, 500, 2400);
  kneeServo.write(currentServoAngle);

  // --- I2C BUS RECOVERY & SCAN ---
  Serial.println(F("Initializing I2C..."));
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000); 
  delay(1000); // Wait for sensors to settle
  
  byte oledAddr = 0;
  Serial.println(F("Starting I2C Scan..."));
  
  for (byte address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    byte error = Wire.endTransmission();
    if (error == 0) {
      Serial.print(F("Device Found: 0x")); Serial.println(address, HEX);
      if (address == 0x68 || address == 0x69) activeMpuAddress = address;
      if (address == 0x3C || address == 0x3D) oledAddr = address;
    }
    delay(5); // Small delay between scans
  }
  Serial.println(F("Scan Complete."));

  if (activeMpuAddress == 0) {
    Serial.println(F("WARNING: MPU6050 not found. Checking connections..."));
  } else {
    Serial.print(F("Waking MPU6050 at 0x")); Serial.println(activeMpuAddress, HEX);
    Wire.beginTransmission(activeMpuAddress);
    Wire.write(0x6B); Wire.write(0);
    if (Wire.endTransmission() != 0) {
      Serial.println(F("ERROR: Failed to wake MPU6050"));
    }
  }
  Wire.beginTransmission(activeMpuAddress);
  Wire.write(0x6B); Wire.write(0);
  if (Wire.endTransmission() != 0) {
    while (1) {
      Serial.println("CRITICAL ERROR: Failed to wake MPU6050!");
      digitalWrite(RED_LED_BUZZER_PIN, HIGH); delay(500);
      digitalWrite(RED_LED_BUZZER_PIN, LOW);  delay(500);
    }
  }
  Serial.println("MPU6050 Initialized!");

  // --- OLED Init ---
  byte finalOledAddr = oledAddr ? oledAddr : 0x3C; 
  Serial.print("Attempting OLED Init at 0x"); Serial.println(finalOledAddr, HEX);
  
  if(!display.begin(SSD1306_SWITCHCAPVCC, finalOledAddr)) {
    Serial.println(F("CRITICAL: SSD1306 Init Failed! Check wiring/power."));
  } else {
    Serial.println(F("OLED Init Success!"));
    display.clearDisplay();
    display.display(); // Show empty buffer
    delay(100);
    
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(20, 10);
    display.println(F("EXOSKELETON"));
    display.println(F("EXOSKELETON"));
    display.setCursor(20, 25);
    display.setTextSize(3);
    display.println(F("READY"));
    display.display();
    delay(1000);
    Wire.setClock(400000); 
  }

  // --- BLE Init ---
  BLEDevice::init("Exoskeleton");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService* pService = pServer->createService(NUS_SERVICE_UUID);

  pTxCharacteristic = pService->createCharacteristic(
    NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic* pRxChar = pService->createCharacteristic(
    NUS_RX_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pRxChar->setCallbacks(new MyRxCallbacks());

  pService->start();

  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(NUS_SERVICE_UUID);
  pAdv->setScanResponse(true);
  
  if (currentMode == MANUAL_MODE) {
    BLEDevice::startAdvertising();
    bleAdvertising = true;
    Serial.println(F("BLE: Manual Mode detected, Advertising Started."));
  } else {
    bleAdvertising = false;
    Serial.println(F("BLE: Auto Mode detected, Bluetooth is OFF."));
  }

  delay(100);
  beep(200);

  // Button wiring check
  if (digitalRead(BUTTON_PIN) == LOW) {
    while (1) {
      Serial.println("HARDWARE ERROR: Button permanently shorted to GND! Rotate 90deg.");
      digitalWrite(RED_LED_BUZZER_PIN, HIGH); delay(100);
      digitalWrite(RED_LED_BUZZER_PIN, LOW);  delay(100);
    }
  }

  Serial.println("System Ready. Currently in AUTO mode.");
  lastFiltTime = micros();
}

// =========================================================
// LOOP
// =========================================================
void loop() {
  handleButton();
  checkBattery();

  // Low Battery Alarm (Non-blocking Blink/Beep)
  if (lowBatteryAlert) {
    static unsigned long lastAlarm = 0;
    if (millis() - lastAlarm > 1000) {
      lastAlarm = millis();
      digitalWrite(RED_LED_BUZZER_PIN, HIGH);
      delay(50); // Short chirp/flash
      digitalWrite(RED_LED_BUZZER_PIN, LOW);
    }
  }

  if (currentMode == AUTO_MODE) {
    handleAutoMode();
  } else {
    handleManualMode();
  }

  updateServo();
  updateDisplay();
}

// =========================================================
// SERVO UPDATER (Non-blocking Sweep)
// =========================================================
void updateServo() {
  if (abs(currentServoAngle - targetServoAngle) < 0.05) return;

  int currentSpeedDelay = servoSpeedDelay;
  if (currentMode == AUTO_MODE && standUpBoost) {
    currentSpeedDelay = 2; // Temporary ultra-fast speed for lift
  }

  if (currentSpeedDelay <= 0) {
    // Instant snap
    currentServoAngle = targetServoAngle;
    int us = 500 + (currentServoAngle * (2400.0 - 500.0) / 180.0);
    kneeServo.writeMicroseconds(us);
    return;
  }

  static unsigned long lastUpdate = 0;
  unsigned long now = micros();
  
  // Initialize or handle long gaps (e.g. after mode switch)
  if (lastUpdate == 0 || (now - lastUpdate) > 100000) {
    lastUpdate = now;
    return;
  }

  float dt = (now - lastUpdate) / 1000000.0;
  lastUpdate = now;

  // Accurate Speed: Degrees per second = 1000 / delay_ms
  // Example: 10ms delay = 100 deg/sec
  float degPerSec = 1000.0 / (float)currentSpeedDelay;
  float maxStep = degPerSec * dt;

  float diff = targetServoAngle - currentServoAngle;
  if (abs(diff) <= maxStep) {
    currentServoAngle = targetServoAngle;
  } else {
    if (diff > 0) currentServoAngle += maxStep;
    else currentServoAngle -= maxStep;
  }

  // Write to hardware with microsecond (sub-degree) precision for smooth movement
  int targetUs = 500 + (currentServoAngle * (2400.0 - 500.0) / 180.0);
  static int lastWrittenUs = -1;
  if (targetUs != lastWrittenUs) {
    kneeServo.writeMicroseconds(targetUs);
    lastWrittenUs = targetUs;
  }
}

// =========================================================
// BATTERY
// =========================================================
void checkBattery() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 2000) return; // Check every 2 seconds
  lastCheck = millis();

  int rawADC = analogRead(BATTERY_PIN);
  float pinVoltage       = (rawADC / 4095.0) * 3.3;
  globalBatteryVoltage   = pinVoltage * BATTERY_CALIBRATION;

  // Alarm Logic: Below 6.5V is LOW, Below 3.0V is cutoff
  if (globalBatteryVoltage < 6.5 && globalBatteryVoltage > 3.0) {
    if (!lowBatteryAlert) Serial.println(F("SYSTEM: Low Battery Alarm Activated"));
    lowBatteryAlert = true;
  } else {
    if (lowBatteryAlert) Serial.println(F("SYSTEM: Battery OK"));
    lowBatteryAlert = false;
  }

  if (bleConnected) sendBLEStatus();
}

// =========================================================
// BLE COMMAND HANDLER (now accepts full string for direct angle)
// =========================================================
void handleBLECommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();
  char first = cmd[0];

  // 1. VOICE & MODE COMMANDS
  if (cmd.startsWith("AUTO") || cmd == "A") {
    if (currentMode != AUTO_MODE) {
      sendBLEStatus(); // Send final status before disconnect
      delay(200);
      switchMode(); 
    }
    return;
  }
  if (cmd.startsWith("MANUAL") || cmd == "M") {
    currentMode = MANUAL_MODE;
    preferences.putInt("mode", currentMode);
    Serial.println(F("VOICE: Mode -> MANUAL"));
    sendBLEStatus();
    return;
  }

  // If in Auto, we ignore manual position overrides
  if (currentMode != MANUAL_MODE) return;

  float targetAngle = targetServoAngle;

  // 2. POSITION COMMANDS (Voice or Button)
  if (cmd.startsWith("RELAX") || cmd == "R") {
    targetAngle = (float)servoRelaxedAngle;
    manualServoActive = false;
    Serial.println(F("VOICE: RELAX"));
  }
  else if (cmd.startsWith("STAND") || cmd == "UP") {
    targetAngle = (float)servoRelaxedAngle + 15.0f; // Slight lift
    manualServoActive = false;
    Serial.println(F("VOICE: STAND"));
  }
  else if (cmd.startsWith("SIT") || cmd == "DOWN") {
    targetAngle = (float)servoAssistAngle + 30.0f; // Deep sit
    manualServoActive = true;
    Serial.println(F("VOICE: SIT"));
  }
  else if (first == 'A' && cmd.length() > 1) { // App direct angle "A90"
    targetAngle = (float)cmd.substring(1).toInt();
  }
  else if (first == '+') {
    targetAngle = min(targetAngle + 10.0f, 180.0f);
  }
  else if (first == '-') {
    targetAngle = max(targetAngle - 10.0f, 0.0f);
  }
  else if (first == 'V' && cmd.length() > 1) {
    servoSpeedDelay = constrain(cmd.substring(1).toInt(), 0, 100);
    preferences.putInt("speed", servoSpeedDelay);
    sendBLEStatus();
    return;
  }
  // Threshold command: "T25"
  else if (first == 'T' && cmd.length() > 1) {
    int val = cmd.substring(1).toInt();
    tiltUpThreshold = constrain(val, 5, 80);
    tiltDownThreshold = tiltUpThreshold - 4.0;
    Serial.print("BLE CMD: THRESHOLD -> "); Serial.println(tiltUpThreshold);
    sendBLEStatus();
    return;
  }
  else {
    switch (first) {
      case '1':
        targetAngle     = servoAssistAngle;
        manualServoActive = true;
        Serial.println("BLE CMD: ASSIST");
        break;
      case '0':
        targetAngle     = servoRelaxedAngle;
        manualServoActive = false;
        Serial.println("BLE CMD: RELAX");
        break;
      case '+':
        targetAngle = min(targetAngle + 10.0f, 180.0f);
        Serial.print("BLE CMD: +10 -> "); Serial.println(targetAngle);
        break;
      case '-':
        targetAngle = max(targetAngle - 10.0f, 0.0f);
        Serial.print("BLE CMD: -10 -> "); Serial.println(targetAngle);
        break;
      case 'S':
        sendBLEStatus();
        return;
      default:
        return;
    }
  }

  if (targetAngle != targetServoAngle) {
    targetServoAngle = targetAngle;
  }
  sendBLEStatus();
}

// =========================================================
// BLE STATUS SENDER
// =========================================================
void sendBLEStatus() {
  if (!bleConnected || pTxCharacteristic == nullptr) return;

  int rawADC = analogRead(BATTERY_PIN);
  float pinVoltage     = (rawADC / 4095.0) * 3.3;
  float batteryVoltage = pinVoltage * BATTERY_CALIBRATION;
  int batteryPercent   = (batteryVoltage - 6.0) / (8.4 - 6.0) * 100;
  if (batteryPercent > 100) batteryPercent = 100;
  if (batteryPercent < 0)   batteryPercent = 0;

  String mode = (currentMode == MANUAL_MODE) ? "MANUAL" : "AUTO";

  String status = "ANGLE:" + String(targetServoAngle) +
                  ",BAT:"  + String(batteryPercent) +
                  ",VOLT:" + String(batteryVoltage, 1) +
                  ",MODE:" + mode +
                  ",PITCH:" + String(currentPitch, 1);

  pTxCharacteristic->setValue(status.c_str());
  pTxCharacteristic->notify();
}

// =========================================================
// BUTTON HANDLER
// =========================================================
void handleButton() {
  static bool lastFlickerableState = HIGH;
  static bool steadyState          = HIGH;
  static unsigned long lastDebounceTime = 0;

  bool currentState   = digitalRead(BUTTON_PIN);
  unsigned long now   = millis();

  if (currentState != lastFlickerableState) {
    lastDebounceTime      = now;
    lastFlickerableState  = currentState;
  }

  if ((now - lastDebounceTime) > DEBOUNCE_TIME) {
    if (steadyState != currentState) {
      steadyState = currentState;

      if (steadyState == LOW) {
        buttonPressTime  = now;
        buttonPressed    = true;
        longPressHandled = false;
        Serial.println("Button Pressed!");
      } else {
        if (buttonPressed && !longPressHandled) {
          Serial.println("Short Press!");
          if (currentMode == MANUAL_MODE) {
            manualServoActive = !manualServoActive;
            float targetAngle = manualServoActive ? (float)servoAssistAngle : (float)servoRelaxedAngle;
            if (abs(targetAngle - targetServoAngle) > 0.1) {
              targetServoAngle = targetAngle;
            }
            if (bleConnected) sendBLEStatus();
          }
        }
        buttonPressed = false;
        Serial.println("Button Released!");
      }
    }
  }

  // Long press detection: switch modes
  if (steadyState == LOW && buttonPressed && !longPressHandled) {
    if ((now - buttonPressTime) >= LONG_PRESS_TIME) {
      Serial.println("Long Press! Switching mode...");
      switchMode();
      longPressHandled = true;
    }
  }
}

// =========================================================
// AUTO MODE
// =========================================================
void handleAutoMode() {
  if (activeMpuAddress == 0) return;

  // 1. LIMIT SENSOR FREQUENCY to 100Hz (10ms)
  static unsigned long lastMpuRead = 0;
  unsigned long nowMs = millis();
  if (nowMs - lastMpuRead < 10) return; 
  lastMpuRead = nowMs;

  // 2. HIGH PRECISION TIMING for filter
  unsigned long nowUs = micros();
  float dt = (nowUs - lastFiltTime) / 1000000.0;
  if (dt <= 0 || dt > 0.1) dt = 0.01; // Safety cap
  lastFiltTime = nowUs;

  Wire.beginTransmission(activeMpuAddress);
  Wire.write(0x3B); 
  Wire.endTransmission(false);
  Wire.requestFrom((int)activeMpuAddress, 14, (int)true); 
  
  if (Wire.available() < 14) return;

  int16_t rawAcX = Wire.read() << 8 | Wire.read();
  int16_t rawAcY = Wire.read() << 8 | Wire.read();
  int16_t rawAcZ = Wire.read() << 8 | Wire.read();
  Wire.read(); Wire.read(); // Skip Temp
  int16_t rawGyX = Wire.read() << 8 | Wire.read();
  int16_t rawGyY = Wire.read() << 8 | Wire.read();
  int16_t rawGyZ = Wire.read() << 8 | Wire.read();

  // 3. FILTER
  float accAngle = atan2((float)rawAcX, sqrt((float)rawAcY * rawAcY + (float)rawAcZ * rawAcZ)) * 180.0 / PI;
  float gyroRate = (float)rawGyY / 131.0; 
  filteredAngle = alpha * (filteredAngle + gyroRate * dt) + (1.0 - alpha) * accAngle;
  currentPitch = filteredAngle; 

  // 4. SMART SIT DETECTION & STATE MACHINE
  float absAngle = abs(filteredAngle);
  float nextTarget = targetServoAngle; 

  // State Transition Logic
  if (absAngle < 8.0) {
    currentAutoState = STATE_IDLE;
    sitTimer = 0;
  } 
  else if (absAngle > 75.0) {
    if (sitTimer == 0) sitTimer = millis();
    if (millis() - sitTimer > 2000) {
      if (currentAutoState != STATE_SITTING) {
        currentAutoState = STATE_SITTING;
        Serial.println("State: SITTING (Stable)");
        beep(50); // Tiny chirp to confirm sit-mode
      }
    } else if (currentAutoState != STATE_SITTING) {
      currentAutoState = STATE_WALKING;
    }
  } 
  else {
    // If we were sitting and angle drops below 70, we are STANDING UP
    if (currentAutoState == STATE_SITTING) {
      if (absAngle < 70.0) {
        standUpBoost = true;
        Serial.println("State: STANDING UP (Boost active)");
        currentAutoState = STATE_WALKING;
        sitTimer = 0;
      }
      // If between 70.0 and 75.0, remain in STATE_SITTING (Hysteresis)
    } else {
      sitTimer = 0;
      currentAutoState = STATE_WALKING;
    }
  }

  // 5. CALCULATE TARGET BASED ON STATE
  if (currentAutoState == STATE_IDLE) {
    nextTarget = (float)servoRelaxedAngle;
    standUpBoost = false;
  } 
  else if (currentAutoState == STATE_SITTING) {
    // SITTING: Provide support but slightly back off (5%) to stop motor humming
    float fullSitTarget = 15.0 + (absAngle - tiltUpThreshold) * (130.0 - 15.0) / (90.0 - tiltUpThreshold);
    nextTarget = fullSitTarget * 0.95; 
    standUpBoost = false;
  }
  else {
    // WALKING or TRANSITIONING
    nextTarget = 15.0 + (absAngle - tiltUpThreshold) * (130.0 - 15.0) / (90.0 - tiltUpThreshold);
    
    // Apply STAND-UP BOOST: Move faster and slightly further to assist the lift
    if (standUpBoost) {
      nextTarget += 10.0; // Extra push
      if (absAngle < 35.0) {
        standUpBoost = false; // End boost when nearly standing
      }
    }
  }
  
  nextTarget = constrain(nextTarget, 0, 180);

  // 6. HYSTERESIS / JITTER FILTER
  // Only update target if sensor moves > 0.5 degrees to filter noise but remain responsive
  if (abs(nextTarget - targetServoAngle) >= 0.5) {
    targetServoAngle = nextTarget;
    
    static unsigned long lastLog = 0;
    if (millis() - lastLog > 500) {
      Serial.print("Pitch: "); Serial.print(filteredAngle, 1);
      Serial.print(" | State: "); 
      if(currentAutoState==STATE_IDLE) Serial.print("IDLE");
      else if(currentAutoState==STATE_SITTING) Serial.print("SITTING");
      else Serial.print("WALKING");
      Serial.print(" | Target: "); Serial.println(targetServoAngle, 1);
      lastLog = millis();
    }
  }

  // 7. PERIODIC TELEMETRY
  static unsigned long lastBle = 0;
  if (bleConnected && (millis() - lastBle > 400)) {
    sendBLEStatus();
    lastBle = millis();
  }
}

// =========================================================
// MANUAL MODE
// =========================================================
void handleManualMode() {
  // Manual mode logic (Button is handled globally in loop)
}

// =========================================================
// SWITCH MODE
// =========================================================
void switchMode() {
  currentMode = (currentMode == AUTO_MODE) ? MANUAL_MODE : AUTO_MODE;
  preferences.putInt("mode", currentMode); 
  
  if (currentMode == MANUAL_MODE) {
    BLEDevice::startAdvertising();
    bleAdvertising = true;
    Serial.println(F("BLE: Switched to Manual, Advertising ON."));
  } else {
    BLEDevice::stopAdvertising();
    bleAdvertising = false;
    if (bleConnected) {
       pServer->disconnect(pServer->getConnId()); // Force disconnect
    }
    Serial.println(F("BLE: Switched to Auto, Bluetooth OFF."));
  }

  beep(200);
  updateModeLEDs();
}

// =========================================================
// HELPERS
// =========================================================
void updateModeLEDs() {
  if (currentMode == AUTO_MODE) {
    digitalWrite(GREEN_LED_PIN, HIGH);
    digitalWrite(BLUE_LED_PIN,  LOW);
  } else {
    digitalWrite(GREEN_LED_PIN, LOW);
    digitalWrite(BLUE_LED_PIN,  HIGH);
  }
}

void beep(int duration) {
  digitalWrite(RED_LED_BUZZER_PIN, HIGH);
  delay(duration);
  digitalWrite(RED_LED_BUZZER_PIN, LOW);
}

// =========================================================
// OLED DISPLAY DASHBOARD
// =========================================================
void updateDisplay() {
  static unsigned long lastDisp = 0;
  if (millis() - lastDisp < 100) return; // 10Hz update for better responsiveness
  lastDisp = millis();

  display.clearDisplay();

  // 1. BIG MODE DISPLAY (Center focus)
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 5);
  display.print(currentMode == AUTO_MODE ? "AUTO" : "MANUAL");
  
  if (bleConnected) {
    display.setTextSize(1);
    display.setCursor(100, 0);
    display.print("BT");
  }

  display.drawLine(0, 25, 128, 25, SSD1306_WHITE);

  // 2. DATA SECTION (Smaller)
  display.setTextSize(1);
  display.setCursor(0, 32);
  display.print("LEG: ");
  display.print((int)round(currentPitch));
  display.print("o");

  display.setCursor(65, 32);
  display.print("SRV: ");
  display.print((int)round(targetServoAngle));
  display.print("o");

  // 3. ACTIVITY STATE (Bottom)
  display.setCursor(0, 54);
  if (currentMode == AUTO_MODE) {
    if (currentAutoState == STATE_IDLE) display.print("ST:IDLE");
    else if (currentAutoState == STATE_SITTING) display.print("ST:SIT");
    else display.print("ST:WALK");
  } else {
    display.print(manualServoActive ? "ST:ACTIVE" : "ST:RELAX");
  }

  // 4. BATTERY BAR (Bottom Right)
  int batteryPercent = (globalBatteryVoltage - 6.0) / (8.4 - 6.0) * 100;
  batteryPercent = constrain(batteryPercent, 0, 100);

  if (lowBatteryAlert) {
    if ((millis() / 500) % 2 == 0) {
      display.setTextSize(1);
      display.setCursor(65, 45);
      display.print("LOW BATT!");
    }
  }

  display.drawRect(85, 54, 30, 8, SSD1306_WHITE);
  display.drawRect(115, 56, 2, 4, SSD1306_WHITE);
  int barWidth = map(batteryPercent, 0, 100, 0, 28);
  display.fillRect(86, 55, barWidth, 6, SSD1306_WHITE);

  display.display();
}
