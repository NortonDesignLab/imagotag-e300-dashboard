import serial, time
s = serial.Serial('/dev/serial/by-id/usb-1a86_USB_Single_Serial_5B91070769-if00', 115200, timeout=0.2)
# ESP32 auto-reset: RTS->EN, DTR->IO0. Keep IO0 high (run app), pulse EN low.
s.setDTR(False)      # IO0 high -> normal boot
s.setRTS(True)       # EN low  -> hold in reset
time.sleep(0.15)
s.reset_input_buffer()
s.setRTS(False)      # EN high -> release, boot app
end = time.time() + 130
buf = b""
while time.time() < end:
    buf += s.read(512)
s.close()
print(buf.decode('utf-8','replace'))
