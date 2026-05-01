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
int currentServoAngle  = 0;
int targetServoAngle   = 0;

int servoSpeedDelay     = 10;   // ms per degree (lower = faster)
float currentPitch      = 0.0;
float tiltUpThreshold   = 32.0;
float tiltDownThreshold = 28.0;

// --- Forward Declarations ---
void beep(int duration);
void updateModeLEDs();
void sendBLEStatus();
void handleBLECommand(String cmd);

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
      BLEDevice::startAdvertising();
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
  kneeServo.write(currentServoAngle);

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

  updateServo();

  delay(10);
}

// =========================================================
// SERVO UPDATER (Non-blocking Sweep)
// =========================================================
void updateServo() {
  if (currentServoAngle == targetServoAngle) return;

  if (servoSpeedDelay <= 0) {
    // Instant snap
    currentServoAngle = targetServoAngle;
    kneeServo.write(currentServoAngle);
    return;
  }

  static unsigned long lastMove = 0;
  if (millis() - lastMove >= servoSpeedDelay) {
    lastMove = millis();
    if (currentServoAngle < targetServoAngle) currentServoAngle++;
    else currentServoAngle--;
    kneeServo.write(currentServoAngle);
  }
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

  // DEBUG: Print raw ADC so you can calibrate
  Serial.print("RAW ADC: "); Serial.print(rawADC);
  Serial.print(" | Pin V: "); Serial.println(pinVoltage);

  int batteryPercent = (batteryVoltage - 6.0) / (8.4 - 6.0) * 100;
  if (batteryPercent > 100) batteryPercent = 100;
  if (batteryPercent < 0)   batteryPercent = 0;

  Serial.print("Battery: "); Serial.print(batteryVoltage);
  Serial.print("V ("); Serial.print(batteryPercent); Serial.println("%)");

  // Alarm: below 6.5V but above 4.5V (ignores no-battery / USB-only parasitic voltage which reads ~4.3V)
  if (batteryVoltage < 6.5 && batteryVoltage > 4.5) {
    Serial.println("LOW BATTERY ALARM!");
    beep(500); delay(200); beep(500);
  }

  // Push update to phone if connected
  if (bleConnected) sendBLEStatus();
}

// =========================================================
// BLE COMMAND HANDLER (now accepts full string for direct angle)
// =========================================================
void handleBLECommand(String cmd) {
  if (currentMode != MANUAL_MODE) return;

  int targetAngle = targetServoAngle;

  char first = cmd[0];

  // Direct angle command: "A90", "A120", etc.
  if (first == 'A' && cmd.length() > 1) {
    int angle = cmd.substring(1).toInt();
    targetAngle = constrain(angle, 0, 180);
    manualServoActive = (targetAngle > 0);
    Serial.print("BLE CMD: ANGLE -> "); Serial.println(targetAngle);
  }
  // Speed control command: "V10"
  else if (first == 'V' && cmd.length() > 1) {
    servoSpeedDelay = constrain(cmd.substring(1).toInt(), 0, 100);
    Serial.print("BLE CMD: SPEED -> "); Serial.println(servoSpeedDelay);
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
        targetAngle = min(targetAngle + 10, 180);
        Serial.print("BLE CMD: +10 -> "); Serial.println(targetAngle);
        break;
      case '-':
        targetAngle = max(targetAngle - 10, 0);
        Serial.print("BLE CMD: -10 -> "); Serial.println(targetAngle);
        break;
      case 's':
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
            int targetAngle = manualServoActive ? servoAssistAngle : servoRelaxedAngle;
            if (targetAngle != targetServoAngle) {
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
  currentPitch = pitch; // Update global for BLE

  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 500) {
    // Send telemetry to phone constantly when in Auto mode
    if (bleConnected) sendBLEStatus();
    lastPrint = millis();
  }

  float absPitch    = abs(pitch);
  int   targetAngle = targetServoAngle;

  if      (absPitch > tiltUpThreshold)   targetAngle = servoAssistAngle;
  else if (absPitch < tiltDownThreshold) targetAngle = servoRelaxedAngle;

  if (targetAngle != targetServoAngle) {
    targetServoAngle = targetAngle;
    Serial.print("Auto Target -> "); Serial.println(targetAngle);
  }
}

// =========================================================
// MANUAL MODE
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
    currentMode       = MANUAL_MODE;
    manualServoActive = false;
    targetServoAngle  = servoRelaxedAngle;

    bleAdvertising = true;
    BLEDevice::startAdvertising();
    Serial.println("BLE: Advertising as 'Exoskeleton'. Open the controller app!");

  } else {
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
