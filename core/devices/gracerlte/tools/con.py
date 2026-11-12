# con.py COMMAND SECONDS: send a console command to the phone's USB serial and print what comes back for SECONDS
import os, sys, time, glob, termios, tty, select
d = sorted(glob.glob('/dev/cu.usbmodem*'))[0]
fd = os.open(d, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
tty.setraw(fd)
os.write(fd, (sys.argv[1] + '\r').encode())
end = time.time() + float(sys.argv[2]); out = b''
while time.time() < end:
    if select.select([fd], [], [], 0.5)[0]:
        try: out += os.read(fd, 65536)
        except BlockingIOError: pass
sys.stdout.write(out.decode('utf8', 'replace'))
