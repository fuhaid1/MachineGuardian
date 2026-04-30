# Wiring Reference

## Pin assignments

| Signal | ESP32 Pin | Notes |
|---|---|---|
| SCT-013 ADC | 34 | Input-only ADC, no pull-up needed |
| I2C SDA | 21 | Shared: PN532 + OLED |
| I2C SCL | 22 | Shared: PN532 + OLED |
| Green LED | 18 | 220Ω to GND |
| Red LED | 19 | 220Ω to GND |
| Yellow LED | 5 | 220Ω to GND |
| Buzzer | 23 | Active buzzer, + to pin, – to GND |

## SCT-013 circuit (100A/50mA bare CT)

```
                    3.3V
                     │
                   10kΩ
                     │
CT OUT+ ────────── NODE ──── PIN 34
                     │
                   10µF (to GND)
                     │
CT OUT- ────────── GND
         │
        33Ω (burden, soldered across CT terminals)
```

Calculation:
- CT secondary: 50mA at 100A primary
- Burden voltage: 50mA × 33Ω = 1.65V peak (RMS ~1.17V)
- Fits 0–3.3V ADC range with room to spare

## PN532 NFC module

DIP switch configuration for I2C mode:
```
  DIP-1  DIP-2
  [ON ]  [OFF]
```

Connections:
```
PN532 SDA  →  ESP32 PIN 21
PN532 SCL  →  ESP32 PIN 22
PN532 VCC  →  3.3V
PN532 GND  →  GND
```

I2C pull-ups: 4.7kΩ from SDA to 3.3V and SCL to 3.3V.
Many PN532 V3 modules include these on-board — check with a multimeter before adding external ones.

## SSD1306 OLED (0.91")

```
OLED SDA  →  ESP32 PIN 21  (same I2C bus as PN532)
OLED SCL  →  ESP32 PIN 22
OLED VCC  →  3.3V
OLED GND  →  GND
```

Default I2C address: `0x3C`. If your module uses `0x3D`, change `OLED_ADDR` in the firmware.

## LEDs

```
ESP32 PIN  →  220Ω  →  LED anode  →  LED cathode  →  GND

PIN 18: Green
PIN 19: Red
PIN 5:  Yellow
```

## Buzzer

Active buzzer (the kind that beeps when you apply voltage directly):
```
+ lead  →  PIN 23
– lead  →  GND
```

If the buzzer draws more than 40mA, add an NPN transistor (2N2222):
```
PIN 23  →  1kΩ  →  Base
                   Collector  →  Buzzer+  →  3.3V
                   Emitter   →  GND
Buzzer– → GND
```

## PCB layout notes

### Why layout matters

On a breadboard, the PN532's 13.56MHz oscillator noise couples into the ADC through shared power rails and parallel wire runs. Proper PCB layout eliminates this without software workarounds.

### Placement zones

```
┌──────────────────────────────────────────────┐
│                                              │
│  CT CIRCUIT          │        OLED           │
│  (top-left)          │     (top-right)       │
│                      │                       │
│  ────────────────────┤                       │
│                      │                       │
│           ESP32 (centre)                     │
│                      │                       │
│  ────────────────────┤                       │
│                      │                       │
│  LEDs + BUZZER       │      PN532            │
│  (bottom-left)       │  (bottom-right)       │
│                      │                       │
└──────────────────────────────────────────────┘
```

CT circuit and PN532 in opposite corners = maximum separation between the noise source and the sensitive analog circuit.

### Ground plane

Pour a copper fill on the bottom layer. Connect every GND pad through a via to the ground plane. Use a single star-ground point at the ESP32 GND pin.

This alone removes most of the ADC noise.

### Decoupling capacitors

Place within 2mm of each module's VCC pin:

| Module | Caps |
|---|---|
| PN532 | 100nF ceramic + 10µF electrolytic |
| OLED | 100nF ceramic |
| ESP32 3.3V pin | 100nF ceramic |

### CT cable

| Property | Recommendation |
|---|---|
| Length | ≤ 15cm. Never exceed 30cm. |
| Type | Twisted pair. Twist every ~2cm. |
| Shielding | Use shielded cable if available. Ground the shield at the **PCB end only** — grounding both ends creates a ground loop. |

Twisted pair reduces inducted EMF from nearby mains cables by ~10x compared to untwisted parallel wires.
