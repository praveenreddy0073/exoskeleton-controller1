# 🦿 Wearable Knee Exoskeleton System

An advanced, ESP32-powered assistive robotic wearable designed to augment human mobility. The platform combines real-time complementary sensor fusion, an autonomous gait state machine with stand-up boost, and a premium Progressive Web App (PWA) controller featuring biometric security, hands-free voice controls, and emergency SOS services.

---

## 🚀 Key Features

### ⚡ Embedded & Control System
* **Real-time Complementary Filter**: Executes at $100\text{Hz}$ to fuse MPU6050 accelerometer and gyroscope data, yielding precision pitch calculations free from high-frequency vibrations or integration drift.
* **Smart Gait State Machine**: Auto-detects user intent with transitions between `IDLE` (standing), `WALKING`, and `SITTING` states.
* **Stand-Up Boost**: Detects when a sitting user is rising, temporarily injecting a $+10^{\circ}$ torque-assist offset and ramping servo speed to maximum ($2\text{ms/degree}$) to aid lift-off.
* **Closed-Loop Safety**: Under-voltage protection reads dual-cell LiPo battery voltages, triggering buzzer chirps/LED alarms below $6.5\text{V}$, and safety-killing motor outputs below a critical $3.0\text{V}$ cell threshold.

### 📱 Progressive Web App (PWA)
* **Web Bluetooth (NUS)**: Low-latency telemetry and control streaming over the Nordic UART Service (NUS) profile.
* **Biometric Lockscreen**: Features local passcode protection with integrated WebAuthn for secure biometric (fingerprint/face scan) unlocking.
* **Hands-Free Multilingual Voice Commands**: Uses the Web Speech API to recognize voice actions ("Assist", "Relax", "Sit", "Stand") across English, Hindi, Kannada, and Telugu.
* **Emergency SOS & Geolocation**: A physical SOS button fetches high-accuracy GPS coordinates, builds a Google Maps URL, and redirects to WhatsApp/SMS to dispatch coordinates to configured emergency contacts.
* **Real-Time Goniometer & Telemetry Charts**: Visually represents knee joints using custom SVG arcs and real-time scrolling charts (Chart.js).

---

## 📂 Repository Structure

```text
├── Exoskeleton/
│   ├── Exoskeleton.ino             # ESP32 C++ Firmware (BLE, I2C, Complementary Filter)
│   ├── controller.html             # PWA Client Dashboard (Web Bluetooth, WebAuthn, Speech API)
│   ├── pcb_designer.html           # Interactive 2D PCB Trace Workbench Visualizer
│   ├── generate_kicad_project.py   # Python script generating KiCad project, schematics, and nets
│   ├── pcb_connections.md          # ESP32 DevKitM-1 physical netlist mapping
│   ├── manifest.json               # PWA App Shell config
│   └── sw.js                       # Service Worker for offline-first support
├── pcb_design_walkthrough.md       # Comprehensive PCB CAD design & routing guide
├── pcb_connections.md              # ESP32 DevKit v1 reference netlist mapping
└── exoskeleton_report.pdf          # Full System Technical Reference Manual
```

---

## 🔌 Hardware Schematics & Connections

The custom PCB supports both the standard **ESP32 DevKit v1 (30-pin)** and the **ESP32-DevKitM-1 (34-pin)** module.

### **Core Netlist Map**
| Net Name | ESP32 GPIO | Target Component Pin | Purpose / Description |
| :--- | :--- | :--- | :--- |
| **SERVO_PWM**| `GPIO 18` | Knee Servo Signal (Orange) | Actuator angle control PWM ($50\text{Hz}$) |
| **I2C_SDA**  | `GPIO 21` | MPU6050 `SDA` / SSD1306 `SDA` | I2C Data (Requires $4.7\text{k}\Omega$ Pull-up to 3.3V) |
| **I2C_SCL**  | `GPIO 22` | MPU6050 `SCL` / SSD1306 `SCL` | I2C Clock (Requires $4.7\text{k}\Omega$ Pull-up to 3.3V) |
| **BUTTON**   | `GPIO 4`  | Tactile Switch Pin 1 | Mode Switch Input (uses internal Pull-up) |
| **ALARM**    | `GPIO 19` | Buzzer (+) / Red LED Anode | Low Battery & Hardware Failure Alert |
| **LED_BLUE** | `GPIO 25` | Blue LED Anode | Manual Mode active indicator |
| **LED_GREEN**| `GPIO 26` | Green LED Anode | Auto Mode active indicator |
| **BAT_SENSE**| `GPIO 34` | Voltage Divider Junction | Analog battery measurement input |

