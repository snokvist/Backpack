#!/usr/bin/env python3
"""
Waybeam TX-Backpack host-channel CLI.

Talks to ESP32C3_SuperMini_TX_Backpack_Host firmware over USB-CDC. Framed as
standard CRSF (sync 0xC8, CRC8 poly 0xD5), with Waybeam vendor type 0x7F and
a 1-byte subtype discriminator.

Designed to be host-agnostic — runs on Linux (Radxa, x86), macOS, anywhere
pyserial + Python 3.8+ does.

Usage:
    backpack_host.py listen [--port /dev/ttyACM0]
    backpack_host.py ping   [--port /dev/ttyACM0] [--seq 42]
    backpack_host.py inject --mac aa:bb:cc:dd:ee:ff --hex 00112233 [--port ...]
"""
import argparse
import struct
import sys
import time
from typing import Optional

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial required: pip install pyserial")

CRSF_SYNC = 0xC8
CRSF_POLY = 0xD5
HOST_TYPE = 0x7F

SUB_HEARTBEAT     = 0x01
SUB_ESPNOW_RX     = 0x02
SUB_ESPNOW_TX     = 0x03
SUB_UART_DIAG     = 0x04
SUB_PONG          = 0x91
SUB_INJECT_ESPNOW = 0x10
SUB_PING          = 0x11


# ----- CRC8 (CRSF polynomial 0xD5, same table as Backpack's GENERIC_CRC8) ----
_CRC_TABLE = None

def _build_crc_table(poly: int) -> list:
    table = []
    for i in range(256):
        crc = i
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
        table.append(crc)
    return table

def crc8(data: bytes, init: int = 0) -> int:
    global _CRC_TABLE
    if _CRC_TABLE is None:
        _CRC_TABLE = _build_crc_table(CRSF_POLY)
    crc = init
    for b in data:
        crc = _CRC_TABLE[(crc ^ b) & 0xFF]
    return crc


# ----- Framing --------------------------------------------------------------
def encode(subtype: int, payload: bytes = b"") -> bytes:
    body = bytes([HOST_TYPE, subtype]) + payload
    crc = crc8(body)
    length = len(body) + 1  # +crc
    return bytes([CRSF_SYNC, length]) + body + bytes([crc])


class FrameParser:
    """Streaming CRSF parser. Yields (subtype, payload) tuples from feed()."""
    def __init__(self):
        self._buf = bytearray()

    def feed(self, chunk: bytes):
        self._buf.extend(chunk)
        while True:
            # Resync
            while self._buf and self._buf[0] != CRSF_SYNC:
                self._buf.pop(0)
            if len(self._buf) < 2:
                return
            body_len = self._buf[1]
            if body_len < 3 or body_len > 250:
                self._buf.pop(0)
                continue
            frame_total = 2 + body_len  # sync + len + body
            if len(self._buf) < frame_total:
                return
            frame = bytes(self._buf[:frame_total])
            self._buf = self._buf[frame_total:]
            type_byte = frame[2]
            if type_byte != HOST_TYPE:
                continue  # not ours
            subtype = frame[3]
            payload = frame[4:-1]
            rx_crc = frame[-1]
            if crc8(frame[2:-1]) != rx_crc:
                continue
            yield subtype, payload


# ----- Pretty-printers ------------------------------------------------------
def _fmt_mac(b: bytes) -> str:
    return ":".join(f"{x:02x}" for x in b)

def _fmt_hex(b: bytes, limit: int = 32) -> str:
    head = b[:limit].hex()
    tail = f"... ({len(b)} bytes)" if len(b) > limit else f" ({len(b)} bytes)"
    return head + tail

