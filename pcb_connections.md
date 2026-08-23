# Knee Exoskeleton PCB: Connection List & Design Guide

This document lists all point-to-point connections, component pinouts, and PCB layout guidelines required to design a custom PCB for the Knee Exoskeleton System.

---

## 1. Pin Connection List (Netlist)

Below is the pin connection list organized by net name. 

| Net Name | Origin Pin (ESP32) | Target Pin(s) | Notes & Component Pin Details |
| :--- | :--- | :--- | :--- |
| **+5V** | ESP32 `VIN` / `5V` | OLED `VCC`*, Servo `VCC`* | Main 5V power bus from the regulator. |
| **+3.3V** | ESP32 `3V3` | MPU6050 `VCC`, OLED `VCC`* | Onboard filtered 3.3V output from ESP32. |
| **GND** | ESP32 `GND` | All Components `GND` | Common reference ground (Star configuration). |
| **I2C_SDA** | ESP32 `GPIO 21` | MPU6050 `SDA`, OLED `SDA` | I2C Data line. Add 4.7kΩ pull-up resistors to 3.3V if not present on modules. |
| **I2C_SCL** | ESP32 `GPIO 22` | MPU6050 `SCL`, OLED `SCL` | I2C Clock line. Add 4.7kΩ pull-up resistors to 3.3V if not present on modules. |
| **SERVO_PWM** | ESP32 `GPIO 18` | Knee Servo `Signal` (Yellow/White) | PWM signal output (5V tolerant, direct connection or via 220Ω buffer). |
| **BUTTON** | ESP32 `GPIO 4` | Button Terminal 1 | Push button input. Connect Terminal 2 to GND. (Uses internal pull-up in ESP32). |
| **ALARM** | ESP32 `GPIO 19` | Buzzer `+`, Red LED Anode | Sound & visual warning. Anode connection through current-limiting resistors. |
| **LED_BLUE** | ESP32 `GPIO 25` | Blue LED Anode | Manual Mode indicator. Active HIGH. |
| **LED_GREEN** | ESP32 `GPIO 26` | Green LED Anode | Auto Mode indicator. Active HIGH. |
| **BAT_SENSE** | ESP32 `GPIO 34` | Voltage Divider Midpoint | Analog battery sensor. Connects to divider junction. |

*\*Note on OLED & Servo Power: Make sure your specific OLED module supports 3.3V or 5V (most support 3.3V). The High-Torque Servo MUST be powered directly from a high-current source, not the ESP32 pins. See Section 2.*

---

## 2. Component Pinout Configurations

### 1. ESP32-DevKitM-1 (MINI-1 Module) Pin Mapping
If you are using the **ESP32-DevKitM-1** development board, connect your components to the physical pin numbers of the 34-pin header layout as follows:

| Physical Pin No. | Header Name | Code GPIO | Connected Target / Net | Purpose / Function |
| :--- | :--- | :--- | :--- | :--- |
| **Pin 2** | `3V3` | - | Net **+3.3V** | Powers ESP32, MPU6050, and OLED |
| **Pin 8** | `I34` | **GPIO 34** | Net **BAT_SENSE** | Analog Battery Voltage Monitor input |
| **Pin 12** | `IO25` | **GPIO 25** | Net **LED_BLUE** | Blue Manual Mode LED Anode |
| **Pin 13** | `IO26` | **GPIO 26** | Net **LED_GREEN** | Green Auto Mode LED Anode |
| **Pin 16** | `5V` | - | Net **+5V** | 5V Input from Buck Converter |
| **Pin 17** | `GND` | - | Net **GND** | Main System Ground |
| **Pin 22** | `IO0` | **GPIO 0** | Net **BOOT** | Programming Boot Pin (Button to GND) |
| **Pin 23** | `IO4` | **GPIO 4** | Net **BUTTON** | Mode Toggle Button Input |
| **Pin 27** | `IO18` | **GPIO 18** | Net **SERVO_PWM** | Knee Servo Signal pin (Yellow/Orange) |
| **Pin 29** | `IO19` | **GPIO 19** | Net **ALARM** | Alarm Red LED Anode & Buzzer Positive |
| **Pin 30** | `IO22` | **GPIO 22** | Net **I2C_SCL** | I2C Clock for MPU6050 and OLED |
| **Pin 31** | `IO21` | **GPIO 21** | Net **I2C_SDA** | I2C Data for MPU6050 and OLED |
| **Pin 34** | `GND` | - | Net **GND** | Star Ground connection node |
| **Pin 7** | `RST` | EN | Net **RESET** | Reset line (EN Button to GND) |

### 2. Standard ESP32 DevKit v1 NodeMCU (For reference)
- **VIN (Pin 5V)**: Power Input ($5\text{V}$ regulated, or battery input if using external regulator).
- **3V3**: $3.3\text{V}$ output from internal ESP32 regulator (used for low-power sensors).
- **GND**: System ground.
- **GPIO 18**: Servo PWM.
- **GPIO 21 / 22**: SDA / SCL (I2C).
- **GPIO 4**: Input button.
- **GPIO 19 / 25 / 26**: Warning and status indicator outputs.
- **GPIO 34**: Analog ADC channel 1 (Input-only pin).