### **Battery Monitoring Network**
To safely monitor a $2\text{S}$ LiPo battery ($8.4\text{V}$ maximum charge) with the ESP32's $3.3\text{V}$ ADC input, a voltage divider scales the battery voltage by a factor of 3:
```text
Battery (+) [8.4V Max]
      |
     [R1] (20kΩ, 1% metal film)
      |
      +-------> ESP32 [GPIO 34] (ADC Input)
      |   |
     [R2] [C1] (100nF ceramic capacitor in parallel with R2)
      |   |
     GND  GND
```
$$\text{V}_{\text{ADC}} = \text{V}_{\text{BAT}} \times \left( \frac{10\text{k}\Omega}{20\text{k}\Omega + 10\text{k}\Omega} \right) = \frac{\text{V}_{\text{BAT}}}{3}$$

---

## ⚡ PCB Design & Star Grounding Guidelines
High-torque servos pull peak currents exceeding $2\text{A}$. To prevent noise injection and MCU brown-outs:
1. **Dedicated Regulator**: Power the servo from an onboard step-down buck converter (e.g., MP1584EN configured to $5\text{V}$ or $6\text{V}$) rated for $\geq 3\text{A}$.
2. **Decoupling Capacitors**: Place a bulk radial electrolytic capacitor ($470\:\mu\text{F} - 1000\:\mu\text{F}$) close to the servo power terminals, and $0.1\:\mu\text{F}$ ceramics near the sensor/OLED headers.
3. **Star Grounding Layout**: Connect the high-current servo ground return and the microcontroller ground at a *single physical point* (the negative terminal of the main buck converter output capacitor). 
4. **RF Keep-out**: Do not route copper traces, ground planes, or lines directly underneath the ESP32 onboard PCB antenna area.

---

## 🛠️ Installation & Setup

### **1. Firmware Deployment**
1. Open the [Exoskeleton.ino](file:///c:/Users/Lenovo/minipro/Exoskeleton/Exoskeleton.ino) in the Arduino IDE.
2. Install dependencies via the Library Manager:
   * `ESP32Servo` by Kevin Sweet
   * `Adafruit SSD1306` & `Adafruit GFX Library`
3. Select your ESP32 Dev Module board target.
4. Compile and flash the code. If your device fails to boot or chirps rapidly, verify that the boot button (`GPIO 0`) and MPU6050 connections are secure.

### **2. Web Controller Client**
Because Web Bluetooth requires a secure context, serve the application over `HTTPS` or run it on a local environment:
1. Navigate to the `Exoskeleton` folder.
2. Spin up a local development server (e.g., using python):
   ```bash
   python -m http.server 8000
   ```
3. Open `http://localhost:8000/controller.html` inside a compatible web browser (Chrome, Edge, or Opera).
4. Tap the **AUTO/MANUAL** mode badge, or pair your ESP32 controller using the **Connect** button.

---

## 🎮 Interface Controls
* **Mode Toggle**: Long-press the physical tactile button ($2\text{s}$) or toggle the mode badge on the PWA to switch between Autonomous and Manual modes.
* **Auto Mode**: The system adjusts angles autonomously based on leg pitch.
* **Manual Mode**: Overrides active joint angles via the slider, fine-adjustment buttons, preset angles, or via voice recognition phrases like:
  * *"Sit"* / *"Down"* $\rightarrow$ Moves joint to $120^{\circ}$ (flexion).
  * *"Stand"* / *"Up"* $\rightarrow$ Moves joint to $15^{\circ}$ (extension).
  * *"Relax"* $\rightarrow$ Relaxes the servo actuator.
