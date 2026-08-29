#!/usr/bin/env python3
"""Pulse ESP32 EN via RTS on COM18 and capture USB serial until a boot milestone.

Guest console is muxed onto USB CDC (vax_console::put_guest / harvest_keys),
so this script can also type `boot netbsd` when /boot stops after netbsd.vax.
"""

from __future__ import annotations

import argparse
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    import serial
except ImportError as exc:
    raise SystemExit("pyserial is required: python -m pip install pyserial") from exc


def hard_reset(ser: serial.Serial) -> None:
    # Pulse EN (RTS); keep DTR clear so GPIO0 is not held in download mode.
    # USB-Serial/JTAG on the S3 needs a longer EN low than classic UART.
    print("=== hard reset via RTS/EN (DTR clear) ===", flush=True)
    ser.dtr = False
    ser.rts = False
    time.sleep(0.05)
    ser.rts = True
    time.sleep(0.5)
    ser.rts = False
    ser.dtr = False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM18")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=1800.0,
                    help="seconds to capture after reset")
    ap.add_argument("--min-seconds", type=float, default=0.0,
                    help="ignore stop needles until this many seconds have elapsed")
    ap.add_argument("--no-reset", action="store_true")
    ap.add_argument("--no-boot-netbsd", action="store_true")
    ap.add_argument("--log", default="",
                    help="output log path (default: tools/_boot_capture.txt)")
    args = ap.parse_args()

    log_path = Path(args.log) if args.log else (
        Path(__file__).resolve().parent / "_boot_capture.txt"
    )

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()

    if not args.no_reset:
        hard_reset(ser)
        # Do not drain the input buffer: the version banner and selftest
        # HALT land in the first few hundred milliseconds.
        time.sleep(0.15)

    # Selftest prints "VAX HALT at PC=" before PASS; only treat a HALT
    # after the guest is running as a stop.
    stop_needles = (
        b"/etc/rc: xot",
        b"/etc/rc: ms:",
        b"/etc/rc.subr:",
        b"terminated abnormally",
        b"VAX reserved inst",
        b"Enter pathname of shell",
        b"Stopped in pid",
        b"panic: SEGV",
        b"boot stop:",
        b"login:",
    )
    typed_boot = False
    poked_cd = False
    poke_t = 0.0
    buf = bytearray()
    t0 = time.monotonic()
    print(f"=== monitor {args.port} timeout={args.timeout:.0f}s log={log_path} ===",
          flush=True)

    with log_path.open("wb") as logf:
        while time.monotonic() - t0 < args.timeout:
            chunk = ser.read(4096)
            if chunk:
                logf.write(chunk)
                logf.flush()
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
                buf.extend(chunk)
                if len(buf) > 256 * 1024:
                    del buf[: len(buf) - 128 * 1024]

            # Match the manual console: CR aborts the /boot countdown, then
            # `boot netbsd` at the prompt. Never type after the kernel is in S0
            # (a late inject stalled uda STEP1 on unattended captures).
            in_kernel = (b"enter S0" in buf or b"NetBSD 10.1 (GENERIC)" in buf
                         or b"mainbus0 (root)" in buf)
            if (not args.no_boot_netbsd and not poked_cd and not in_kernel
                    and b"abort autoboot" in buf):
                print("\n=== inject CR (abort autoboot) ===", flush=True)
                ser.write(b"\r")
                ser.flush()
                poked_cd = True
                poke_t = time.monotonic()
            if (not args.no_boot_netbsd and not typed_boot and not in_kernel
                    and (b"Device not configured" in buf
                         or (poked_cd and time.monotonic() - poke_t >= 1.5))):
                print("\n=== inject boot netbsd ===", flush=True)
                ser.write(b"boot netbsd\r")
                ser.flush()
                typed_boot = True

            guest = buf.find(b"guest running")
            halt_after_guest = (
                guest >= 0 and b"VAX HALT at PC=" in buf[guest:]
            )
            elapsed = time.monotonic() - t0
            if elapsed >= args.min_seconds and (
                    any(n in buf for n in stop_needles) or halt_after_guest):
                # Keep a few extra seconds so trailing lines land.
                extra_until = time.monotonic() + 40.0
                while time.monotonic() < extra_until:
                    more = ser.read(4096)
                    if more:
                        logf.write(more)
                        logf.flush()
                        sys.stdout.buffer.write(more)
                        sys.stdout.buffer.flush()
                        buf.extend(more)
                print("\n=== stop needle matched ===", flush=True)
                break
        else:
            print("\n=== capture timeout ===", flush=True)

    ser.close()
    text = buf.decode("latin-1", errors="replace")
    print(f"\n=== capture done {datetime.now().isoformat(timespec='seconds')} "
          f"bytes={log_path.stat().st_size} typed_boot={typed_boot} ===",
          flush=True)
    for label, needle in (
        ("version", "[vVax] vVax V"),
        ("plant", "vax_mem_bus_space @"),
        ("uba", "uba1 at cmi0"),
        ("uda", "uda0 at uba1"),
        ("ra0", "ra0 at mscpbus"),
        ("root", "root on ra0a"),
        ("xot", "/etc/rc: xot"),
        ("reserved", "VAX reserved inst"),
        ("fpa", "fpa: op="),
        ("rc.subr", "/etc/rc.subr:"),
        ("user_mmgt", "user mmgt"),
        ("abnormal", "terminated abnormally"),
        ("single_user", "Enter pathname of shell"),
        ("panic", "panic:"),
        ("halt", "VAX HALT"),
        ("guest_halt", "GUEST_HALT_PLACEHOLDER"),
    ):
        if label == "guest_halt":
            gi = text.find("guest running")
            hit = gi >= 0 and "VAX HALT" in text[gi:]
            print(f"  {label}: {'yes' if hit else 'no'}", flush=True)
            continue
        print(f"  {label}: {'yes' if needle in text else 'no'}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
