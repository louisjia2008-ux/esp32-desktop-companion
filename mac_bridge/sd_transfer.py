"""One-time, acknowledged SD upload over the existing WebSocket connection."""

from __future__ import annotations

import asyncio
import secrets
import time

from wallpaper_cache import CHUNK_BYTES, chunk_packet


class TransferStopped(Exception):
    pass


class SdTransfer:
    def __init__(self, wallpaper_id, package_factory, send, *, ack_timeout=4.0):
        self.wallpaper_id = wallpaper_id
        self.transfer_id = secrets.token_hex(8)
        self.package_factory = package_factory
        self.send = send
        self.ack_timeout = ack_timeout
        self.acks = asyncio.Queue(maxsize=8)
        self.stop_event = asyncio.Event()
        self.task = None
        self.state = "preparing"
        self.reason = ""
        self.offset = 0
        self.total = 0
        self.sent_bytes = 0
        self.sent_chunks = 0
        self.sd_capacity = 0

    def stop(self):
        self.stop_event.set()

    def feed(self, message):
        if (message["transfer_id"] != self.transfer_id or
                message["wallpaper_id"] != self.wallpaper_id):
            return False
        self.sd_capacity = message.get("sd_capacity_bytes", 0)
        if self.acks.full():
            # Only eight outstanding acknowledgements can be retained. A
            # sender waits after every chunk and retries the same offset.
            self.acks.get_nowait()
        self.acks.put_nowait(message)
        return True

    def snapshot(self):
        return {"wallpaper_id": self.wallpaper_id, "state": self.state,
                "received_bytes": self.offset, "total_bytes": self.total,
                "sent_bytes": self.sent_bytes, "sent_chunks": self.sent_chunks,
                "sd_capacity_bytes": self.sd_capacity, "reason": self.reason}

    def control(self, operation, **fields):
        return {"v": 1, "type": "wallpaper_sync", "op": operation,
                "transfer_id": self.transfer_id, "wallpaper_id": self.wallpaper_id,
                **fields}

    async def _ack(self, predicate, timeout):
        deadline = time.monotonic() + timeout
        while True:
            if self.stop_event.is_set():
                raise TransferStopped()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise asyncio.TimeoutError()
            incoming = asyncio.create_task(self.acks.get())
            stopped = asyncio.create_task(self.stop_event.wait())
            try:
                done, _ = await asyncio.wait(
                    {incoming, stopped}, timeout=remaining,
                    return_when=asyncio.FIRST_COMPLETED,
                )
                if stopped in done:
                    raise TransferStopped()
                if incoming not in done:
                    raise asyncio.TimeoutError()
                message = incoming.result()
                if message["state"] == "error" or predicate(message):
                    return message
            finally:
                for task in (incoming, stopped):
                    if not task.done(): task.cancel()
                await asyncio.gather(incoming, stopped, return_exceptions=True)

    async def _exchange(self, packet, predicate):
        for attempt in range(3):
            if self.stop_event.is_set(): raise TransferStopped()
            await self.send(packet)
            if isinstance(packet, bytes):
                self.sent_bytes += len(packet) - 20
                self.sent_chunks += 1
            try:
                return await self._ack(predicate, self.ack_timeout)
            except asyncio.TimeoutError:
                if attempt == 2: raise

    @staticmethod
    def _require_success(message):
        if message["state"] == "error":
            raise ValueError(message.get("reason") or "device_rejected")

    async def run(self):
        try:
            package = await asyncio.to_thread(self.package_factory)
            self.total = package.size
            self.state = "uploading"
            deadline = time.monotonic() + 60
            while True:
                begin = self.control("begin", bytes=package.size, crc32=package.crc32)
                ack = await self._exchange(begin, lambda m: m["state"] in {"ready", "cached"})
                if ack["state"] != "error" or ack.get("reason") != "busy" or time.monotonic() >= deadline:
                    break
                try:
                    await asyncio.wait_for(self.stop_event.wait(), timeout=1.0)
                    raise TransferStopped()
                except asyncio.TimeoutError:
                    pass
            self._require_success(ack)
            if ack["bytes"] != self.total:
                raise ValueError("device_size_mismatch")
            if ack["state"] == "cached":
                if ack["offset"] != self.total: raise ValueError("cached_offset_mismatch")
                self.offset = self.total
                self.state = "cached"
                print(f"SD_WALLPAPER_REUSED id={self.wallpaper_id} bytes={self.total} uploaded_bytes=0", flush=True)
                return
            if not 0 <= ack["offset"] <= self.total:
                raise ValueError("device_offset_mismatch")
            self.offset = ack["offset"]
            with package.path.open("rb") as handle:
                handle.seek(self.offset)
                while self.offset < self.total:
                    data = handle.read(CHUNK_BYTES)
                    if not data: raise ValueError("package_truncated")
                    end = self.offset + len(data)
                    ack = await self._exchange(
                        chunk_packet(self.transfer_id, self.offset, data),
                        lambda m: m["state"] == "progress" and m["offset"] == end,
                    )
                    self._require_success(ack)
                    if ack["bytes"] != self.total: raise ValueError("device_size_mismatch")
                    self.offset = end
            self.state = "verifying"
            ack = await self._exchange(self.control("commit"),
                                       lambda m: m["state"] in {"verifying", "complete"})
            self._require_success(ack)
            if ack["state"] != "complete":
                # Firmware validates one frame per UI loop while the previous
                # wallpaper remains visible. Long videos need a bounded wait
                # that grows with the number of frames, rather than 180 seconds.
                validation_timeout = max(180.0, package.frame_count * 0.25 + 60.0)
                ack = await self._ack(lambda m: m["state"] == "complete", validation_timeout)
                self._require_success(ack)
            if ack["offset"] != self.total or ack["bytes"] != self.total:
                raise ValueError("commit_size_mismatch")
            self.state = "complete"
            print(f"SD_WALLPAPER_STORED id={self.wallpaper_id} bytes={self.total} chunks={self.sent_chunks}", flush=True)
        except TransferStopped:
            self.state = "aborted"
            self.reason = "selection_changed_or_disconnected"
        except Exception as error:
            self.state = "error"
            self.reason = str(error)[:48] if isinstance(error, ValueError) else type(error).__name__
            print(f"SD_WALLPAPER_FAILED id={self.wallpaper_id} reason={self.reason}", flush=True)
        finally:
            if self.state not in {"complete", "cached"}:
                try: await self.send(self.control("abort"))
                except Exception: pass
