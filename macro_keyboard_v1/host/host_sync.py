#!/usr/bin/env python3
"""Push Windows volume, now-playing, and Discord mute/deafen to the keyboard."""

from __future__ import annotations

import asyncio
import json
import os
import re
import subprocess
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
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

from discord_voice import DiscordVoice

USB_VID = 0xCAFE
USB_PID = 0x4D4B
VENDOR_USAGE_PAGE = 0xFF00
REPORT_ID_VOLUME = 1
REPORT_ID_MEDIA = 2
REPORT_ID_DISCORD = 3
REPORT_ID_HOSTCMD = 4
REPORT_ID_MEDIA_TITLE = 5
REPORT_ID_MEDIA_ARTIST = 6
HOST_CMD_OPEN_SPOTIFY = 1
MEDIA_META_LEN = 5
MEDIA_TITLE_LEN = 60
MEDIA_ARTIST_LEN = 60
MEDIA_FLAG_ACTIVE = 0x01
MEDIA_FLAG_PLAYING = 0x02
MEDIA_FLAG_TIMELINE = 0x04
DISCORD_FLAG_OPEN = 0x01
DISCORD_FLAG_MUTED = 0x02
DISCORD_FLAG_DEAF = 0x04

POLL_S = 0.08
RESEND_S = 0.4
MEDIA_RESEND_S = 2.0
MEDIA_PLAYING_SEND_S = 0.25
# Bias slightly behind wall-clock so the OLED does not lead the Windows flyout.
MEDIA_LEAD_COMPENSATION_S = 0.35
DISCORD_SEND_S = 0.35

# Spotify/others often put extra artists in the title: "Song (feat. A, B)"
_FEAT_RE = re.compile(
    r"\s*[\(\[]\s*(?:feat\.?|ft\.?|featuring|with)\s+([^\)\]]+?)[\)\]]\s*",
    re.IGNORECASE,
)
_ARTIST_SPLIT_RE = re.compile(r"\s*,\s*|\s*;\s*|\s+&\s+|\s+/\s+|\s+x\s+", re.IGNORECASE)

# SMTC often only has the primary artist; iTunes search usually has full credits.
_itunes_credit_cache: dict[str, str] = {}
_itunes_miss_until: dict[str, float] = {}


def _split_artist_names(text: str) -> list[str]:
    if not text:
        return []
    return [p.strip() for p in _ARTIST_SPLIT_RE.split(text) if p.strip()]


def _add_unique(names: list[str], candidates: list[str]) -> None:
    for name in candidates:
        if not name:
            continue
        if any(name.lower() == existing.lower() for existing in names):
            continue
        names.append(name)


def _norm_match(a: str) -> str:
    s = unicodedata.normalize("NFKD", a or "")
    s = s.encode("ascii", "ignore").decode("ascii").lower()
    s = re.sub(r"[^\w\s]", " ", s)
    return re.sub(r"\s+", " ", s).strip()


def _itunes_lookup_artists(title: str, artist: str, album: str) -> Optional[str]:
    """Return a fuller artist credit from iTunes, or None."""
    if not title:
        return None

    key = f"{title}\0{artist}\0{album}".lower()
    cached = _itunes_credit_cache.get(key)
    if cached is not None:
        return cached or None

    now = time.monotonic()
    miss_until = _itunes_miss_until.get(key, 0.0)
    if now < miss_until:
        return None

    term = " ".join(part for part in (title, artist, album) if part)
    query = urllib.parse.urlencode(
        {"term": term, "entity": "song", "limit": 8, "media": "music"}
    )
    url = "https://itunes.apple.com/search?" + query
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "MacroKeyboard/1.0"},
        method="GET",
    )

    try:
        with urllib.request.urlopen(req, timeout=2.5) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError, json.JSONDecodeError, OSError):
        _itunes_miss_until[key] = now + 60.0
        return None

    want_title = _norm_match(title)
    want_album = _norm_match(album)
    best: Optional[str] = None
    best_score = -1

    for item in data.get("results") or []:
        track = _norm_match(item.get("trackName") or "")
        if not track or (want_title not in track and track not in want_title):
            # Allow near-equal lengths with shared start
            if not want_title or not track:
                continue
            if want_title.split(" ")[0] != track.split(" ")[0]:
                continue

        score = 0
        if track == want_title:
            score += 5
        elif want_title in track or track in want_title:
            score += 3

        coll = _norm_match(item.get("collectionName") or "")
        if want_album and coll:
            if coll == want_album:
                score += 4
            elif want_album in coll or coll in want_album:
                score += 2

        itunes_artist = (item.get("artistName") or "").strip()
        if not itunes_artist:
            continue
        # Prefer credits that clearly list multiple artists.
        extras = len(_split_artist_names(itunes_artist))
        score += min(extras, 3)

        if score > best_score:
            best_score = score
            best = itunes_artist

    if best is None or best_score < 3:
        _itunes_credit_cache[key] = ""
        _itunes_miss_until[key] = now + 300.0
        return None

    _itunes_credit_cache[key] = best
    return best


