#!/usr/bin/env python3
# /// script
# dependencies = ["pyserial"]
# ///
"""Turn Kindle pages from the Mac keyboard while the ZECTRIX NOTE4 runs this project's firmware (firmware/).

  Mac keyboard ──USB serial──▶ NOTE4 ──BLE HID (posing as Kindle Click)──▶ Kindle

Usage:
  uv run tools/kindle_remote.py                 # auto-detects /dev/cu.usbmodem*
  PORT=/dev/cu.usbmodemXXX uv run tools/kindle_remote.py

Keys:  Space / → / PageDown / j = next page   ← / PageUp / k = previous page
       + / - = brightness (one step)   U / D = hold brightness 2 s   s = put Kindle to sleep
       0-9 / B = set battery 0..90% / 100%   ? = status   c = clear bonds   q / Ctrl-C = quit

Non-interactive (no tty, e.g. CI or called from another program):
  uv run tools/kindle_remote.py fff 3     # send f f f, then collect 3 s of logs
  uv run tools/kindle_remote.py - 60      # send nothing, just watch 60 s of logs (for pairing)

The serial port stays open (reopening per command resets some boards); DTR/RTS are lowered
on open without resetting the device. Device logs stream below.
"""
import glob, os, sys, termios, threading, tty

import serial

KEYMAP = {
    b' ': b'f', b'\x1b[C': b'f', b'\x1b[6~': b'f', b'j': b'f', b'f': b'f',
    b'\x1b[D': b'b', b'\x1b[5~': b'b', b'k': b'b', b'b': b'b',
    b'+': b'u', b'=': b'u', b'-': b'd', b'_': b'd',
    b'U': b'U', b'D': b'D', b'F': b'F', b's': b's', b'?': b'?', b'c': b'c', b'B': b'B', b'h': b'h',
    **{str(i).encode(): str(i).encode() for i in range(10)},
}


def open_port():
    port = os.environ.get('PORT') or next(iter(sorted(glob.glob('/dev/cu.usbmodem*'))), None)
    if not port:
        sys.exit('No /dev/cu.usbmodem* found; plug in the device or set PORT=...')
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.1
    # S3 USB-Serial/JTAG: the moment RTS=1 and DTR=0 is a chip reset (esptool's hard reset).
    # macOS raises both when opening the tty; pyserial open() writes DTR before RTS, so presetting
    # both False passes through (DTR=0,RTS=1) and resets [observed rst:0x15]. So open with both
    # high, then lower RTS first, DTR second.
    s.dtr = True
    s.rts = True
    s.open()
    s.rts = False
    s.dtr = False
    return s


def reader(s, stop):
    while not stop.is_set():
        try:
            data = s.read(256)
        except (serial.SerialException, OSError):
            print('\r\n[serial disconnected]\r', flush=True)
            stop.set()
            return
        if data:
            sys.stdout.write(data.decode('utf-8', 'replace').replace('\n', '\r\n'))
            sys.stdout.flush()


def read_key(fd):
    b = os.read(fd, 1)
    if b != b'\x1b':
        return b
    seq = b
    while True:  # read the whole escape sequence (arrow keys 3 bytes, PageUp/Down 4 bytes)
        c = os.read(fd, 1)
        seq += c
        if len(seq) >= 3 and (c.isalpha() or c == b'~'):
            return seq
        if len(seq) > 6:
            return seq


def batch(cmds, wait):
    """Non-interactive: send a string of serial commands (one per character, 0.3 s apart),
    then collect logs for `wait` seconds and exit. For environments without a tty and scripted checks."""
    import time
    s = open_port()
    end = time.time() + 0.3
    for ch in cmds:
        while time.time() < end:
            sys.stdout.write(s.read(256).decode('utf-8', 'replace'))
        s.write(ch.encode())
        end = time.time() + 0.3
    end = time.time() + wait
    while time.time() < end:
        sys.stdout.write(s.read(256).decode('utf-8', 'replace'))
        sys.stdout.flush()
    s.close()


def main():
    if len(sys.argv) > 1 or not sys.stdin.isatty():
        # usage: kindle_remote.py [commands] [wait seconds], e.g. `fff 3`, `? 1`, `- 60` (logs only)
        cmds = sys.argv[1] if len(sys.argv) > 1 else '?'
        batch(cmds.replace('-', ''), float(sys.argv[2]) if len(sys.argv) > 2 else 3)
        return
    s = open_port()
    print(f'Connected to {s.port}. Space/→ next page, ← previous, +/- brightness, s sleep, ? status, q quit', flush=True)
    stop = threading.Event()
    threading.Thread(target=reader, args=(s, stop), daemon=True).start()
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        s.write(b'?')
        while not stop.is_set():
            k = read_key(fd)
            if k in (b'q', b'\x03'):
                break
            cmd = KEYMAP.get(k)
            if cmd:
                s.write(cmd)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
        stop.set()
        s.close()
        print()


if __name__ == '__main__':
    main()
