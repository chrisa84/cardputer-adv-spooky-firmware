"""Talk to Spooky Radio on a Cardputer ADV over USB serial.

Spooky answers these while its menu is showing.

  python tools/spooky.py --port COM12 ls
      List the SD card.
  python tools/spooky.py --port COM12 put .pio/build/spooky/firmware.bin spooky-radio.bin
      Copy a file onto the SD card. It is hash-checked on the device, and the old file is only replaced once the
      new one checks out.
  python tools/spooky.py --port COM12 vault-status
      Show whether the chip has a vault key and a stored network.
  python tools/spooky.py --port COM12 burn-key
      ONE-TIME and PERMANENT: burns a 256-bit HMAC key into eFuse BLOCK_KEY5. Software can never read it back.
  python tools/spooky.py --port COM12 wifi
      Store the home Wi-Fi in the encrypted vault. Prompts for SSID and password.

The password is never written to a file and never echoed. If you set the environment variables WIFI_SSID,
WIFI_PASSWORD and VAULT_KEY (64 hex characters) before running, it uses those instead of prompting. Nothing
leaves this PC except over the USB cable.

Run with: uvx --with pyserial --with "bitstring<4.3" --from esptool==5.1.0 python -I tools/spooky.py ...
(esptool is only needed for burn-key.)
"""
import argparse
import getpass
import hashlib
import os
import secrets
import subprocess
import sys
import tempfile
import time

import serial

KEY_BLOCK = 'BLOCK_KEY5'


def open_port(port):
    # Default DTR/RTS: the S3's USB serial only sends once the host asserts DTR, and opening this way doesn't reset it.
    s = serial.Serial(port, 115200, timeout=5, write_timeout=10)
    time.sleep(0.2)
    s.reset_input_buffer()
    return s


def reply(s, prefixes, timeout=10):
    """Return the first line that starts with one of the prefixes, skipping log lines."""
    end = time.time() + timeout
    while time.time() < end:
        t = s.readline().decode(errors='replace').strip()
        if any(t.startswith(p) for p in prefixes):
            return t
    sys.exit(f'no reply from Spooky on {s.port}. Is the Cardputer on the Spooky Radio menu (not inside a demo)?')


def cmd_ls(a):
    s = open_port(a.port)
    s.write(b'ls\n')
    end = time.time() + 10
    while time.time() < end:
        t = s.readline().decode(errors='replace').rstrip()
        if t.startswith('  '):
            print(t)
        elif t.startswith(('ok', 'err')):
            if t.startswith('err'):
                print(t)
            return
    sys.exit('no reply: is Spooky on its menu?')


def cmd_put(a):
    data = open(a.src, 'rb').read()
    digest = hashlib.sha256(data).hexdigest()
    s = open_port(a.port)
    s.write(f'put {a.name} {len(data)} {digest}\n'.encode())
    r = reply(s, ('ready', 'err'))
    if r != 'ready':
        sys.exit(r)
    start = time.time()
    for i in range(0, len(data), 4096):
        s.write(data[i:i + 4096])
        ack = s.read(1)
        if ack != b'\x06':
            sys.exit(f'no ack at byte {i}: {ack!r} {s.read(200)!r}')
        print(f'\r{min(len(data), i + 4096) * 100 // len(data)}%', end='', flush=True)
    r = reply(s, ('ok', 'err'), 60)
    print(f'\r{a.name}: {len(data)} B, {r}, {time.time() - start:.1f} s')
    if not r.startswith('ok'):
        sys.exit(1)


def cmd_vault_status(a):
    s = open_port(a.port)
    s.write(b'vault status\n')
    print(reply(s, ('vault',)))


def cmd_burn_key(a):
    key_hex = os.environ.get('VAULT_KEY') or getpass.getpass('Vault key, 64 hex characters (empty = generate one): ')
    key_hex = key_hex.strip().lower()
    if not key_hex:
        key_hex = secrets.token_hex(32)
        print('Generated a random key. You never need it again: the chip uses it but can\'t reveal it.')
    if len(key_hex) != 64 or any(c not in '0123456789abcdef' for c in key_hex):
        sys.exit('the key must be exactly 64 hex characters (32 bytes)')
    print(f'\nThis PERMANENTLY burns the key into eFuse {KEY_BLOCK} on the chip on {a.port}, purpose HMAC_UP,')
    print('read-protected. It cannot be changed, read back or undone. It does not affect booting, flashing or Launcher.')
    if input('Type BURN to continue: ').strip() != 'BURN':
        sys.exit('cancelled, nothing written')
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, 'k.bin')
        with open(path, 'wb') as f:
            f.write(bytes.fromhex(key_hex))
        try:
            subprocess.run([sys.executable, '-m', 'espefuse', '--chip', 'esp32s3', '--port', a.port, '--do-not-confirm',
                            'burn-key', KEY_BLOCK, path, 'HMAC_UP'], check=True)
        finally:
            with open(path, 'wb') as f:  # overwrite before the temp dir is removed
                f.write(b'\0' * 32)
    print('Burned. Switch the Cardputer off and on, open Spooky, then run: wifi')


def cmd_wifi(a):
    ssid = os.environ.get('WIFI_SSID') or input('Wi-Fi network name (2.4 GHz): ').strip()
    pw = os.environ.get('WIFI_PASSWORD') or getpass.getpass('Password (not shown): ')
    if not ssid or len(ssid.encode()) > 32 or len(pw.encode()) > 64:
        sys.exit('SSID must be 1-32 bytes and the password at most 64')
    s = open_port(a.port)
    s.write(f'vault set {ssid.encode().hex()} {pw.encode().hex()}\n'.encode())
    print(reply(s, ('vault',)))
    s.write(b'vault status\n')
    print(reply(s, ('vault',)))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--port', default='COM12')
    sub = p.add_subparsers(dest='cmd', required=True)
    sub.add_parser('ls')
    put = sub.add_parser('put')
    put.add_argument('src')
    put.add_argument('name')
    sub.add_parser('vault-status')
    sub.add_parser('burn-key')
    sub.add_parser('wifi')
    a = p.parse_args()
    {'ls': cmd_ls, 'put': cmd_put, 'vault-status': cmd_vault_status, 'burn-key': cmd_burn_key,
     'wifi': cmd_wifi}[a.cmd](a)


if __name__ == '__main__':
    main()
