#!/usr/bin/env python3
# /// script
# dependencies = ["pyserial"]
# ///
"""ZECTRIX NOTE4 跑本项目固件（firmware/）时，从 Mac 键盘翻页。

  Mac 键盘 ──USB 串口──▶ NOTE4 ──BLE HID（伪装 Kindle Click）──▶ Kindle

用法:
  uv run tools/kindle_remote.py                 # 自动找 /dev/cu.usbmodem*
  PORT=/dev/cu.usbmodemXXX uv run tools/kindle_remote.py

按键:  空格 / → / PageDown / j = 下一页   ← / PageUp / k = 上一页
       + / - = 亮度（单步）   U / D = 亮度按住 2 s   s = 让 Kindle 休眠
       0-9 / B = 设电量 0..90% / 100%   ? = 状态   c = 清绑定   q / Ctrl-C = 退出

非交互（没有 tty，例如 Claude Code 的 `!`）：
  uv run tools/kindle_remote.py fff 3     # 发 f f f，再收 3 s 日志
  uv run tools/kindle_remote.py - 60      # 不发命令，只看 60 s 日志（配对时用）

串口保持常开（每条命令重开会让部分板子复位）；打开时 DTR/RTS 都拉低，不复位设备。
设备的日志实时打在下面。
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
        sys.exit('找不到 /dev/cu.usbmodem*；插上设备或用 PORT=... 指定')
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.1
    # S3 USB-Serial/JTAG：RTS=1 且 DTR=0 的瞬间就是芯片复位（esptool 的 hard reset）。
    # macOS 打开 tty 时两根都拉高；pyserial open() 先写 DTR 再写 RTS，预设两者 False 会
    # 经过 (DTR=0,RTS=1) 而复位 [实测 rst:0x15]。所以开着时保持两高，再先降 RTS、后降 DTR。
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
            print('\r\n[串口断开]\r', flush=True)
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
    while True:  # 读完整个转义序列（方向键 3 字节，PageUp/Down 4 字节）
        c = os.read(fd, 1)
        seq += c
        if len(seq) >= 3 and (c.isalpha() or c == b'~'):
            return seq
        if len(seq) > 6:
            return seq


def batch(cmds, wait):
    """非交互：发一串串口命令（每个字符一条，间隔 0.3 s），再收 wait 秒日志后退出。
    给 Claude Code 的 `!`（没有 tty）和脚本化验收用。"""
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
        # 用法: kindle_remote.py [命令串] [等待秒数]，如 `fff 3`、`? 1`、`- 60`（只看日志）
        cmds = sys.argv[1] if len(sys.argv) > 1 else '?'
        batch(cmds.replace('-', ''), float(sys.argv[2]) if len(sys.argv) > 2 else 3)
        return
    s = open_port()
    print(f'已连接 {s.port}。空格/→ 下一页，←上一页，+/- 亮度，s 休眠，? 状态，q 退出', flush=True)
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
