#!/usr/bin/env python3
"""Push Windows volume + now-playing info to the keyboard over vendor HID."""

from __future__ import annotations

import asyncio
import time
import unicodedata
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Optional

import hid
from comtypes import CoInitialize
from pycaw.pycaw import AudioUtilities

try:
    from winrt.windows.media.control import (
        GlobalSystemMediaTransportControlsSessionManager as SessionManager,
        GlobalSystemMediaTransportControlsSessionPlaybackStatus as PlaybackStatus,
    )
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "Missing WinRT media bindings. Install host/requirements.txt "
        "(winrt-Windows.Media.Control)."
    ) from exc

USB_VID = 0xCAFE
USB_PID = 0x4D4B
VENDOR_USAGE_PAGE = 0xFF00
REPORT_ID_VOLUME = 1
REPORT_ID_MEDIA = 2
MEDIA_REPORT_LEN = 48
MEDIA_TITLE_LEN = 21
MEDIA_ARTIST_LEN = 21
MEDIA_FLAG_ACTIVE = 0x01
MEDIA_FLAG_PLAYING = 0x02
MEDIA_FLAG_TIMELINE = 0x04

POLL_S = 0.08
RESEND_S = 0.4
MEDIA_RESEND_S = 2.0
MEDIA_PLAYING_SEND_S = 0.25
# Bias slightly behind wall-clock so the OLED does not lead the Windows flyout.
MEDIA_LEAD_COMPENSATION_S = 0.35


@dataclass(frozen=True)
class MediaInfo:
    active: bool
    playing: bool
    timeline: bool
    position_s: int
    duration_s: int
    title: str
    artist: str


EMPTY_MEDIA = MediaInfo(
    active=False,
    playing=False,
    timeline=False,
    position_s=0,
    duration_s=0,
    title="",
    artist="",
)


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


def _oled_field(text: str, length: int) -> bytes:
    cleaned = unicodedata.normalize("NFKD", text or "")
    cleaned = cleaned.encode("ascii", "ignore").decode("ascii")
    cleaned = " ".join(cleaned.split())
    return cleaned[:length].encode("ascii").ljust(length, b"\0")


def _timespan_seconds(value) -> float:
    if value is None:
        return 0.0
    if hasattr(value, "total_seconds"):
        return max(0.0, float(value.total_seconds()))
    duration = getattr(value, "duration", None)
    if duration is None:
        return 0.0
    return max(0.0, float(duration) / 10_000_000.0)


def _effective_position_s(timeline, playing: bool) -> float:
    """SMTC position often freezes while playing; advance from LastUpdatedTime."""
    pos = _timespan_seconds(getattr(timeline, "position", None))
    if not playing:
        return pos

    last_updated = getattr(timeline, "last_updated_time", None)
    if last_updated is None:
        return pos

    if getattr(last_updated, "tzinfo", None) is None:
        last_updated = last_updated.replace(tzinfo=timezone.utc)

    lag = (datetime.now(timezone.utc) - last_updated).total_seconds()
    if lag < 0.0:
        lag = 0.0
    # Guard against broken clocks / abandoned sessions.
    if lag > 6 * 3600:
        return pos
    return max(0.0, pos + lag - MEDIA_LEAD_COMPENSATION_S)


