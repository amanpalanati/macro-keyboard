"""Discord mute/deafen via local RPC (StreamKit client — no app secret needed)."""

from __future__ import annotations

import json
import os
import struct
import threading
import time
import urllib.request
import uuid
from pathlib import Path
from typing import Any, Optional

STREAMKIT_CLIENT_ID = "207646673902501888"
OAUTH_SCOPES = ["rpc", "rpc.voice.read"]
TOKEN_EXCHANGE_URL = "https://streamkit.discord.com/overlay/token"

OP_HANDSHAKE = 0
OP_FRAME = 1
OP_CLOSE = 2

DISCORD_PROCESS_NAMES = {
    "discord.exe",
    "discordcanary.exe",
    "discordptb.exe",
    "discorddevelopment.exe",
}


def _cache_path() -> Path:
    base = os.environ.get("LOCALAPPDATA") or str(Path.home())
    return Path(base) / "MacroKeyboard" / "discord_rpc.json"


def _discord_pids() -> set[int]:
    try:
        import psutil
    except ImportError:
        return set()

    pids: set[int] = set()
    for proc in psutil.process_iter(["pid", "name"]):
        name = (proc.info.get("name") or "").lower()
        if name in DISCORD_PROCESS_NAMES:
            pid = proc.info.get("pid")
            if isinstance(pid, int):
                pids.add(pid)
    return pids


def discord_process_running() -> bool:
    if _discord_pids():
        return True
    return _discord_running_tasklist()


def discord_window_open() -> bool:
    """True only when a real Discord window is visible (not just tray/background)."""
    pids = _discord_pids()
    if not pids:
        return False

    import ctypes
    from ctypes import wintypes

    user32 = ctypes.windll.user32
    found = False
    GWL_EXSTYLE = -20
    WS_EX_TOOLWINDOW = 0x00000080

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def enum_proc(hwnd: int, _lparam: int) -> bool:
        nonlocal found
        if found:
            return False
        if not user32.IsWindowVisible(hwnd):
            return True

        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value not in pids:
            return True

        ex_style = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
        if ex_style & WS_EX_TOOLWINDOW:
            return True

        length = user32.GetWindowTextLengthW(hwnd)
        if length <= 0:
            return True
        buf = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buf, length + 1)
        if "discord" not in buf.value.lower():
            return True

        rect = wintypes.RECT()
        if not user32.GetClientRect(hwnd, ctypes.byref(rect)):
            return True
        if (rect.right - rect.left) < 200 or (rect.bottom - rect.top) < 200:
            return True

        found = True
        return False

    user32.EnumWindows(enum_proc, 0)
    return found


def _discord_running_tasklist() -> bool:
    try:
        import subprocess

        out = subprocess.check_output(
            ["tasklist", "/FI", "IMAGENAME eq Discord.exe", "/NH"],
            text=True,
            stderr=subprocess.DEVNULL,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        return "Discord.exe" in out
    except Exception:
        return False


def _load_token() -> Optional[str]:
    path = _cache_path()
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        token = data.get("access_token")
        return token if isinstance(token, str) and token else None
    except (OSError, json.JSONDecodeError, TypeError):
        return None


def _save_token(token: str) -> None:
    path = _cache_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"access_token": token}), encoding="utf-8")


def _clear_token() -> None:
    try:
        _cache_path().unlink(missing_ok=True)
    except OSError:
        pass


