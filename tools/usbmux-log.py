#!/usr/bin/env python3
"""The Pixel 8's kernel log while HakiOS runs, over the USB cable (usbmux=1): through usbmuxd's socket to the phone's
port 1, where the kernel's usbmux (drivers/usb/hakios.akita_usbmux.c) serves its message buffer -- all of it, then
what is added, until this stops.

usage: usbmux-log.py [-k] [OUTPUT]      (stdout, or OUTPUT as well; needs usbmuxd running, and access to its socket)
       -k: the kernel message buffer (port 5: printf and IOLog, which without -v are not on the console)

usbmuxd: Linux's socket /var/run/usbmuxd, or else Windows' Apple Mobile Device Service (iTunes, Apple Devices) at
127.0.0.1:27015 -- from WSL, run this with Windows' Python, which reaches it and the USB port with no usbipd:
  python.exe "$(wslpath -w devices/shiba/tools/usbmux-log.py)" run.txt
It waits for the phone (started before HakiOS is, it has the log from the first line the kernel still keeps).
"""
import os
import plistlib
import socket
import struct
import sys
import time


def mux_socket():
    if os.path.exists('/var/run/usbmuxd'):
        s = socket.socket(socket.AF_UNIX)
        s.connect('/var/run/usbmuxd')
    else:
        s = socket.create_connection(('127.0.0.1', 27015))
    return s


def recv_all(s, n):
    data = b''
    while len(data) < n:
        part = s.recv(n - len(data))
        if not part:
            raise EOFError('usbmuxd closed the connection')
        data += part
    return data


def mux_answer(s):
    n = struct.unpack('<I', recv_all(s, 16)[:4])[0]
    return plistlib.loads(recv_all(s, n - 16))


def mux_connect(port):
    said = False
    while True:
        s = mux_socket()
        s.sendall(struct.pack('<IIII', 16 + len(b := plistlib.dumps({'MessageType': 'ListDevices'})), 1, 8, 1) + b)
        devices = mux_answer(s).get('DeviceList', [])
        s.close()
        if devices:
            break
        if not said:
            print('usbmux-log: waiting for the phone (HakiOS with usbmux=1, on the cable)', file=sys.stderr)
            said = True
        time.sleep(1)
    for d in devices:
        props = d.get('Properties', {})
        print('usbmux-log: device %s, serial %s' % (d['DeviceID'], props.get('SerialNumber', '?')), file=sys.stderr)
    device = devices[0]['DeviceID']
    s = mux_socket()
    body = plistlib.dumps({'MessageType': 'Connect', 'DeviceID': device, 'PortNumber': socket.htons(port)})
    s.sendall(struct.pack('<IIII', 16 + len(body), 1, 8, 2) + body)
    result = mux_answer(s)
    if result.get('Number') != 0:
        sys.exit(f'usbmux-log: the phone refused port {port}: {result}')
    return s


args = sys.argv[1:]
port = 5 if args[:1] == ['-k'] else 1
args = args[1:] if port == 5 else args
out = open(args[0], 'ab') if args else None
s = mux_connect(port)
try:
    while data := s.recv(65536):
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
        if out:
            out.write(data)
            out.flush()
except KeyboardInterrupt:
    pass
