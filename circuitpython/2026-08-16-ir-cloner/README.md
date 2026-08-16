# Raspberry Pi Pico Universal IR Cloner & Blaster

A CircuitPython project that records any infrared remote control signal (NEC, Sony SIRC, Samsung, RC-5, HVAC, or raw pulses) using an IR receiver on **GP19**, and replays the recorded signal via an IR transmitter/blaster on **GP20** whenever a push button on **GP17** is pressed.

## Hardware Wiring

| Component | Pico Pin | Physical Pin | Description |
| :--- | :--- | :--- | :--- |
| **IR Receiver DATA / OUT** | **GP19** | **Pin 25** | Receives & captures incoming IR signals |
| **IR Blaster DATA / IN** | **GP20** | **Pin 26** | Modulates carrier wave and transmits IR LED pulses |
| **Trigger Push Button** | **GP17** | **Pin 22** | Press to blast the active cloned signal (Active LOW, internal pull-up) |
| **Ground Common** | **GND** | **Pin 23 / 28** | Connected to GND of receiver, blaster, and button |
| **Power Supply** | **3V3 OUT** | **Pin 36** | Power for receiver and blaster (or 5V VBUS on Pin 40 for higher range) |

---

## How It Works

1. **Record**: Point any remote control at the receiver on **GP19** and press a button.
2. **Auto-Detect Protocol & Carrier**:
   - Detects **Sony SIRC** $\rightarrow$ Configures transmitter for **40 kHz** carrier and 3x repeated bursts.
   - Detects **NEC** $\rightarrow$ Configures transmitter for **38 kHz** carrier.
   - Any other signal $\rightarrow$ Captures raw microsecond waveform and configures transmitter for **38 kHz**.
3. **Blast**: Press the push button connected to **GP17** to retransmit the exact recorded waveform via the IR blaster on **GP20**.

---

## How to Run

1. Run the script directly:
   ```bash
   /home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-ir-cloner/ir-cloner.py
   ```
   Or copy to your CIRCUITPY drive:
   ```bash
   cp /home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-ir-cloner/ir-cloner.py /run/media/omakoto/CIRCUITPY/code.py
   ```
2. Open a serial monitor (`picocom -b 115200 /dev/ttyACM0`) to see real-time capture and blast confirmations.
