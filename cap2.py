import serial, sys, time
OUT = r"d:\Desktop\jiliguagua\xiaozhi-esp32-guagua-ai-vox3-private-nulllab-AI-VOX3\xiaozhi-esp32-guagua-ai-vox3-private-nulllab-AI-VOX3\boot_capture.log"
try:
    s = serial.Serial('COM6', 115200, timeout=1)
except Exception as e:
    print("OPEN_FAIL:", e); sys.exit(1)
# USB-Serial-JTAG reset: pulse DTR/RTS
s.setDTR(False); s.setRTS(True); time.sleep(0.15)
s.setRTS(False); time.sleep(0.05)
s.reset_input_buffer()
buf = b''
t0 = time.time()
while time.time() - t0 < 20:
    d = s.read(4096)
    if d: buf += d
s.close()
open(OUT, 'w', encoding='utf-8').write(buf.decode('utf-8', errors='replace'))
print("CAPTURED_BYTES", len(buf))
print("OUT", OUT)
