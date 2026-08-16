# Infrared (IR) Remote Control Protocols Reference

A comprehensive guide to common consumer infrared remote control protocols, their encoding mechanisms, timing specifications, and supportability on the Raspberry Pi Pico (CircuitPython).

---

## 1. Overview of IR Transmission

Infrared remotes transmit data by turning an infrared LED on and off at a specific **carrier frequency** (typically 36 kHz to 40 kHz). The receiver demodulates this carrier, producing an active-LOW digital signal where:
- **Mark (Pulse)**: Carrier frequency detected (line pulled LOW).
- **Space**: No carrier detected (line idle HIGH).

---

## 2. Common IR Protocols Comparison

| Protocol | Typical Brands & Devices | Carrier | Modulation Style | Typical Bit Length | Frame Characteristics |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **NEC** *(Supported)* | Arduino kits, Apple TV, LG, budget electronics | 38 kHz | Space-Distance | 32 bits | 9.0ms leader + 4.5ms space; 32 bits with inverted checksums; 4-pulse repeat code |
| **Sony SIRC** *(Supported)* | Sony TVs, AVRs, Soundbars, DVD/Blu-ray | 40 kHz | Pulse-Width (PWM) | 12, 15, or 20 bits | 2.4ms leader + 0.6ms space; PWM mark durations (1.2ms for '1', 0.6ms for '0') |
| **Samsung** | Samsung TVs, Monitors, Soundbars | 38 kHz | Space-Distance | 32 bits | 4.5ms leader + 4.5ms space; 16-bit address + 16-bit command |
| **Philips RC-5** | Philips, Marantz, Denon, European audio gear | 36 kHz | Manchester (Bi-phase) | 14 bits | 1.78ms bit period (mid-bit transition); toggle bit flips on subsequent presses |
| **Philips RC-6** | Philips, Microsoft Windows Media Center (MCE) | 36 kHz | Manchester (Bi-phase) | 21+ bits | 2.66ms leader; variable payload length with trailer field |
| **Panasonic / Kaseikyo** | Panasonic, Pioneer, Denon, Sharp | 37 kHz | Space-Distance | 48 bits | 3.5ms leader + 1.75ms space; includes 16-bit OEM manufacturer ID + parity |
| **JVC / Sharp** | JVC, Sharp TVs, Audio components | 38 kHz | Space-Distance | 15–16 bits | 8.4ms / 4.2ms leader (JVC); transmitted twice per press |
| **HVAC / Air Conditioners** | Daikin, Mitsubishi, LG, Gree, Midea, Panasonic | 38 kHz | Space-Distance (Burst) | **100–300+ bits** | Stateful packet sending full AC state (temperature, fan, mode, vane swing, timer) |
| **Bang & Olufsen (B&O)** | Bang & Olufsen premium audio/video | **455 kHz** | Pulse-Distance | Variable | High-frequency carrier; requires specialized optical sensor hardware |

---

## 3. Protocol Deep-Dives

### A. NEC Protocol
- **Leader**: 9000 µs Mark + 4500 µs Space
- **Logical `0`**: 560 µs Mark + 560 µs Space ($\approx 1120\,\mu\text{s}$)
- **Logical `1`**: 560 µs Mark + 1690 µs Space ($\approx 2250\,\mu\text{s}$)
- **Repeat Frame**: 9000 µs Mark + 2250 µs Space + 560 µs Mark
- **Structure**: `[Address (8 bits)] [~Address (8 bits)] [Command (8 bits)] [~Command (8 bits)]`

### B. Sony SIRC Protocol
- **Leader**: 2400 µs Mark + 600 µs Space
- **Logical `0`**: 600 µs Mark + 600 µs Space ($\approx 1200\,\mu\text{s}$)
- **Logical `1`**: 1200 µs Mark + 600 µs Space ($\approx 1800\,\mu\text{s}$)
- **Structure (12-bit)**: `[Command (7 bits, LSB first)] [Device Address (5 bits, LSB first)]`
- **Repetition**: Typically re-transmitted 3 times per single keypress.

### C. Samsung Protocol
- **Leader**: 4500 µs Mark + 4500 µs Space
- **Logical `0`**: 560 µs Mark + 560 µs Space
- **Logical `1`**: 560 µs Mark + 1690 µs Space
- **Structure**: `[Address (8 bits)] [Address Copy (8 bits)] [Command (8 bits)] [~Command (8 bits)]`

### D. Philips RC-5 Protocol
- Uses **Manchester Encoding** (bit transition in the center of each bit clock):
  - **Logical `0`**: Mark (889 µs) followed by Space (889 µs)
  - **Logical `1`**: Space (889 µs) followed by Mark (889 µs)
- **Structure (14 bits)**:
  - 2 Start bits (`11`)
  - 1 Toggle bit (flips state on each new button press to distinguish hold from re-press)
  - 5 Device / System bits
  - 6 Command bits

### E. Air Conditioner / HVAC Protocols
- Unlike AV remotes which send stateless momentary commands ("Volume Up", "Key 1"), AC remotes are **stateful**.
- Pressing any single button (e.g. changing temp by +1°C) sends a massive binary payload containing:
  - Power State (ON/OFF)
  - Operating Mode (Auto, Cool, Dry, Fan, Heat)
  - Set Temperature (e.g., 16°C to 30°C)
  - Fan Speed (Auto, Low, Medium, High)
  - Vane Swing / Horizontal & Vertical Direction
  - Timers / Sleep / Eco Mode
  - Frame Checksum
- Total transitions often exceed 150–300 pulses.

---

## 4. Hardware and Software Compatibility on Raspberry Pi Pico

### Software Support
- **100% of these protocols are supportable** on the Raspberry Pi Pico via CircuitPython.
- `pulseio.PulseIn` records raw microsecond pulse and space durations into a buffer. Any protocol can be decoded by writing a parser function for its pulse timing characteristics.

### Hardware Considerations
1. **Standard 38 kHz Receiver Modules (VS1838B / TSOP38238)**:
   - Contains an internal bandpass filter centered at 38 kHz with a reception bandwidth of **36 kHz to 40 kHz**.
   - Covers **>99% of all commercial consumer remotes** (NEC, Sony, Samsung, RC-5, RC-6, Panasonic, HVAC).
2. **High-Frequency (455 kHz - B&O)**:
   - Cannot be picked up by a standard 38 kHz receiver; requires a dedicated 455 kHz receiver module (e.g., TSOP7000).
3. **Buffer Sizing for AC Remotes**:
   - Standard AV remotes require buffer sizes of $\approx 64\text{--}100$ pulses (`maxlen=200`).
   - AC remotes require expanding the `pulseio.PulseIn` buffer to `maxlen=600` or higher to prevent buffer truncation.
