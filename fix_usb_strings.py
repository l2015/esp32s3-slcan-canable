"""
Post-build script: Inject CANable USB string descriptors.

Sets ARDUINO_USB_MANUFACTURER and ARDUINO_USB_PRODUCT macros so that
the compiled firmware reports correct USB manufacturer/product strings
(CANable) instead of the default Espressif strings.

This runs as a PlatformIO 'post' script after CPPDEFINES are resolved.
"""

Import("env")

CANABLE_MANUFACTURER = "CANable"
CANABLE_PRODUCT = "CANable 2 Slcan"

existing = env.get("CPPDEFINES", [])

# Remove any existing ARDUINO_USB_MANUFACTURER / ARDUINO_USB_PRODUCT
filtered = []
for item in existing:
    name = item[0] if isinstance(item, tuple) else item
    if name in ("ARDUINO_USB_MANUFACTURER", "ARDUINO_USB_PRODUCT"):
        continue
    filtered.append(item)

filtered += [
    ("ARDUINO_USB_MANUFACTURER", '"' + CANABLE_MANUFACTURER + '"'),
    ("ARDUINO_USB_PRODUCT",       '"' + CANABLE_PRODUCT + '"'),
]

env.Replace(CPPDEFINES=filtered)
print("[fix_usb_strings.py] Added USB strings: manufacturer='%s', product='%s'" % (CANABLE_MANUFACTURER, CANABLE_PRODUCT))
