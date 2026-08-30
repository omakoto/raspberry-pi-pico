# boot.py
# Configures USB composite device descriptors for Nintendo Switch controller emulation.
# Exposes the HORI Pokken Controller HID Gamepad descriptor alongside standard USB CDC
# serial console and Mass Storage (CIRCUITPY) for development and live monitoring.

import supervisor
import usb_hid

# Configure USB Vendor/Product identification to match HORI Pokken Controller,
# which is natively recognized without pairing prompts on Nintendo Switch OS.
supervisor.set_usb_identification(
    vid=0x0F0D,
    pid=0x0092,
    manufacturer="HORI CO.,LTD.",
    product="POKKEN CONTROLLER"
)

# 8-byte Nintendo Switch Gamepad HID Report Descriptor:
# Byte 0..1 : Buttons bitmask (uint16 little-endian)
# Byte 2    : Hat switch (D-pad: 0=UP, 1=UP_RIGHT, 2=RIGHT, 3=DOWN_RIGHT, 4=DOWN, 5=DOWN_LEFT, 6=LEFT, 7=UP_LEFT, 8=CENTER)
# Byte 3    : Left Stick X (0x00 - 0xFF, center 128)
# Byte 4    : Left Stick Y (0x00 - 0xFF, center 128)
# Byte 5    : Right Stick X (0x00 - 0xFF, center 128)
# Byte 6    : Right Stick Y (0x00 - 0xFF, center 128)
# Byte 7    : Vendor Reserved (0x00)
SWITCH_DESCRIPTOR: bytes = bytes((
    0x05, 0x01,        # Usage Page (Generic Desktop Ctrls)
    0x09, 0x05,        # Usage (Game Pad)
    0xA1, 0x01,        # Collection (Application)
    0x15, 0x00,        #   Logical Minimum (0)
    0x25, 0x01,        #   Logical Maximum (1)
    0x35, 0x00,        #   Physical Minimum (0)
    0x45, 0x01,        #   Physical Maximum (1)
    0x75, 0x01,        #   Report Size (1)
    0x95, 0x10,        #   Report Count (16 buttons)
    0x05, 0x09,        #   Usage Page (Button)
    0x19, 0x01,        #   Usage Minimum (0x01)
    0x29, 0x10,        #   Usage Maximum (0x10)
    0x81, 0x02,        #   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x05, 0x01,        #   Usage Page (Generic Desktop Ctrls)
    0x25, 0x07,        #   Logical Maximum (7)
    0x46, 0x3B, 0x01,  #   Physical Maximum (315)
    0x75, 0x04,        #   Report Size (4)
    0x95, 0x01,        #   Report Count (1)
    0x65, 0x14,        #   Unit (Eng Rot:Angular Pos)
    0x09, 0x39,        #   Usage (Hat switch)
    0x81, 0x42,        #   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,Null State)
    0x65, 0x00,        #   Unit (None)
    0x95, 0x01,        #   Report Count (1)
    0x75, 0x04,        #   Report Size (4)
    0x81, 0x01,        #   Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x26, 0xFF, 0x00,  #   Logical Maximum (255)
    0x46, 0xFF, 0x00,  #   Physical Maximum (255)
    0x09, 0x30,        #   Usage (X - Left Stick X)
    0x09, 0x31,        #   Usage (Y - Left Stick Y)
    0x09, 0x32,        #   Usage (Z - Right Stick X)
    0x09, 0x35,        #   Usage (Rz - Right Stick Y)
    0x75, 0x08,        #   Report Size (8)
    0x95, 0x04,        #   Report Count (4)
    0x81, 0x02,        #   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x75, 0x08,        #   Report Size (8)
    0x95, 0x01,        #   Report Count (1)
    0x81, 0x01,        #   Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0xC0               # End Collection
))

switch_device: usb_hid.Device = usb_hid.Device(
    report_descriptor=SWITCH_DESCRIPTOR,
    usage_page=0x01,
    usage=0x05,
    report_ids=(0,),
    in_report_lengths=(8,),
    out_report_lengths=(0,),
)

# Enable Switch Gamepad HID interface.
# Standard USB CDC serial console and Mass Storage (CIRCUITPY) remain enabled by default.
usb_hid.enable((switch_device,))