async def _fetch_media(manager: SessionManager) -> MediaInfo:
    session = manager.get_current_session()
    if session is None:
        return EMPTY_MEDIA

    playback = session.get_playback_info()
    status = playback.playback_status if playback is not None else None
    playing = status == PlaybackStatus.PLAYING

    props = await session.try_get_media_properties_async()
    title = (props.title or "").strip() if props is not None else ""
    artist = ""
    if props is not None:
        artist = (props.artist or props.album_artist or "").strip()

    timeline = session.get_timeline_properties()
    position_s = _effective_position_s(timeline, playing)
    start_s = _timespan_seconds(getattr(timeline, "start_time", None))
    end_s = _timespan_seconds(getattr(timeline, "end_time", None))
    duration_s = max(0.0, end_s - start_s)
    has_timeline = duration_s > 0.0 or position_s > 0.0
    if has_timeline and duration_s > 0.0 and position_s > duration_s:
        position_s = duration_s

    active = bool(title or artist or playing or has_timeline)
    if not active:
        return EMPTY_MEDIA

    if not title:
        title = "Unknown"

    return MediaInfo(
        active=True,
        playing=playing,
        timeline=has_timeline,
        position_s=min(int(position_s), 0xFFFF),
        duration_s=min(int(duration_s), 0xFFFF),
        title=title,
        artist=artist,
    )


def pack_media(info: MediaInfo) -> bytes:
    flags = 0
    if info.active:
        flags |= MEDIA_FLAG_ACTIVE
    if info.playing:
        flags |= MEDIA_FLAG_PLAYING
    if info.timeline:
        flags |= MEDIA_FLAG_TIMELINE

    payload = bytearray(MEDIA_REPORT_LEN)
    payload[0] = flags
    payload[1] = info.position_s & 0xFF
    payload[2] = (info.position_s >> 8) & 0xFF
    payload[3] = info.duration_s & 0xFF
    payload[4] = (info.duration_s >> 8) & 0xFF
    payload[5 : 5 + MEDIA_TITLE_LEN] = _oled_field(info.title, MEDIA_TITLE_LEN)
    payload[5 + MEDIA_TITLE_LEN : 5 + MEDIA_TITLE_LEN + MEDIA_ARTIST_LEN] = _oled_field(
        info.artist, MEDIA_ARTIST_LEN
    )
    return bytes(payload)


def send_media(dev: hid.device, payload: bytes) -> None:
    n = dev.write(bytes([REPORT_ID_MEDIA]) + payload)
    if n is None or n < 0:
        raise OSError("hid write failed")


def media_changed(a: MediaInfo, b: MediaInfo) -> bool:
    return (
        a.active != b.active
        or a.playing != b.playing
        or a.timeline != b.timeline
        or a.title != b.title
        or a.artist != b.artist
        or a.duration_s != b.duration_s
        or a.position_s != b.position_s
    )


def main() -> None:
    try:
        CoInitialize()
    except OSError:
        pass

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    manager: Optional[SessionManager] = None

    dev: Optional[hid.device] = None
    last_vol: Optional[int] = None
    last_vol_sent = 0.0
    last_media = EMPTY_MEDIA
    last_media_payload = pack_media(EMPTY_MEDIA)
    last_media_sent = 0.0

    while True:
        try:
            if manager is None:
                manager = loop.run_until_complete(SessionManager.request_async())

            if dev is None:
                dev = open_keyboard()
                last_vol = None
                last_media = EMPTY_MEDIA
                last_media_payload = pack_media(EMPTY_MEDIA)
                print("Connected", flush=True)

            now = time.monotonic()
            vol = read_volume()
            if vol != last_vol or now - last_vol_sent >= RESEND_S:
                send_volume(dev, vol)
                last_vol = vol
                last_vol_sent = now

            media = loop.run_until_complete(_fetch_media(manager))
            payload = pack_media(media)
            due = MEDIA_PLAYING_SEND_S if media.playing else MEDIA_RESEND_S
            if (
                media_changed(media, last_media)
                or payload != last_media_payload
                or now - last_media_sent >= due
            ):
                send_media(dev, payload)
                last_media = media
                last_media_payload = payload
                last_media_sent = now

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
        except Exception as exc:
            # Media APIs can fail transiently; keep volume sync alive.
            print(f"Media: {exc}", flush=True)
            manager = None
            time.sleep(0.5)
            continue

        time.sleep(POLL_S)

    if dev is not None:
        try:
            dev.close()
        except Exception:
            pass
    loop.close()


if __name__ == "__main__":
    main()
