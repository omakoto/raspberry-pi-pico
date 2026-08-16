# boot.py
# Configures USB CDC interfaces and filesystem read-write permissions at boot.
import board
import digitalio
import storage
import supervisor
import usb_cdc

# Customize the USB manufacturer and product names shown by the OS / Web Serial chooser
supervisor.set_usb_identification(
    manufacturer="Raspberry Pi",
    product="Pico Config Portal"
)

# Enable both console (REPL) and secondary data CDC ports
usb_cdc.enable(console=True, data=True)

# Set up GP14 as an input with internal pull-up.
# Jumper GP14 to GND to make filesystem writable by PC (Default Mode).
# Leave GP14 open (disconnected) to make filesystem writable by CircuitPython.
write_switch: digitalio.DigitalInOut = digitalio.DigitalInOut(board.GP14)
write_switch.direction = digitalio.Direction.INPUT
write_switch.pull = digitalio.Pull.UP

if write_switch.value:
    # GP14 is High (disconnected) -> Writable by CircuitPython, read-only to PC
    storage.remount("/", readonly=False)
else:
    # GP14 is Low (grounded) -> Read-only to CircuitPython, writable by PC
    storage.remount("/", readonly=True)
