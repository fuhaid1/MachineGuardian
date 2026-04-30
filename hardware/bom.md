# Bill of Materials

| # | Component | Specification | Qty | Notes |
|---|---|---|---|---|
| 1 | ESP32 DevKit | 38-pin, 4MB flash | 1 | Any standard ESP32 DevKit works |
| 2 | SCT-013 current clamp | 100A / 50mA, bare CT output | 1 | Do **not** use the -030 (voltage output) variant |
| 3 | PN532 NFC/RFID module | V3, I2C/SPI/UART | 1 | Set DIP to I2C mode before soldering |
| 4 | SSD1306 OLED display | 0.91 inch, I2C, 128×32 | 1 | Address 0x3C (most common) |
| 5 | Active buzzer | 3.3V or 5V (active = self-oscillating) | 1 | Not a passive piezo element |
| 6 | LED — red | 5mm, standard | 1 | Any red LED, ~2V Vf |
| 7 | LED — green | 5mm, standard | 1 | Any green LED, ~2.2V Vf |
| 8 | LED — yellow | 5mm, standard | 1 | Any yellow LED, ~2.1V Vf |
| 9 | Resistor 220Ω | 1/4W, ±5% | 3 | LED current limiting |
| 10 | Resistor 10kΩ | 1/4W, ±1% | 2 | Bias voltage divider. Use 1% for stable midpoint |
| 11 | Resistor 33Ω | 1/4W, ±1% **metal film** | 1 | Burden resistor — metal film is lower noise than carbon |
| 12 | Resistor 4.7kΩ | 1/4W, ±5% | 2 | I2C pull-ups (if not on PN532 module) |
| 13 | NFC card or fob | MIFARE Classic or Ultralight | ≥1 | Any 13.56MHz ISO14443A card |
| 14 | USB cable | Micro-USB or USB-C (match ESP32) | 1 | Power supply |
| 15 | USB power supply | 5V, ≥1A | 1 | Phone charger works fine |

### Optional — decoupling capacitors

Not required for basic operation. Recommended for soldered PCB builds to improve noise rejection and ADC stability. The firmware's `Wire.end()` trick handles interference without them on a breadboard.

| # | Component | Specification | Qty | Notes |
|---|---|---|---|---|
| C1 | Capacitor 10µF | Electrolytic, 10V+ | 1 | Bias midpoint filter — reduces ADC flicker |
| C2 | Capacitor 100nF | Ceramic, 0.1µF, 10V+ | 3 | Decoupling — one per module VCC pin (PN532, OLED, ESP32) |

## Optional — for PCB build

| # | Component | Specification | Qty | Notes |
|---|---|---|---|---|
| P1 | 2.54mm pin headers | Male/female, through-hole | assorted | For module sockets |
| P2 | 3.5mm screw terminal | 2-pin | 1 | For CT sensor connection |
| P3 | NPN transistor | 2N2222 or BC547 | 1 | Only needed if buzzer draws >40mA |
| P4 | Resistor 1kΩ | 1/4W | 1 | Transistor base resistor (if using transistor) |
| P5 | Shielded 2-wire cable | Twisted pair, microphone cable | ~15cm | For CT sensor connection to PCB |

## Sourcing notes

- **SCT-013 (100A/50mA):** Search for "SCT-013-000" (bare CT, no built-in burden). Available on AliExpress, Amazon. The "-030" suffix indicates a built-in burden and voltage output — this is a different product requiring different firmware constants.
- **PN532 V3 module:** The blue module with solder-bridge or DIP switch for interface selection. Ensure it has exposed SDA/SCL pads.
- **OLED 0.91" SSD1306:** I2C variant (4-pin: VCC, GND, SCL, SDA). Not the SPI variant.
- **Active buzzer:** Must be self-oscillating (active), not a passive piezo element. Active buzzers buzz when you connect 3.3V directly. Passive ones need a PWM signal.

## Total estimated cost

| Category | Approximate cost |
|---|---|
| ESP32 | $3–6 |
| SCT-013 clamp | $5–10 |
| PN532 module | $4–8 |
| OLED display | $2–4 |
| Passives + LEDs | $2–4 |
| Capacitors (optional) | $1–2 |
| NFC cards (5-pack) | $3–6 |
| **Total (without caps)** | **~$19–38 USD** |
| **Total (with caps)** | **~$20–40 USD** |

Prices vary by source and shipping. Buying from AliExpress is significantly cheaper but has longer delivery times.
