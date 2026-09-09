#!/usr/bin/env python3
"""Push the Windows default-output volume to the keyboard over vendor HID."""

from __future__ import annotations

import time
from typing import Optional

import hid
from comtypes import CoInitialize
from pycaw.pycaw import AudioUtilities

USB_VID = 0xCAFE
USB_PID = 0x4D4B
VENDOR_USAGE_PAGE = 0xFF00
REPORT_ID_VOLUME = 1
POLL_S = 0.08
RESEND_S = 0.4


def find_vendor_path() -> Optional[bytes]:
    for info in hid.enumerate(USB_VID, USB_PID):
        if (int(info.get("usage_page") or 0) & 0xFFFF) == VENDOR_USAGE_PAGE:
            return info["path"]
    return None


def open_keyboard() -> hid.device:
    path = find_vendor_path()
    if path is None:
        raise OSError("keyboard not found")
    dev = hid.device()
    dev.open_path(path)
    return dev


def read_volume() -> int:
    device = AudioUtilities.GetSpeakers()
    if hasattr(device, "volume_percent"):
        pct = int(round(float(device.volume_percent)))
    else:
        scalar = float(device.EndpointVolume.GetMasterVolumeLevelScalar())
        pct = int(round(scalar * 100.0))
    return max(0, min(100, pct))


def send_volume(dev: hid.device, percent: int) -> None:
    n = dev.write(bytes([REPORT_ID_VOLUME, percent]))
    if n is None or n < 0:
        raise OSError("hid write failed")


def main() -> None:
    try:
        CoInitialize()
    except OSError:
        pass

    dev: Optional[hid.device] = None
    last_vol: Optional[int] = None
    last_sent = 0.0

    while True:
        try:
            if dev is None:
                dev = open_keyboard()
                last_vol = None
                print("Connected", flush=True)

            vol = read_volume()
            now = time.monotonic()
            if vol != last_vol or now - last_sent >= RESEND_S:
                send_volume(dev, vol)
                last_vol = vol
                last_sent = now

        except KeyboardInterrupt:
            break
        except OSError as exc:
            print(f"Waiting: {exc}", flush=True)
            if dev is not None:
                try:
                    dev.close()
                except Exception:
                    pass
                dev = None
            time.sleep(0.5)
            continue

        time.sleep(POLL_S)

    if dev is not None:
        try:
            dev.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()
