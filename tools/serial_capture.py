import serial
import time
import sys

port = sys.argv[1] if len(sys.argv) > 1 else "COM21"
duration = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0

s = serial.Serial(port, 115200, timeout=0.5)
time.sleep(0.3)
s.setDTR(False)
s.setRTS(True)
time.sleep(0.1)
s.setRTS(False)
time.sleep(0.2)

end = time.time() + duration
while time.time() < end:
    data = s.read(4096)
    if data:
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()