### 2. MPU6050 Accelerometer / Gyroscope Module
- **VCC**: Connect to ESP32 `3.3V` (most breakout boards have a $3.3\text{V}$ LDO regulator, allowing $3.3\text{V} - 5\text{V}$).
- **GND**: Connect to common ground.
- **SDA**: Connect to ESP32 `GPIO 21`.
- **SCL**: Connect to ESP32 `GPIO 22`.
- **AD0**: Connect to `GND` (sets I2C address to `0x68`, which matches firmware setup).

### 3. SSD1306 OLED Display Module (I2C version)
- **VCC**: Connect to `3.3V` (or `5V` depending on the specific module version).
- **GND**: Connect to common ground.
- **SDA**: Connect to ESP32 `GPIO 21`.
- **SCL**: Connect to ESP32 `GPIO 22`.

### 4. Knee Servo Motor (High-Current)
- **Signal (Orange/Yellow)**: Connect to ESP32 `GPIO 18`.
- **VCC (Red)**: Connect directly to the output of a high-current buck regulator or battery.
- **GND (Brown/Black)**: Connect to common ground.

### 5. Indicator LEDs (Red, Blue, Green)
- **LED Cathodes (-)**: Connect to common ground.
- **LED Anodes (+)**: Connect to their respective GPIO pins through current-limiting resistors:
  - Red LED Anode $\rightarrow$ Resistor (e.g., $220\:\Omega$) $\rightarrow$ ESP32 `GPIO 19`.
  - Blue LED Anode $\rightarrow$ Resistor (e.g., $220\:\Omega$) $\rightarrow$ ESP32 `GPIO 25`.
  - Green LED Anode $\rightarrow$ Resistor (e.g., $220\:\Omega$) $\rightarrow$ ESP32 `GPIO 26`.

### 6. Active Buzzer
- **Positive Pin (+)**: Connect through resistor (e.g., $100\:\Omega$ to limit current) $\rightarrow$ ESP32 `GPIO 19`.
- **Negative Pin (-)**: Connect to common ground.

---

## 3. PCB Schematic & Layout Guidelines

### A. Battery Voltage Divider Network
The battery monitoring logic relies on a voltage divider to scale down the battery's maximum voltage ($8.4\text{V}$ full charge LiPo) to a safe level for the ESP32 ADC pin (max $3.3\text{V}$). 

```text
    Battery (+) [8.4V Max]
          |
         [R1] (20kΩ, 1% tolerance)
          |
          +-------> [GPIO 34] (ESP32 ADC Input)
          |   |
         [R2] [C1] (100nF Ceramic capacitor in parallel with R2)
          |   |
         GND  GND
```
* **Resistor R1**: $20\text{ k}\Omega$ ($1\%$ metal film resistor for precision).
* **Resistor R2**: $10\text{ k}\Omega$ ($1\%$ metal film resistor for precision).
* **Division Ratio**: $\frac{R_2}{R_1 + R_2} = \frac{10}{20 + 10} = \frac{1}{3}$. A battery voltage of $8.4\text{V}$ scales to $2.8\text{V}$ at the GPIO pin, which falls safely below the ESP32's $3.3\text{V}$ ceiling.
* **Filter Capacitor C1**: $100\text{ nF}$ ceramic capacitor placed in parallel with $R_2$ close to the ESP32 pin. This filters out high-frequency switching noise caused by the servo motor and regulates the ADC measurements.

### B. Power Routing Guidelines (High-Current Paths)
> [!WARNING]
> Servo motors can draw transient peak currents exceeding $2\text{A}$ under load. Powering the servo directly from the ESP32's regulator will cause the microcontroller to brown out, crash, or permanently damage the board.

1. **Dedicated Servo Regulator**: Use an onboard buck regulator (e.g., LM2596, XL4015, or MP1584) configured for $5\text{V}$ or $6\text{V}$ with a current rating of at least $3\text{A}$ to feed the servo motor.
2. **Trace Widths**: 
   - **Signal Traces** (I2C, PWM, LEDs): $10\text{ mils}$ ($0.254\text{ mm}$).
   - **Low Power Traces** (3.3V, Sensor power): $15\text{-}20\text{ mils}$ ($0.381\text{-}0.508\text{ mm}$).
   - **High Power Traces** (Battery Vin, Servo VCC, Servo GND): Min $40\text{ mils}$ ($1.016\text{ mm}$) for 1oz copper, or use a copper plane.
3. **Decoupling Capacitors**:
   - Place a large electrolytic capacitor (e.g., $470\:\mu\text{F}$ to $1000\:\mu\text{F}$, $16\text{V}$) across the Servo VCC and GND terminals on the PCB. This acts as a reservoir to prevent voltage drops during motor acceleration.
   - Place $0.1\:\mu\text{F}$ ceramic bypass capacitors close to the VCC and GND pins of the MPU6050 and SSD1306 modules.

### C. Grounding & Noise Suppression (Star Ground)
* Implement a **Star Grounding Scheme**. Connect the high-power servo ground and the low-power microcontroller ground at a single, physical point (the negative terminal of the main power input capacitor). This prevents ground loop currents from injecting noise into the MPU6050 sensor readings.
* Keep the I2C lines (SDA/SCL) as short as possible, and run them away from the servo power and PWM signal traces to minimize crosstalk.
