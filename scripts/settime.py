#!/usr/bin/env python
# Set the device clock to the host's time over the serial PhoneAPI.
#
# The board has no battery-backed RTC, so every reboot (and thus every flash)
# loses the clock and messages get stamped timeless until some source sets it.
# flash.sh calls this after an upload so the dev loop never runs clockless; it
# lands as set_time_only -> RTCQualityNTP, so it also outranks a manual entry.
# Usage: settime.py [port]
import sys, threading, time

import meshtastic.serial_interface

# The lib's background reader can choke on a stray boot-log line mid-stream and
# dump a traceback; the admin write itself still goes through. Keep the output clean.
threading.excepthook = lambda args: None

port = sys.argv[1] if len(sys.argv) > 1 else None
try:
    iface = meshtastic.serial_interface.SerialInterface(devPath=port)
    iface.localNode.setTime()  # 0 = host's current time
    time.sleep(1)              # let the admin frame drain before closing
    iface.close()
    print("device clock set to host time")
except Exception as e:
    print(f"settime skipped: {e}", file=sys.stderr)
    sys.exit(1)
