#include <Wire.h>
#include <ESP32Servo.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// --- Pin Definitions ---
const int SERVO_PIN          = 18;
const int I2C_SDA            = 21;
const int I2C_SCL            = 22;
const int BUTTON_PIN         = 4;
const int RED_LED_BUZZER_PIN = 19;
const int BLUE_LED_PIN       = 25;  // Manual Mode Indicator
const int GREEN_LED_PIN      = 26;  // Auto Mode Indicator
const int BATTERY_PIN        = 34;  // Analog pin for battery voltage divider

// --- Battery Calibration ---
float BATTERY_CALIBRATION = 3.0;

// --- BLE Nordic UART Service (NUS) UUIDs ---
#define NUS_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID       "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // Phone -> ESP32
#define NUS_TX_UUID       "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // ESP32 -> Phone

BLEServer*         pServer          = nullptr;
BLECharacteristic* pTxCharacteristic = nullptr;
bool bleConnected   = false;
bool bleAdvertising = false;

// --- Objects ---
Servo kneeServo;
byte activeMpuAddress = 0;

// --- State ---
enum Mode { AUTO_MODE, MANUAL_MODE };
Mode currentMode = AUTO_MODE;

unsigned long buttonPressTime = 0;
bool buttonPressed     = false;
bool longPressHandled  = false;

const unsigned long LONG_PRESS_TIME = 1000;
const unsigned long DEBOUNCE_TIME   = 50;

int servoRelaxedAngle  = 0;
int servoAssistAngle   = 90;
bool manualServoActive = false;
int currentServoAngle  = -1;

const float TILT_UP_THRESHOLD   = 32.0;
const float TILT_DOWN_THRESHOLD = 28.0;

// --- Forward Declarations ---
void beep(int duration);
void updateModeLEDs();
void sendBLEStatus();
void handleBLECommand(char cmd);

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
    if (bleAdvertising) {
      BLEDevice::startAdvertising(); // Restart so phone can reconnect
    }
  }
};

class MyRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    String val = String(pChar->getValue().c_str());
    if (val.length() > 0) {
      handleBLECommand(val[0]);
    }
  }
};

// =========================================================
// SETUP
// =========================================================
void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_PIN,         INPUT_PULLUP);
  pinMode(RED_LED_BUZZER_PIN, OUTPUT);
  pinMode(BLUE_LED_PIN,       OUTPUT);
  pinMode(GREEN_LED_PIN,      OUTPUT);

  digitalWrite(RED_LED_BUZZER_PIN, LOW);
  updateModeLEDs();

  // Servo
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  kneeServo.setPeriodHertz(50);
  kneeServo.attach(SERVO_PIN, 500, 2400);
  kneeServo.write(servoRelaxedAngle);

  // MPU6050 I2C scan
  Wire.begin(I2C_SDA, I2C_SCL);
  delay(100);
  Serial.println("Scanning I2C bus for MPU6050...");
  for (byte address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      Serial.print("-> I2C device at 0x"); Serial.println(address, HEX);
      if (address == 0x68 || address == 0x69) activeMpuAddress = address;
    }
  }
  if (activeMpuAddress == 0) {
    while (1) {
      Serial.println("CRITICAL ERROR: No MPU6050 found!");
      digitalWrite(RED_LED_BUZZER_PIN, HIGH); delay(500);
      digitalWrite(RED_LED_BUZZER_PIN, LOW);  delay(500);
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

  // --- BLE Init (done once; advertising starts only in Manual Mode) ---
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
  // Do NOT start advertising here — only when entering Manual Mode

  delay(100);
  beep(200); // Startup beep

  // Button wiring check
  if (digitalRead(BUTTON_PIN) == LOW) {
    while (1) {
      Serial.println("HARDWARE ERROR: Button permanently shorted to GND! Rotate 90deg.");
      digitalWrite(RED_LED_BUZZER_PIN, HIGH); delay(100);
      digitalWrite(RED_LED_BUZZER_PIN, LOW);  delay(100);
    }
  }

  Serial.println("System Ready. Currently in AUTO mode.");
}

// =========================================================
// LOOP
// =========================================================
void loop() {
  handleButton();
  checkBattery();

  if (currentMode == AUTO_MODE) {
    handleAutoMode();
  } else {
    handleManualMode();
  }

  delay(20);
}

// =========================================================
// BATTERY
// =========================================================
void checkBattery() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 10000) return;
  lastCheck = millis();

  int rawADC = analogRead(BATTERY_PIN);
  float pinVoltage    = (rawADC / 4095.0) * 3.3;
  float batteryVoltage = pinVoltage * BATTERY_CALIBRATION;

  int batteryPercent = (batteryVoltage - 5.0) / (8.4 - 5.0) * 100;
  if (batteryPercent > 100) batteryPercent = 100;
  if (batteryPercent < 0)   batteryPercent = 0;

  Serial.print("Battery: "); Serial.print(batteryVoltage);
  Serial.print("V ("); Serial.print(batteryPercent); Serial.println("%)");

  // Alarm: below 4.0V (but > 0.5V to ignore floating pin)
  if (batteryVoltage < 4.0 && batteryVoltage > 0.5) {
    Serial.println("LOW BATTERY ALARM! (Below 4.0V)");
    beep(500); delay(200); beep(500);
  }

  // Push update to phone if connected
  if (bleConnected) sendBLEStatus();
}

