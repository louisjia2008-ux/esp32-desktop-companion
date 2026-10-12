"""Real loopback requests exercise the same Host/Origin checks as the editor."""
import asyncio
import http.client
import json
from pathlib import Path
import tempfile
import threading
import unittest
from concurrent.futures import Future
from bridge import Bridge
from wallpaper_library import WallpaperLibrary
from wallpaper_http import WallpaperEditorService

def resolved(value):
    future=Future();future.set_result(value);return future

class HubHTTPTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.library=WallpaperLibrary(Path(self.temp.name)/'wallpapers')
        self.loop=asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        self.bridge=Bridge(self.library)
        self.thread=threading.Thread(target=self.loop.run_forever,daemon=True);self.thread.start()
        try:
            self.service=WallpaperEditorService(self.library,Path(__file__).resolve().parents[1],0,
                lambda _:resolved({}),lambda:resolved({}),lambda _:resolved({}),
                hub=lambda section,method,payload:asyncio.run_coroutine_threadsafe(self.bridge.hub_api(section,method,payload),self.loop))
        except PermissionError:
            self.loop.call_soon_threadsafe(self.loop.stop);self.thread.join();self.loop.close();self.temp.cleanup()
            self.skipTest('loopback bind is unavailable in this sandbox')
        self.service.start()
    def tearDown(self):
        self.service.stop()
        asyncio.run_coroutine_threadsafe(self.bridge.shutdown(),self.loop).result(5)
        self.loop.call_soon_threadsafe(self.loop.stop);self.thread.join();self.loop.close();asyncio.set_event_loop(None);self.temp.cleanup()
    def request(self,path,method='GET',payload=None,headers=None):
        connection=http.client.HTTPConnection('127.0.0.1',self.service.port,timeout=3)
        body=json.dumps(payload).encode() if payload is not None else None
        h={'Content-Type':'application/json'} if body is not None else {}
        h.update(headers or {})
        connection.request(method,path,body,h);response=connection.getresponse()
        result=(response.status,json.loads(response.read()),dict(response.getheaders()));connection.close();return result
    def test_card_roundtrip_headers_and_scope(self):
        status,result,headers=self.request('/api/hub/cards','POST',dict(kind='link',title='二维码',url='https://example.com/'))
        self.assertEqual(status,200);card=result['card']
        self.assertEqual(headers['Cache-Control'],'no-store');self.assertEqual(headers['X-Frame-Options'],'DENY')
        self.assertEqual(self.request('/api/hub/cards')[1]['cards'][0]['id'],card['id'])
        self.assertEqual(self.request('/api/hub/cards','POST',dict(operation='pin',id=card['id'],pinned=True))[0],200)
        self.assertTrue(self.request('/api/hub/cards')[1]['cards'][0]['pinned'])
        self.assertEqual(self.request('/api/hub/cards','POST',dict(operation='clear',id=card['id']))[0],200)
        self.assertFalse(self.request('/api/hub/cards')[1]['cards'])
    def test_rejects_remote_host_origin_and_cross_site(self):
        for headers in [{'Host':'attacker.test'},{'Origin':'https://attacker.test'},{'Sec-Fetch-Site':'cross-site'}]:
            self.assertEqual(self.request('/api/hub/cards','POST',dict(kind='text',title='x'),headers)[0],403)
        self.assertFalse(self.bridge.hub.data['cards'])
    def test_invalid_fields_actions_and_large_body(self):
        self.assertEqual(self.request('/api/hub/cards','POST',dict(kind='file',title='x',path='/private/file'))[0],400)
        self.assertEqual(self.request('/api/hub/actions','POST',dict(operation_id='x',action='exec',target='ls'))[0],400)
        self.assertEqual(self.request('/api/hub/cards','POST',dict(kind='text',title='x',body='a'*17000))[0],400)
        self.assertEqual(self.request('/api/hub/tasks','POST',[])[0],400)
        self.assertEqual(self.request('/api/hub/unknown')[0],400)
    def test_task_sequence_and_registered_action_result(self):
        import time
        task=dict(id='test',source='wrapper',title='测试',seq=2,state='running',updated_at=int(time.time()))
        self.assertTrue(self.request('/api/hub/tasks','POST',task)[1]['accepted'])
        task.update(seq=1,state='failed')
        self.assertFalse(self.request('/api/hub/tasks','POST',task)[1]['accepted'])
        self.assertEqual(self.request('/api/hub/tasks')[1]['tasks'][0]['state'],'running')
        self.bridge.hub.result('known','succeeded','已确认')
        action=dict(operation_id='known',action='airdrop_open')
        self.assertEqual(self.request('/api/hub/actions','POST',action)[1]['state'],'succeeded')
        self.assertEqual(self.request('/api/hub/actions?operation_id=known')[1]['reason'],'已确认')