def pretty(sub: int, p: bytes) -> str:
    if sub == SUB_HEARTBEAT and len(p) >= 5:
        uptime_ms, build = struct.unpack("<IB", p[:5])
        return f"HEARTBEAT uptime={uptime_ms/1000:.1f}s build={build}"
    if sub == SUB_ESPNOW_RX and len(p) >= 8:
        mac = p[:6]; rssi = struct.unpack("b", p[6:7])[0]; ch = p[7]
        return f"ESPNOW_RX src={_fmt_mac(mac)} rssi={rssi} ch={ch} data={_fmt_hex(p[8:])}"
    if sub == SUB_ESPNOW_TX and len(p) >= 7:
        mac = p[:6]; ok = p[6]
        return f"ESPNOW_TX dst={_fmt_mac(mac)} ok={ok} data={_fmt_hex(p[7:])}"
    if sub == SUB_UART_DIAG and len(p) >= 5:
        total = struct.unpack("<I", p[:4])[0]
        sample_len = p[4]
        sample = p[5:5 + sample_len]
        return f"UART_DIAG elrs_bytes={total} ring[{sample_len}]={sample.hex()}"
    if sub == SUB_PONG and len(p) >= 4:
        seq = struct.unpack("<I", p[:4])[0]
        return f"PONG seq={seq}"
    return f"UNKNOWN sub=0x{sub:02x} payload={_fmt_hex(p)}"


# ----- Commands -------------------------------------------------------------
def cmd_listen(port: serial.Serial, duration: Optional[float] = None):
    parser = FrameParser()
    start = time.monotonic()
    while True:
        chunk = port.read(port.in_waiting or 1)
        if chunk:
            for sub, payload in parser.feed(chunk):
                ts = time.monotonic() - start
                print(f"[{ts:8.3f}] {pretty(sub, payload)}", flush=True)
        if duration is not None and time.monotonic() - start >= duration:
            return

def cmd_ping(port: serial.Serial, seq: int, timeout: float = 1.0):
    port.reset_input_buffer()
    port.write(encode(SUB_PING, struct.pack("<I", seq)))
    port.flush()
    parser = FrameParser()
    start = time.monotonic()
    while time.monotonic() - start < timeout:
        chunk = port.read(port.in_waiting or 1)
        for sub, payload in parser.feed(chunk):
            if sub == SUB_PONG and len(payload) >= 4:
                rx_seq = struct.unpack("<I", payload[:4])[0]
                rtt_ms = (time.monotonic() - start) * 1000
                print(f"pong seq={rx_seq} rtt={rtt_ms:.1f}ms")
                return 0
    print("timeout", file=sys.stderr)
    return 1

def cmd_inject(port: serial.Serial, mac: bytes, data: bytes):
    port.write(encode(SUB_INJECT_ESPNOW, mac + data))
    port.flush()
    print(f"injected {len(data)} bytes to {_fmt_mac(mac)}")


def parse_mac(s: str) -> bytes:
    parts = s.replace("-", ":").split(":")
    if len(parts) != 6:
        raise argparse.ArgumentTypeError(f"bad MAC: {s}")
    return bytes(int(p, 16) for p in parts)

def parse_hex(s: str) -> bytes:
    s = s.replace(" ", "").replace(":", "")
    if len(s) % 2:
        raise argparse.ArgumentTypeError("odd hex length")
    return bytes.fromhex(s)


def main():
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--port", default="/dev/ttyACM0",
                        help="serial port (default: /dev/ttyACM0)")

    ap = argparse.ArgumentParser(description="Waybeam TX-Backpack host CLI",
                                 parents=[common])
    sub = ap.add_subparsers(dest="cmd", required=True)

    listen = sub.add_parser("listen", parents=[common],
                            help="print inbound frames until Ctrl-C")
    listen.add_argument("--duration", type=float, default=None)

    ping = sub.add_parser("ping", parents=[common],
                          help="send PING, wait for PONG")
    ping.add_argument("--seq", type=int, default=1)

    inject = sub.add_parser("inject", parents=[common],
                            help="send INJECT_ESPNOW to a peer")
    inject.add_argument("--mac", required=True, type=parse_mac)
    inject.add_argument("--hex", required=True, type=parse_hex)

    args = ap.parse_args()

    # USB-CDC baud is cosmetic; any speed works.
    port = serial.Serial(args.port, 115200, timeout=0.1)
    try:
        if args.cmd == "listen":
            return cmd_listen(port, args.duration) or 0
        if args.cmd == "ping":
            return cmd_ping(port, args.seq)
        if args.cmd == "inject":
            cmd_inject(port, args.mac, args.hex)
            return 0
    except KeyboardInterrupt:
        return 130
    finally:
        port.close()


if __name__ == "__main__":
    sys.exit(main() or 0)