def _enrich_title_artists(
    title: str, artist: str, album_artist: str, album: str = ""
) -> tuple[str, str]:
    """Build display title/artist using feat. tags and iTunes credits when needed."""
    featured: list[str] = []
    for match in _FEAT_RE.finditer(title or ""):
        _add_unique(featured, _split_artist_names(match.group(1)))

    clean_title = _FEAT_RE.sub(" ", title or "")
    clean_title = re.sub(r"\s{2,}", " ", clean_title).strip(" -\t")
    if not clean_title:
        clean_title = (title or "").strip()

    names: list[str] = []
    _add_unique(names, _split_artist_names(artist))
    _add_unique(names, _split_artist_names(album_artist))
    _add_unique(names, featured)

    # SMTC frequently omits co-artists (e.g. Coldplay-only for Princess of China).
    itunes_artists = _itunes_lookup_artists(title, artist or album_artist, album)
    if itunes_artists:
        _add_unique(names, _split_artist_names(itunes_artists))

    return clean_title, ", ".join(names)


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
    try:
        dev.set_nonblocking(True)
    except Exception:
        pass
    return dev


def launch_spotify() -> None:
    try:
        os.startfile("spotify:")  # type: ignore[attr-defined]
        return
    except OSError:
        pass

    local = os.environ.get("LOCALAPPDATA", "")
    candidates = [
        os.path.join(local, "Microsoft", "WindowsApps", "Spotify.exe"),
        os.path.join(local, "Spotify", "Spotify.exe"),
    ]
    for path in candidates:
        if os.path.isfile(path):
            subprocess.Popen([path], close_fds=True)
            return
    print("Spotify launch failed", flush=True)


def poll_device_commands(dev: hid.device) -> None:
    """Read device→host vendor IN reports (e.g. open Spotify)."""
    for _ in range(8):
        try:
            data = dev.read(64)
        except Exception:
            break
        if not data:
            break
        raw = bytes(data)
        if len(raw) >= 2 and raw[0] == REPORT_ID_HOSTCMD:
            if raw[1] == HOST_CMD_OPEN_SPOTIFY:
                launch_spotify()
        elif len(raw) >= 1 and raw[0] == HOST_CMD_OPEN_SPOTIFY and len(raw) == 1:
            # Some stacks strip report ID on read.
            launch_spotify()


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


def pack_discord(open_: bool, muted: bool, deaf: bool) -> int:
    flags = 0
    if open_:
        flags |= DISCORD_FLAG_OPEN
    if muted:
        flags |= DISCORD_FLAG_MUTED
    if deaf:
        flags |= DISCORD_FLAG_DEAF
    return flags


def send_discord(dev: hid.device, flags: int) -> None:
    n = dev.write(bytes([REPORT_ID_DISCORD, flags & 0xFF]))
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
        raw_artist = (props.artist or "").strip()
        album_artist = (props.album_artist or "").strip()
        album = (props.album_title or "").strip()
        title, artist = _enrich_title_artists(
            title, raw_artist, album_artist, album=album
        )

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


