from __future__ import annotations
import asyncio
from concurrent.futures import Future
import hashlib
import hmac
import json
from pathlib import Path
import struct
import tempfile
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen
import zlib
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
from bridge import Bridge, ProtocolError, pairing_mac, parse_device_message
from ota import CHUNK_BYTES, OtaError, OtaTransfer, MAX_FIRMWARE_BYTES, offer_mac, validate_firmware
from wallpaper_http import WallpaperEditorService
from wallpaper_library import WallpaperLibrary


def firmware_bytes(size=28000):
    header = bytearray(24)
    header[0:2] = b'\xe9\x01'
    struct.pack_into('<H', header, 12, 9)
    header[23] = 1
    segment = bytearray(size)
    struct.pack_into('<I', segment, 0, 0xabcd5432)
    segment[80:88] = b'12:34:56'
    segment[96:107] = b'Oct  1 2026'
    segment[256:278] = b'DEVCOMPANION_FW=0.7.1\x00'
    image = header + struct.pack('<II', 0x3c000020, len(segment)) + segment
    checksum = 0xef
    for byte in segment:
        checksum ^= byte
    image += bytes((len(image) | 15) - len(image)) + bytes([checksum])
    image += hashlib.sha256(image).digest()
    return bytes(image)


class ImageTests(unittest.TestCase):
    def test_valid_and_structural_failures(self):
        image = firmware_bytes()
        self.assertEqual(validate_firmware(image).version, '0.7.1')
        self.assertEqual(validate_firmware(image).build, 'Oct  1 2026 12:34:56')
        bad = [b'', image[:287], image[:-1], image + b'\x00', b'\x00' + image[1:],
               image[:12] + b'\x00\x00' + image[14:], image[:32] + b'\x00'*4 + image[36:]]
        corrupt = bytearray(image); corrupt[600] ^= 1; bad.append(bytes(corrupt))
        for value in bad:
            with self.subTest(length=len(value)), self.assertRaises(OtaError):
                validate_firmware(value)

    def test_signature_binds_nonce_and_image(self):
        fw = validate_firmware(firmware_bytes())
        signature = offer_mac(b'k'*16, 'a'*16, fw, 'b'*32)
        self.assertNotEqual(signature, offer_mac(b'k'*16, 'a'*16, fw, 'c'*32))
        self.assertNotEqual(signature, offer_mac(b'x'*16, 'a'*16, fw, 'b'*32))

    def test_strict_ack_and_boot_parser(self):
        ack = dict(v=1,type='ota_ack',transfer_id='a'*16,state='ready',offset=0,reason='')
        self.assertEqual(parse_device_message(json.dumps(ack)), ack)
        for changes in ({'offset':True}, {'offset':MAX_FIRMWARE_BYTES+1}, {'state':'accepted'}, {'extra':1}):
            with self.assertRaises(ProtocolError): parse_device_message(json.dumps(dict(ack,**changes)))
        with self.assertRaises(ProtocolError):
            parse_device_message(json.dumps(dict(v=1,type='ota_boot',firmware='0.7.1',state='valid',image_sha256='wrong')))


