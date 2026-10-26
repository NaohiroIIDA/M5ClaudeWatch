#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["bleak>=0.22", "pyserial>=3.5"]
# ///
"""M5ClaudeWatch bridge: relays Claude Code hook events to the M5Stack StopWatch.

Hooks (m5claudewatch_hook.py) connect to a Unix socket and send one JSON line.
Permission requests stay connected until the watch answers OK / NG.

    uv run host/m5claudewatch_bridge.py                 # BLE (auto-discover "M5ClaudeWatch")
    uv run host/m5claudewatch_bridge.py --serial /dev/cu.usbmodem1101
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import itertools
import json
import logging
import os
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

SOCKET_PATH = Path(os.environ.get("M5CLAUDEWATCH_SOCKET", Path.home() / ".claude" / "m5claudewatch.sock"))
CONFIG_PATH = Path.home() / ".config" / "m5claudewatch" / "config.json"
SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # host -> device (write)
TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # device -> host (notify)
DEVICE_NAME = "M5ClaudeWatch"

log = logging.getLogger("m5claudewatch")


@dataclass
class Session:
    proj: str
    state: str = "idle"  # busy / done / notice / idle
    since: float = field(default_factory=time.monotonic)
    msg: str = ""
    ask: bool = False  # the finished turn ended with a question to the user
    updated: float = field(default_factory=time.monotonic)


@dataclass
class Pending:
    id: str
    sid: str
    message: dict
    future: asyncio.Future


class Bridge:
    def __init__(self, perm_timeout: float) -> None:
        self.perm_timeout = perm_timeout
        self.sessions: dict[str, Session] = {}
        self.pending: dict[str, Pending] = {}
        self.ids = itertools.count(1)
        self.transport: Transport | None = None

    # -- device side -------------------------------------------------------

    @property
    def connected(self) -> bool:
        return self.transport is not None and self.transport.connected

    async def send(self, msg: dict) -> None:
        if not self.connected:
            return
        try:
            await self.transport.send(json.dumps(msg, ensure_ascii=False))
        except Exception as exc:  # noqa: BLE001 - transport errors vary by backend
            log.warning("send failed: %s", exc)

    def working_count(self) -> int:
        return sum(1 for s in self.sessions.values() if s.state == "busy")

    def state_message(self, s: Session, quiet: bool = False) -> dict:
        el = int(time.monotonic() - s.since)
        msg = {"t": s.state, "proj": s.proj, "n": self.working_count()}
        if s.state == "busy":
            msg["el"] = el
        elif s.state in ("done", "notice"):
            msg["t"] = "done" if s.state == "done" else "notify"
            msg["msg"] = s.msg
            if s.ask:
                msg["ask"] = True
            if quiet:
                msg["quiet"] = True
        return msg

    async def on_device_connected(self) -> None:
        log.info("watch connected")
        await self.send({"t": "reset"})
        if self.sessions:
            latest = max(self.sessions.values(), key=lambda s: s.updated)
            if latest.state != "idle":
                await self.send(self.state_message(latest, quiet=True))
        for p in self.pending.values():
            await self.send(p.message)

    async def on_device_line(self, line: str) -> None:
        log.debug("device -> %s", line)
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            return  # firmware log output, ignore
        t = msg.get("t")
        if t == "hello":
            log.info("firmware %s", msg.get("fw"))
        elif t == "ans":
            p = self.pending.get(str(msg.get("id")))
            if p and not p.future.done():
                if msg.get("skip"):
                    p.future.set_result(None)
                else:
                    p.future.set_result("allow" if msg.get("ok") else "deny")

    # -- hook side ---------------------------------------------------------

    async def handle_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            line = await asyncio.wait_for(reader.readline(), 5)
            log.debug("hook -> %s", line.decode(errors="replace").strip())
            req = json.loads(line or b"{}")
            ev = req.get("ev")
            if ev == "perm":
                decision = await self.handle_perm(req, reader)
                writer.write((json.dumps({"decision": decision}) + "\n").encode())
            elif ev == "status":
                writer.write((json.dumps({"connected": self.connected,
                                          "sessions": len(self.sessions),
                                          "pending": len(self.pending)}) + "\n").encode())
            else:
                await self.handle_event(req)
            await writer.drain()
        except (asyncio.TimeoutError, json.JSONDecodeError, ConnectionError):
            pass
        finally:
            writer.close()

    async def handle_event(self, req: dict) -> None:
        ev, sid = req.get("ev"), req.get("sid", "")
        proj = req.get("proj", "")
        s = self.sessions.setdefault(sid, Session(proj=proj))
        s.proj, s.updated = proj, time.monotonic()
        if ev == "busy":
            if s.state != "busy":
                s.since = time.monotonic()
            s.state = "busy"
            await self.cancel_session_perms(sid)
        elif ev == "done":
            el = int(time.monotonic() - s.since) if s.state == "busy" else 0
            s.state, s.msg, s.ask = "done", req.get("msg", ""), bool(req.get("ask"))
            s.since = time.monotonic()
            await self.cancel_session_perms(sid)
            await self.send({"t": "done", "proj": proj, "msg": s.msg, "el": el,
                             "ask": s.ask, "n": self.working_count()})
            return
        elif ev == "notify":
            s.state, s.msg = "notice", req.get("msg", "")
        elif ev == "end":
            self.sessions.pop(sid, None)
            await self.cancel_session_perms(sid)
            await self.send({"t": "idle", "n": self.working_count()})
            return
        else:
            return
        await self.send(self.state_message(s))

    async def handle_perm(self, req: dict, reader: asyncio.StreamReader) -> str | None:
        if not self.connected:
            return None  # fall back to the normal terminal prompt immediately
        pid = str(next(self.ids))
        message = {"t": "perm", "id": pid, "proj": req.get("proj", ""),
                   "tool": req.get("tool", ""), "detail": req.get("detail", "")}
        fut = asyncio.get_running_loop().create_future()
        self.pending[pid] = Pending(pid, req.get("sid", ""), message, fut)
        log.info("perm #%s %s: %s", pid, message["tool"], message["detail"][:80])
        await self.send(message)

        # If Claude Code kills the hook (e.g. answered in the terminal), the
        # socket closes; withdraw the request from the watch.
        eof = asyncio.create_task(reader.read())
        try:
            done, _ = await asyncio.wait({fut, eof}, timeout=self.perm_timeout,
                                         return_when=asyncio.FIRST_COMPLETED)
            decision = fut.result() if fut in done else None
        finally:
            eof.cancel()
            self.pending.pop(pid, None)
            if not fut.done() or fut.result() is None:
                await self.send({"t": "cancel", "id": pid})
        log.info("perm #%s -> %s", pid, decision or "terminal")
        return decision

    async def cancel_session_perms(self, sid: str) -> None:
        for p in [p for p in self.pending.values() if p.sid == sid]:
            if not p.future.done():
                p.future.set_result(None)


# -- transports ---------------------------------------------------------------

class Transport:
    connected = False

    async def send(self, line: str) -> None:
        raise NotImplementedError

    async def run(self, bridge: Bridge) -> None:
        raise NotImplementedError


class BleTransport(Transport):
    def __init__(self, address: str | None) -> None:
        self.address = address
        self.client = None
        self.buf = b""
        # bleak's CoreBluetooth backend tracks one pending write per
        # characteristic; overlapping writes orphan the earlier one forever.
        self.write_lock = asyncio.Lock()

    async def send(self, line: str) -> None:
        data = (line + "\n").encode()
        size = max(20, min(180, (self.client.mtu_size or 23) - 3))
        async with self.write_lock:
            for i in range(0, len(data), size):
                await asyncio.wait_for(
                    self.client.write_gatt_char(RX_UUID, data[i:i + size], response=True), 5)

    async def find(self):
        from bleak import BleakScanner

        if self.address:
            # Only the watch we paired with first; delete config.json to re-discover.
            return await BleakScanner.find_device_by_address(self.address, timeout=10)
        return await BleakScanner.find_device_by_filter(
            lambda d, ad: SERVICE_UUID in [u.lower() for u in ad.service_uuids]
            or (d.name or ad.local_name or "") == DEVICE_NAME,
            timeout=10,
        )

    async def run(self, bridge: Bridge) -> None:
        from bleak import BleakClient

        loop = asyncio.get_running_loop()
        while True:
            dev = await self.find()
            if dev is None:
                log.info("watch not found, retrying...")
                await asyncio.sleep(3)
                continue
            if dev.address != self.address:
                self.address = dev.address
                save_config({"ble_address": dev.address})
            disconnected = asyncio.Event()

            def on_notify(_, data: bytearray) -> None:
                log.debug("notify %r", bytes(data))
                self.buf += bytes(data)
                while b"\n" in self.buf:
                    raw, self.buf = self.buf.split(b"\n", 1)
                    asyncio.ensure_future(bridge.on_device_line(raw.decode(errors="replace")))

            try:
                async with BleakClient(dev, disconnected_callback=lambda _: loop.call_soon_threadsafe(
                        disconnected.set)) as client:
                    self.client, self.buf = client, b""
                    await client.start_notify(TX_UUID, on_notify)
                    self.connected = True
                    await bridge.on_device_connected()
                    await disconnected.wait()
            except Exception as exc:  # noqa: BLE001
                log.warning("BLE error: %s", exc)
            finally:
                self.connected = False
                log.info("watch disconnected")
            await asyncio.sleep(2)


class SerialTransport(Transport):
    def __init__(self, port: str) -> None:
        self.port = port
        self.ser = None

    async def send(self, line: str) -> None:
        await asyncio.to_thread(self.ser.write, (line + "\n").encode())

    async def run(self, bridge: Bridge) -> None:
        import serial

        loop = asyncio.get_running_loop()
        while True:
            try:
                self.ser = serial.Serial(self.port, 115200, timeout=1)
            except serial.SerialException as exc:
                log.info("serial open failed (%s), retrying...", exc)
                await asyncio.sleep(3)
                continue
            lost = asyncio.Event()

            def reader() -> None:
                try:
                    while True:
                        raw = self.ser.readline()
                        if raw:
                            asyncio.run_coroutine_threadsafe(
                                bridge.on_device_line(raw.decode(errors="replace").strip()), loop)
                except Exception:  # noqa: BLE001 - port unplugged
                    loop.call_soon_threadsafe(lost.set)

            threading.Thread(target=reader, daemon=True).start()
            self.connected = True
            await bridge.on_device_connected()
            await lost.wait()
            self.connected = False
            with contextlib.suppress(Exception):
                self.ser.close()
            log.info("serial disconnected")
            await asyncio.sleep(2)


# -- main ---------------------------------------------------------------------

def load_config() -> dict:
    try:
        return json.loads(CONFIG_PATH.read_text())
    except (OSError, json.JSONDecodeError):
        return {}


def save_config(values: dict) -> None:
    cfg = load_config() | values
    CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
    CONFIG_PATH.write_text(json.dumps(cfg, indent=2))


async def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--serial", help="use USB serial port instead of BLE")
    ap.add_argument("--address", help="BLE address/UUID of the watch (saved after first connect)")
    ap.add_argument("--perm-timeout", type=float, default=float(os.environ.get("M5CLAUDEWATCH_PERM_TIMEOUT", 110)),
                    help="seconds to wait for OK/NG before falling back to the terminal prompt")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(message)s", datefmt="%H:%M:%S")

    bridge = Bridge(args.perm_timeout)
    bridge.transport = (SerialTransport(args.serial) if args.serial
                        else BleTransport(args.address or load_config().get("ble_address")))

    SOCKET_PATH.parent.mkdir(parents=True, exist_ok=True)
    with contextlib.suppress(FileNotFoundError):
        SOCKET_PATH.unlink()
    server = await asyncio.start_unix_server(bridge.handle_client, path=str(SOCKET_PATH))
    os.chmod(SOCKET_PATH, 0o600)
    log.info("listening on %s", SOCKET_PATH)
    async with server:
        await bridge.transport.run(bridge)


if __name__ == "__main__":
    with contextlib.suppress(KeyboardInterrupt):
        asyncio.run(main())
