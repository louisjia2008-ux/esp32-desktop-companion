"""Validated, physically approved ESP32-S3 OTA over the paired WebSocket."""
from __future__ import annotations

import asyncio
from dataclasses import dataclass
import hashlib
import hmac
import re
import secrets
import struct
import time
import zlib

MAX_FIRMWARE_BYTES = 0x640000
# Keep a chunk inside a typical TCP segment. The receiver accepts up to 12 KiB,
# but its bounded socket read can time out on a large frame over a jittery LAN.
CHUNK_BYTES = 1024
TERMINAL_STATES = frozenset({'complete', 'error', 'cancelled', 'rolled_back'})


class OtaError(ValueError):
    pass


@dataclass(frozen=True)
class Firmware:
    data: bytes
    version: str
    build: str
    sha256: str


def validate_firmware(data: bytes) -> Firmware:
    """Parse segments, checksum and appended digest; reject merged flash files."""
    if not 288 <= len(data) <= MAX_FIRMWARE_BYTES:
        raise OtaError('Firmware size is outside the OTA partition bounds')
    if data[0] != 0xE9 or not 1 <= data[1] <= 16 or struct.unpack_from('<H', data, 12)[0] != 9:
        raise OtaError('Expected an ESP32-S3 application image')
    if struct.unpack_from('<I', data, 32)[0] != 0xABCD5432:
        raise OtaError('Application descriptor is missing')
    position, checksum = 24, 0xEF
    for index in range(data[1]):
        if position + 8 > len(data):
            raise OtaError('Truncated segment header')
        size = struct.unpack_from('<I', data, position + 4)[0]
        position += 8
        if size == 0 or size > len(data) - position or (index == 0 and size < 256):
            raise OtaError('Truncated or invalid segment')
        for value in data[position:position + size]:
            checksum ^= value
        position += size
    checksum_offset = position | 15
    if checksum_offset >= len(data) or data[checksum_offset] != checksum:
        raise OtaError('Application checksum does not match')
    end = checksum_offset + 1
    if data[23] != 1 or len(data) != end + 32:
        raise OtaError('Expected exactly one image with an appended SHA-256')
    if not hmac.compare_digest(hashlib.sha256(data[:end]).digest(), data[end:]):
        raise OtaError('Application SHA-256 does not match')
    versions = set(re.findall(rb'DEVCOMPANION_FW=(\d{1,3}\.\d{1,3}\.\d{1,3})\x00', data[:end]))
    if len(versions) != 1:
        raise OtaError('Expected one Dev Companion firmware version marker')
    version = versions.pop().decode('ascii')
    def descriptor_text(start, length):
        try:
            return data[start:start+length].split(b'\0', 1)[0].decode('ascii')
        except UnicodeDecodeError as error:
            raise OtaError('Invalid application descriptor text') from error
    build = descriptor_text(128, 16) + ' ' + descriptor_text(112, 16)
    builds = set(re.findall(rb'DEVCOMPANION_BUILD=([ -~]{1,39})\x00', data[:end]))
    if len(builds) > 1:
        raise OtaError('Conflicting build markers')
    if builds:
        build = builds.pop().decode('ascii')
    return Firmware(data, version, build.strip(), hashlib.sha256(data).hexdigest())


def offer_mac(key: bytes, transfer_id: str, firmware: Firmware, nonce: str) -> str:
    canonical = f'ota-v1\n{nonce}\n{transfer_id}\n{len(firmware.data)}\n{firmware.sha256}\n{firmware.version}\n{firmware.build}'
    return hmac.new(key, canonical.encode('ascii'), hashlib.sha256).hexdigest()


class OtaTransfer:
    def __init__(self, firmware, key, nonce, send, *, ack_timeout=8.0, approval_timeout=120.0):
        self.firmware, self.key, self.nonce, self.send = firmware, key, nonce, send
        self.ack_timeout, self.approval_timeout = ack_timeout, approval_timeout
        self.transfer_id = secrets.token_hex(8)
        self.state, self.reason, self.offset = 'offered', '', 0
        self.acks = asyncio.Queue(maxsize=8)
        self.task = None
        self.sent_chunks = 0
        self.old_version = ''
        self.old_sha256 = ''
        self.complete_received = False

    def snapshot(self):
        return {'transfer_id': self.transfer_id, 'version': self.firmware.version,
                'build': self.firmware.build, 'state': self.state, 'reason': self.reason,
                'offset': self.offset, 'bytes': len(self.firmware.data),
                'percent': self.offset * 100 // len(self.firmware.data)}

    def feed(self, message):
        if message.get('transfer_id') != self.transfer_id or self.acks.full():
            return
        if message.get('state') == 'complete' and message.get('offset') == len(self.firmware.data):
            self.complete_received = True
        self.acks.put_nowait(message)

    def disconnected(self):
        # A complete transfer must reboot before it can be confirmed.
        if not self.complete_received and self.state not in {'rebooting', *TERMINAL_STATES}:
            self.feed({'transfer_id': self.transfer_id, 'state': 'error', 'reason': 'disconnected'})

    async def wait_for(self, states, offset=None, timeout=None):
        deadline = time.monotonic() + (self.ack_timeout if timeout is None else timeout)
        while True:
            message = await asyncio.wait_for(self.acks.get(), max(0.001, deadline-time.monotonic()))
            if message['state'] in {'error', 'cancelled'}:
                raise OtaError(message.get('reason') or message['state'])
            if message['state'] in states and (offset is None or message.get('offset') == offset):
                return message

    async def exchange(self, packet, states, offset=None):
        for attempt in range(3):
            await self.send(packet)
            try:
                return await self.wait_for(states, offset)
            except asyncio.TimeoutError:
                if attempt == 2:
                    raise

    async def run(self):
        fw = self.firmware
        try:
            await self.send({'v': 1, 'type': 'ota_offer', 'transfer_id': self.transfer_id,
                             'bytes': len(fw.data), 'sha256': fw.sha256, 'version': fw.version,
                             'build': fw.build, 'mac': offer_mac(self.key, self.transfer_id, fw, self.nonce)})
            try:
                await self.wait_for({'ready'}, 0, self.approval_timeout)
            except asyncio.TimeoutError as error:
                raise OtaError('approval_timeout') from error
            self.state = 'uploading'
            while self.offset < len(fw.data):
                chunk = fw.data[self.offset:self.offset + CHUNK_BYTES]
                packet = struct.pack('<4sQIII', b'DWO1', int(self.transfer_id, 16), self.offset,
                                     len(chunk), zlib.crc32(chunk)) + chunk
                end = self.offset + len(chunk)
                await self.exchange(packet, {'progress'}, end)
                self.offset = end
                self.sent_chunks += 1
            self.state = 'verifying'
            await self.exchange({'v': 1, 'type': 'ota_control', 'op': 'commit',
                                 'transfer_id': self.transfer_id}, {'complete'}, self.offset)
            self.state = 'rebooting'
        except asyncio.CancelledError:
            self.state, self.reason = 'cancelled', 'bridge_stopped'
            await self._abort()
            raise
        except Exception as error:
            self.reason = str(error) if isinstance(error, OtaError) else type(error).__name__
            self.state = 'cancelled' if self.reason in {'cancelled', 'approval_timeout'} else 'error'
            await self._abort()

    async def _abort(self):
        try:
            await self.send({'v': 1, 'type': 'ota_control', 'op': 'abort', 'transfer_id': self.transfer_id})
        except Exception:
            pass