def _exchange_code(code: str) -> str:
    body = json.dumps({"code": code}).encode("utf-8")
    req = urllib.request.Request(
        TOKEN_EXCHANGE_URL,
        data=body,
        headers={
            "Content-Type": "application/json",
            "User-Agent": "MacroKeyboard/1.0",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=15) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    token = data.get("access_token")
    if not isinstance(token, str) or not token:
        raise RuntimeError("StreamKit token exchange returned no access_token")
    return token


class DiscordVoice:
    """Background poller for Discord open + mute/deafen."""

    def __init__(self) -> None:
        self._pipe: Any = None
        self._authed = False
        self._lock = threading.Lock()
        self._state = (False, False, False)  # open, muted, deaf
        self._stop = False
        self._thread = threading.Thread(
            target=self._run, name="discord-voice", daemon=True
        )
        self._thread.start()

    def snapshot(self) -> tuple[bool, bool, bool]:
        with self._lock:
            return self._state

    def close(self) -> None:
        self._stop = True
        self._close_pipe()

    def _set_state(self, open_: bool, muted: bool, deaf: bool) -> None:
        with self._lock:
            self._state = (open_, muted, deaf)

    def _close_pipe(self) -> None:
        if self._pipe is not None:
            try:
                self._pipe.close()
            except Exception:
                pass
        self._pipe = None
        self._authed = False

    def _run(self) -> None:
        while not self._stop:
            try:
                # Process can keep running in the tray; only treat a visible
                # Discord window as "open" for the OLED icons.
                if not discord_window_open():
                    self._close_pipe()
                    self._set_state(False, False, False)
                    time.sleep(1.0)
                    continue

                self._ensure_ready()
                data = self._command("GET_VOICE_SETTINGS")
                payload = data.get("data") if isinstance(data.get("data"), dict) else data
                muted = bool(payload.get("mute", False))
                deaf = bool(payload.get("deaf", False))
                self._set_state(True, muted, deaf)
                time.sleep(0.35)
            except Exception:
                self._close_pipe()
                if discord_window_open():
                    with self._lock:
                        _, muted, deaf = self._state
                    self._set_state(True, muted, deaf)
                else:
                    self._set_state(False, False, False)
                time.sleep(1.0)

    def _ensure_ready(self) -> None:
        if self._pipe is None:
            self._connect()
            self._handshake()
            self._authed = False

        if not self._authed:
            self._authenticate()

    def _connect(self) -> None:
        last_err: Optional[Exception] = None
        for i in range(10):
            path = rf"\\.\pipe\discord-ipc-{i}"
            try:
                self._pipe = open(path, "r+b", buffering=0)
                return
            except OSError as exc:
                last_err = exc
        raise OSError(f"discord IPC pipe not found: {last_err}")

    def _send(self, opcode: int, payload: dict[str, Any]) -> None:
        assert self._pipe is not None
        raw = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self._pipe.write(struct.pack("<II", opcode, len(raw)) + raw)
        self._pipe.flush()

    def _recv(self) -> tuple[int, dict[str, Any]]:
        assert self._pipe is not None
        header = self._pipe.read(8)
        if len(header) < 8:
            raise OSError("discord IPC closed")
        opcode, length = struct.unpack("<II", header)
        data = self._pipe.read(length)
        if len(data) < length:
            raise OSError("discord IPC short read")
        return opcode, json.loads(data.decode("utf-8"))

    def _recv_until_nonce(self, nonce: str, timeout_s: float = 120.0) -> dict[str, Any]:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            opcode, payload = self._recv()
            if opcode == OP_CLOSE:
                raise OSError("discord IPC close frame")
            if payload.get("nonce") == nonce:
                if payload.get("evt") == "ERROR":
                    err = payload.get("data") or {}
                    raise RuntimeError(
                        f"discord RPC error {err.get('code')}: {err.get('message')}"
                    )
                return payload
        raise TimeoutError("discord RPC response timeout")

    def _handshake(self) -> None:
        self._send(OP_HANDSHAKE, {"v": 1, "client_id": STREAMKIT_CLIENT_ID})
        opcode, _payload = self._recv()
        if opcode == OP_CLOSE:
            raise OSError("discord handshake closed")

    def _command(self, cmd: str, args: Optional[dict[str, Any]] = None) -> dict[str, Any]:
        nonce = str(uuid.uuid4())
        self._send(OP_FRAME, {"cmd": cmd, "nonce": nonce, "args": args or {}})
        return self._recv_until_nonce(nonce)

    def _authenticate(self) -> None:
        token = _load_token()
        if token:
            try:
                self._command("AUTHENTICATE", {"access_token": token})
                self._authed = True
                return
            except Exception:
                _clear_token()
                self._close_pipe()
                self._connect()
                self._handshake()

        # Discord shows an in-app authorize prompt (once).
        auth = self._command(
            "AUTHORIZE",
            {"client_id": STREAMKIT_CLIENT_ID, "scopes": OAUTH_SCOPES},
        )
        data = auth.get("data") or {}
        code = data.get("code")
        if not isinstance(code, str) or not code:
            raise RuntimeError("discord AUTHORIZE returned no code")
        token = _exchange_code(code)
        _save_token(token)
        self._command("AUTHENTICATE", {"access_token": token})
        self._authed = True