// =========================================================
// BLE COMMAND HANDLER
// =========================================================
void handleBLECommand(char cmd) {
  if (currentMode != MANUAL_MODE) return; // Safety: ignore if not in manual

  int targetAngle = (currentServoAngle < 0) ? servoRelaxedAngle : currentServoAngle;

  switch (cmd) {
    case '1':  // ASSIST position
      targetAngle     = servoAssistAngle;
      manualServoActive = true;
      Serial.println("BLE CMD: ASSIST");
      break;
    case '0':  // RELAX position
      targetAngle     = servoRelaxedAngle;
      manualServoActive = false;
      Serial.println("BLE CMD: RELAX");
      break;
    case '+':  // Fine +10 degrees
      targetAngle = min(targetAngle + 10, 180);
      Serial.print("BLE CMD: +10 -> "); Serial.println(targetAngle);
      break;
    case '-':  // Fine -10 degrees
      targetAngle = max(targetAngle - 10, 0);
      Serial.print("BLE CMD: -10 -> "); Serial.println(targetAngle);
      break;
    case 's':  // Status request only
      sendBLEStatus();
      return;
    default:
      return;
  }

  if (targetAngle != currentServoAngle) {
    kneeServo.write(targetAngle);
    currentServoAngle = targetAngle;
  }
  sendBLEStatus(); // Always confirm back to phone
}

// =========================================================
// BLE STATUS SENDER  (format: "ANGLE:90,BAT:75,MODE:MANUAL")
// =========================================================
void sendBLEStatus() {
  if (!bleConnected || pTxCharacteristic == nullptr) return;

  int rawADC = analogRead(BATTERY_PIN);
  float pinVoltage     = (rawADC / 4095.0) * 3.3;
  float batteryVoltage = pinVoltage * BATTERY_CALIBRATION;
  int batteryPercent   = (batteryVoltage - 5.0) / (8.4 - 5.0) * 100;
  if (batteryPercent > 100) batteryPercent = 100;
  if (batteryPercent < 0)   batteryPercent = 0;

  int angle = (currentServoAngle < 0) ? 0 : currentServoAngle;
  String mode = (currentMode == MANUAL_MODE) ? "MANUAL" : "AUTO";

  String status = "ANGLE:" + String(angle) +
                  ",BAT:"  + String(batteryPercent) +
                  ",MODE:" + mode;

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
          // Short press: toggle servo in Manual mode
          Serial.println("Short Press!");
          if (currentMode == MANUAL_MODE) {
            manualServoActive = !manualServoActive;
            int targetAngle = manualServoActive ? servoAssistAngle : servoRelaxedAngle;
            if (targetAngle != currentServoAngle) {
              kneeServo.write(targetAngle);
              currentServoAngle = targetAngle;
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

  Wire.beginTransmission(activeMpuAddress);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom((int)activeMpuAddress, 6, (int)true);
  if (Wire.available() < 6) return;

  int16_t AcX = Wire.read() << 8 | Wire.read();
  int16_t AcY = Wire.read() << 8 | Wire.read();
  int16_t AcZ = Wire.read() << 8 | Wire.read();

  float pitch = atan2((float)AcX,
                      sqrt((float)AcY * AcY + (float)AcZ * AcZ)) * 180.0 / PI;

  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 500) {
    Serial.print("Pitch: "); Serial.println(pitch);
    lastPrint = millis();
  }

  float absPitch    = abs(pitch);
  int   targetAngle = currentServoAngle;

  if      (absPitch > TILT_UP_THRESHOLD)   targetAngle = servoAssistAngle;
  else if (absPitch < TILT_DOWN_THRESHOLD) targetAngle = servoRelaxedAngle;

  if (targetAngle != currentServoAngle) {
    kneeServo.write(targetAngle);
    currentServoAngle = targetAngle;
    Serial.print("Auto -> "); Serial.println(targetAngle);
  }
}

// =========================================================
// MANUAL MODE  (BLE callbacks handle commands; button is fallback)
// =========================================================
void handleManualMode() {
  // Commands arrive via BLE callbacks (non-blocking)
  // Physical button short-press also works (handled in handleButton)
}

// =========================================================
// SWITCH MODE
// =========================================================
void switchMode() {
  if (currentMode == AUTO_MODE) {
    // --- Switch to MANUAL ---
    currentMode       = MANUAL_MODE;
    manualServoActive = false;
    kneeServo.write(servoRelaxedAngle);
    currentServoAngle = servoRelaxedAngle;

    // Start BLE advertising
    bleAdvertising = true;
    BLEDevice::startAdvertising();
    Serial.println("BLE: Advertising as 'Exoskeleton'. Open the controller app!");

  } else {
    // --- Switch to AUTO ---
    currentMode    = AUTO_MODE;
    bleAdvertising = false;
    bleConnected   = false;
    BLEDevice::stopAdvertising();
    Serial.println("BLE: Stopped.");
  }

  updateModeLEDs();
  beep(100); delay(100); beep(100);
  Serial.print("Mode -> ");
  Serial.println(currentMode == AUTO_MODE ? "AUTO" : "MANUAL");
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