def pack_media_meta(info: MediaInfo) -> bytes:
    flags = 0
    if info.active:
        flags |= MEDIA_FLAG_ACTIVE
    if info.playing:
        flags |= MEDIA_FLAG_PLAYING
    if info.timeline:
        flags |= MEDIA_FLAG_TIMELINE

    payload = bytearray(MEDIA_META_LEN)
    payload[0] = flags
    payload[1] = info.position_s & 0xFF
    payload[2] = (info.position_s >> 8) & 0xFF
    payload[3] = info.duration_s & 0xFF
    payload[4] = (info.duration_s >> 8) & 0xFF
    return bytes(payload)


def send_media_meta(dev: hid.device, payload: bytes) -> None:
    n = dev.write(bytes([REPORT_ID_MEDIA]) + payload)
    if n is None or n < 0:
        raise OSError("hid write failed")


def send_media_title(dev: hid.device, title: str) -> None:
    n = dev.write(bytes([REPORT_ID_MEDIA_TITLE]) + _oled_field(title, MEDIA_TITLE_LEN))
    if n is None or n < 0:
        raise OSError("hid write failed")


def send_media_artist(dev: hid.device, artist: str) -> None:
    n = dev.write(bytes([REPORT_ID_MEDIA_ARTIST]) + _oled_field(artist, MEDIA_ARTIST_LEN))
    if n is None or n < 0:
        raise OSError("hid write failed")


def media_meta_changed(a: MediaInfo, b: MediaInfo) -> bool:
    return (
        a.active != b.active
        or a.playing != b.playing
        or a.timeline != b.timeline
        or a.duration_s != b.duration_s
        or a.position_s != b.position_s
    )


def media_text_changed(a: MediaInfo, b: MediaInfo) -> bool:
    return a.title != b.title or a.artist != b.artist or a.active != b.active


def main() -> None:
    try:
        CoInitialize()
    except OSError:
        pass

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    manager: Optional[SessionManager] = None
    discord = DiscordVoice()

    dev: Optional[hid.device] = None
    last_vol: Optional[int] = None
    last_vol_sent = 0.0
    last_media = EMPTY_MEDIA
    last_media_meta = pack_media_meta(EMPTY_MEDIA)
    last_media_sent = 0.0
    last_discord_flags: Optional[int] = None
    last_discord_sent = 0.0

    while True:
        try:
            if manager is None:
                manager = loop.run_until_complete(SessionManager.request_async())

            if dev is None:
                dev = open_keyboard()
                last_vol = None
                last_media = EMPTY_MEDIA
                last_media_meta = pack_media_meta(EMPTY_MEDIA)
                last_discord_flags = None
                print("Connected", flush=True)

            now = time.monotonic()
            poll_device_commands(dev)

            vol = read_volume()
            if vol != last_vol or now - last_vol_sent >= RESEND_S:
                send_volume(dev, vol)
                last_vol = vol
                last_vol_sent = now

            media = loop.run_until_complete(_fetch_media(manager))
            meta = pack_media_meta(media)
            due = MEDIA_PLAYING_SEND_S if media.playing else MEDIA_RESEND_S
            text_changed = media_text_changed(media, last_media)
            meta_changed = media_meta_changed(media, last_media) or meta != last_media_meta
            if text_changed or meta_changed or now - last_media_sent >= due:
                send_media_meta(dev, meta)
                if text_changed or now - last_media_sent >= due:
                    send_media_title(dev, media.title if media.active else "")
                    send_media_artist(dev, media.artist if media.active else "")
                last_media = media
                last_media_meta = meta
                last_media_sent = now

            d_open, d_muted, d_deaf = discord.snapshot()
            d_flags = pack_discord(d_open, d_muted, d_deaf)
            if d_flags != last_discord_flags or now - last_discord_sent >= DISCORD_SEND_S:
                send_discord(dev, d_flags)
                last_discord_flags = d_flags
                last_discord_sent = now

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
            # Media/Discord APIs can fail transiently; keep volume sync alive.
            print(f"Host: {exc}", flush=True)
            manager = None
            discord.close()
            time.sleep(0.5)
            continue

        time.sleep(POLL_S)

    discord.close()
    if dev is not None:
        try:
            dev.close()
        except Exception:
            pass
    loop.close()


if __name__ == "__main__":
    main()