class TransferTests(unittest.IsolatedAsyncioTestCase):
    async def test_chunks_retries_and_commit_wait_for_approval(self):
        fw = validate_firmware(firmware_bytes())
        packets, received = [], bytearray()
        drop_first_ack = True
        async def send(packet):
            nonlocal drop_first_ack
            packets.append(packet)
            if isinstance(packet, dict):
                if packet['type'] == 'ota_control' and packet['op'] == 'commit':
                    transfer.feed(dict(transfer_id=transfer.transfer_id,state='complete',offset=len(received)))
                    transfer.disconnected()  # reboot can race the coroutine's next turn
                return
            magic, key, offset, size, crc = struct.unpack_from('<4sQIII', packet)
            self.assertEqual(magic,b'DWO1'); self.assertEqual(key,int(transfer.transfer_id,16))
            self.assertEqual(len(packet)-24,size); self.assertEqual(zlib.crc32(packet[24:]),crc)
            self.assertLessEqual(size, 1024)  # avoid a multi-segment payload on the device's bounded read
            if offset == len(received): received.extend(packet[24:])
            else: self.assertEqual(received[offset:offset+size],packet[24:])
            if drop_first_ack:
                drop_first_ack=False; return
            transfer.feed(dict(transfer_id=transfer.transfer_id,state='progress',offset=offset+size))
        transfer = OtaTransfer(fw,b'k'*16,'b'*32,send,ack_timeout=.01)
        task=asyncio.create_task(transfer.run())
        await asyncio.sleep(.02)
        self.assertEqual(len(packets),1)  # offer only, no flash data before consent
        transfer.feed(dict(transfer_id=transfer.transfer_id,state='ready',offset=0))
        await task
        self.assertEqual(received,fw.data)
        self.assertEqual(transfer.state,'rebooting')  # not yet verified running
        self.assertEqual(sum(isinstance(p,bytes) for p in packets),
                         (len(fw.data) + CHUNK_BYTES - 1) // CHUNK_BYTES + 1)

    async def test_cancel_disconnect_and_timeout_never_send_firmware(self):
        for reason in ('cancelled','disconnected','timeout'):
            sent=[]
            async def send(packet): sent.append(packet)
            transfer=OtaTransfer(validate_firmware(firmware_bytes()),b'k'*16,'b'*32,send,approval_timeout=.02)
            task=asyncio.create_task(transfer.run()); await asyncio.sleep(0)
            if reason=='cancelled': transfer.feed(dict(transfer_id=transfer.transfer_id,state='cancelled',reason='cancelled'))
            elif reason=='disconnected': transfer.disconnected()
            await task
            self.assertFalse(any(isinstance(p,bytes) for p in sent))
            self.assertIn(transfer.state,{'cancelled','error'})

    async def test_real_websocket_pair_offer_transfer_and_boot_confirmation(self):
        with tempfile.TemporaryDirectory() as directory:
            key=b'k'*16; nonce='b'*32; fw=validate_firmware(firmware_bytes())
            bridge=Bridge(WallpaperLibrary(Path(directory)),pairing_key=key)
            with self.assertRaises(OtaError): await bridge.start_ota(fw.data)
            async with serve(bridge.handle_client,'127.0.0.1',0,subprotocols=['arduino']) as server:
                port=server.sockets[0].getsockname()[1]
                async with connect(f'ws://127.0.0.1:{port}',subprotocols=['arduino']) as ws:
                    await ws.send(json.dumps(dict(v=1,type='hello',device='sim',width=480,height=320,
                        firmware='0.7.0',capabilities=['ota','claude_companion'],ota_nonce=nonce)))
                    challenge=json.loads(await ws.recv())
                    with self.assertRaises(OtaError): await bridge.start_ota(fw.data)
                    await ws.send(json.dumps(dict(v=1,type='auth_response',mac=pairing_mac(key,challenge['nonce']))))
                    await ws.recv() # paired agent snapshot
                    await ws.send(json.dumps(dict(v=1,type='ota_boot',firmware='0.7.0',state='valid',image_sha256='c'*64)))
                    for _ in range(100):
                        if any(d.get('image_sha256') == 'c'*64 for d in bridge.device_info.values()): break
                        await asyncio.sleep(.001)
                    await bridge.start_ota(fw.data)
                    with self.assertRaises(OtaError): await bridge.start_ota(fw.data)
                    offer=json.loads(await ws.recv()); tid=offer['transfer_id']
                    self.assertEqual(offer['mac'],offer_mac(key,tid,fw,nonce))
                    async def ack(state,offset):
                        await ws.send(json.dumps(dict(v=1,type='ota_ack',transfer_id=tid,state=state,offset=offset,reason='')))
                    await ack('ready',0)
                    received=bytearray()
                    while True:
                        packet=await ws.recv()
                        if isinstance(packet,str):
                            self.assertEqual(json.loads(packet)['op'],'commit');break
                        _,_,offset,size,crc=struct.unpack_from('<4sQIII',packet)
                        self.assertEqual(offset,len(received));self.assertEqual(crc,zlib.crc32(packet[24:]))
                        received.extend(packet[24:]);await ack('progress',len(received))
                    self.assertEqual(received,fw.data)
                    await ack('complete',len(received));await bridge.ota_transfer.task
                    self.assertEqual(bridge.ota_transfer.state,'rebooting')
                    boot=dict(v=1,type='ota_boot',firmware='0.7.1',image_sha256=fw.data[-32:].hex())
                    for device_state,expected in [('pending','validating'),('valid','complete')]:
                        await ws.send(json.dumps(dict(boot,state=device_state)))
                        for _ in range(100):
                            if bridge.ota_transfer.state==expected:break
                            await asyncio.sleep(.001)
                        self.assertEqual(bridge.ota_transfer.state,expected)
                    bridge.ota_transfer.state='rebooting'
                    # A different hash alone is not evidence of a rollback.
                    for version,digest,device_state,expected in [
                        ('0.7.1','d'*64,'valid','unconfirmed'),
                        ('0.7.1',fw.data[-32:].hex(),'unknown','unconfirmed'),
                        ('0.7.0','c'*64,'valid','rolled_back')]:
                        await ws.send(json.dumps(dict(v=1,type='ota_boot',firmware=version,state=device_state,image_sha256=digest)))
                        await asyncio.sleep(.02)
                        self.assertEqual(bridge.ota_transfer.state,expected)
            await bridge.shutdown()


class HttpTests(unittest.TestCase):
    def test_binary_upload_is_bounded_and_same_origin(self):
        def completed(value):
            future=Future();future.set_result(value);return future
        received=[]
        def upload(data): received.append(data);return completed({'state':'offered'})
        with tempfile.TemporaryDirectory() as directory:
            editor=WallpaperEditorService(WallpaperLibrary(Path(directory)),Path(__file__).resolve().parents[1],0,
                lambda _:completed({}),lambda:completed({}),lambda _:completed({}),ota=upload)
            editor.start()
            try:
                url=f'http://127.0.0.1:{editor.port}/api/ota';data=firmware_bytes()
                with urlopen(Request(url,data=data,headers={'Content-Type':'application/octet-stream'}),timeout=3) as response:
                    self.assertEqual(response.status,202)
                for headers,code in [({'Content-Type':'text/plain'},400),
                    ({'Content-Type':'application/octet-stream','Origin':'https://evil.invalid'},403),
                    ({'Content-Type':'application/octet-stream','Host':'evil.invalid'},403)]:
                    with self.assertRaises(HTTPError) as error:
                        urlopen(Request(url,data=b'bad',headers=headers),timeout=3)
                    self.assertEqual(error.exception.code,code)
                self.assertEqual(received,[data])
            finally:editor.stop()
