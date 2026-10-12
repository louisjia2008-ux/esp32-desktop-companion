from __future__ import annotations
import asyncio,json,tempfile,time,unittest
from pathlib import Path
from unittest import mock
from desktop_hub import HubStore,HubController,HubError,validate_action,THRESHOLDS,CAPABILITY,BATCH_CAPABILITY,PACKET_LIMIT
from codex_usage import normalize_limits,CodexUsageCollector
from bridge import Bridge,parse_device_message,ProtocolError,pairing_mac,encode_message
from wallpaper_library import WallpaperLibrary
from test_bridge import FakeConnection

class StateTests(unittest.TestCase):
 def setUp(self):self.now=1800000000;self.store=HubStore(clock=lambda:self.now)
 def record(self,used,minutes=300,reset=None,lid='codex',state='fresh'):
  return dict(provider='codex',limit_id=lid,window_minutes=minutes,used_percent=used,resets_at=reset or self.now+1800,observed_at=self.now,state=state)
 def usage(self,used,**kw):return self.store.usage_submit(dict(source='collector',account_scope='account1',records=[self.record(used,**kw)]))
 def test_cards_limits_and_expiry(self):
  with self.assertRaises(HubError):self.store.card(dict(kind='text',title='长'*17,body='x'))
  with self.assertRaises(HubError):self.store.card(dict(kind='text',title='x',body='x'*513))
  with self.assertRaises(HubError):self.store.card(dict(kind='link',title='x',url='file:///secret'))
  for i in range(6):self.store.card(dict(kind='text',title=str(i),body='x'))
  self.assertEqual(len(self.store.data['cards']),5)
  pinned=self.store.data['cards'][0]['id'];self.store.card_action('card_pin',pinned,True)
  self.now+=1801;self.store.expire();self.assertEqual([c['id'] for c in self.store.data['cards']],[pinned])
 def test_all_pinned_requires_manual_clear(self):
  for i in range(5):c=self.store.card(dict(kind='text',title=str(i)));self.store.card_action('card_pin',c['id'],True)
  with self.assertRaises(HubError):self.store.card(dict(kind='text',title='6'))
 def test_native_file_required(self):
  with self.assertRaises(HubError):self.store.card(dict(kind='file',title='x'))
 def test_file_reference_survives_creation_and_reboot(self):
  with tempfile.TemporaryDirectory() as d:
   path=(Path(d)/'example.txt').resolve();path.write_text('public test data')
   self.store=HubStore(Path(d)/'state.json',lambda:self.now)
   card=self.store.card(dict(kind='file',title='分享'),str(path))
   self.assertEqual(self.store.data['files'][card['file_ref']],str(path))
   self.store=HubStore(Path(d)/'state.json',lambda:self.now)
   self.assertEqual(self.store.data['files'][card['file_ref']],str(path))
   self.assertNotIn(str(path),json.dumps(self.store.packets()))
   self.store.card_action('card_clear',card['id']);self.assertFalse(self.store.data['files'])
 def test_packets_are_bounded_and_private(self):
  self.store.card(dict(kind='link',title='链接',url='https://example.com/'+'x'*490))
  self.usage(45)
  for packet in self.store.packets():
   raw=json.dumps(packet,ensure_ascii=False,separators=(',',':'))
   self.assertLessEqual(len(raw.encode()),2048);self.assertNotIn('account_scope',raw);self.assertNotIn('account1',raw)
 def test_control_batch_64_rows_and_legacy_default(self):
  self.store.control=[dict(kind='wifi',id=f'n{i}',name=f'Network {i}') for i in range(64)]
  self.store.card(dict(kind='link',title='链接',url='https://example.com/'))
  legacy=self.store.packets();batched=self.store.packets(batch_control=True)
  for packets,stride,count in ((legacy,1,64),(batched,4,16)):
   control=[p for p in packets if p['section']=='control']
   self.assertEqual(len(control),count);self.assertEqual([p['page'] for p in control],list(range(count)))
   self.assertTrue(all(p['pages']==count and p['total']==64 and len(p['items'])==stride for p in control))
   self.assertEqual([item for p in control for item in p['items']],self.store.control)
   self.assertEqual({p['revision'] for p in packets},{self.store.data['revision']})
   self.assertTrue(all(len(encode_message(p).encode('utf-8'))<=PACKET_LIMIT for p in packets))
  self.assertEqual([p for p in batched if p['section']!='control'],[p for p in legacy if p['section']!='control'])
 def test_control_batch_empty_exact_and_partial_last_page(self):
  for total,counts in ((0,[0]),(1,[1]),(4,[4]),(9,[4,4,1])):
   with self.subTest(total=total):
    self.store.control=[dict(kind='audio',id=f'a{i}',name='Output') for i in range(total)]
    packets=[p for p in self.store.packets(batch_control=True) if p['section']=='control']
    self.assertEqual([len(p['items']) for p in packets],counts)
    self.assertTrue(all(p['pages']==len(counts) and p['total']==total for p in packets))
 def test_control_batch_falls_back_whole_section_for_worst_fields(self):
  # UTF-8 names use their full 48-byte allowance; escaped reasons occupy
  # twice their 128-byte input length on the wire. The oversized group is
  # deliberately after a small first page, so fallback must rebuild both.
  largest=dict(kind='bluetooth',id='x'*32,name='网'*16,state='not_provided',reason='\\'*128,
               settings='bluetooth',selected=False,connected=False,volume=100,
               volume_supported=False,mute_supported=False,muted=False)
  self.store.control=[dict(kind='wifi',id=f'n{i}',name='Network') for i in range(4)]+[dict(largest) for _ in range(5)]
  self.store.data['revision']=0xffffffff;self.now=0xffffffff
  packets=[p for p in self.store.packets(batch_control=True) if p['section']=='control']
  self.assertEqual(len(packets),9);self.assertEqual([p['page'] for p in packets],list(range(9)))
  self.assertTrue(all(p['pages']==9 and p['total']==9 and len(p['items'])==1 for p in packets))
  self.assertEqual([p['items'][0] for p in packets],self.store.control)
  self.assertTrue(all(len(encode_message(p).encode('utf-8'))<=PACKET_LIMIT for p in packets))
  self.assertEqual(packets,[p for p in self.store.packets() if p['section']=='control'])
 def test_snapshot_limit_counts_utf8_bytes_at_exact_boundary(self):
  self.store.control=[dict(kind='status',reason='网')]
  packet=next(p for p in self.store.packets() if p['section']=='control')
  padding=PACKET_LIMIT-len(encode_message(packet).encode('utf-8'))
  self.store.control[0]['reason']+='x'*padding
  for batch_control in (False,True):
   packet=next(p for p in self.store.packets(batch_control=batch_control) if p['section']=='control')
   self.assertEqual(len(encode_message(packet).encode('utf-8')),PACKET_LIMIT)
  self.store.control[0]['reason']+='x'
  for batch_control in (False,True):
   with self.assertRaises(HubError):self.store.packets(batch_control=batch_control)
 def task(self,seq=0,state='running',tid='task1',**kw):
  return self.store.task_update(dict(id=tid,source='wrapper',title='构建',state=state,seq=seq,updated_at=self.now,**kw))
 def test_old_task_sequence_and_stale(self):
  self.task(2);self.assertFalse(self.task(1,'failed')['accepted'])
  self.now+=45;self.store.expire();self.assertEqual(self.store.data['tasks'][0]['state'],'stale')
  self.task(3);self.assertEqual(self.store.data['tasks'][0]['state'],'running')
 def test_terminal_cannot_heartbeat_back_to_running(self):
  self.task(1,'success')
  with self.assertRaises(HubError):self.task(2,'running')
 def test_task_notifications_once(self):
  self.task(1);self.task(2);self.assertEqual(self.store.pending_alerts,[])
  self.task(3,'success');self.task(4,'success');self.assertEqual(len(self.store.pending_alerts),1)
 def test_viewed_terminal_evicted_first(self):
  self.task(1,'success','first');self.store.task_viewed('first')
  for i in range(32):self.task(1,tid=f't{i}')
  self.assertEqual(len(self.store.data['tasks']),32);self.assertNotIn('first',[t['id'] for t in self.store.data['tasks']])
 def test_only_week_window_missing_five_hour(self):
  self.usage(41,minutes=10080)
  records=self.store.sections()['usage'];missing=next(r for r in records if r['window_minutes']==300)
  self.assertIsNone(missing['used_percent']);self.assertEqual(missing['state'],'not_provided')
  self.assertEqual(missing['observed_at'],self.now)
  self.now+=30
  self.assertEqual(self.store.sections()['usage'],records) # Reads cannot invent a new observation or defeat snapshot dedup.
 def test_claude_slots_are_reserved_without_collection_or_alerts(self):
  reserved=[r for r in self.store.sections()['usage'] if r['provider']=='claude']
  self.assertEqual([r['window_minutes'] for r in reserved],[300,10080])
  self.assertTrue(all(r['used_percent'] is None and r['observed_at']==0 and r['state']=='not_provided' for r in reserved))
  self.assertFalse(self.store.data['usage']);self.assertFalse(self.store.pending_alerts)
  record=self.record(66);record['provider']='claude';record['limit_id']='claude'
  self.store.usage_submit(dict(source='claude-collector',account_scope='test-account',records=[record]))
  records=[r for r in self.store.sections()['usage'] if r['provider']=='claude' and r['window_minutes']==300]
  self.assertEqual(len(records),1);self.assertEqual(records[0]['used_percent'],66)
 def test_eight_thresholds(self):
  self.usage(0)
  for remaining in THRESHOLDS:
   self.usage(100-remaining)
   self.assertEqual(len(self.store.pending_alerts),1);self.store.pending_alerts.clear()
   self.usage(100-remaining);self.assertEqual(self.store.pending_alerts,[])
 def test_first_low_only_most_urgent(self):
  self.usage(97);self.assertEqual(len(self.store.pending_alerts),1)
  self.assertEqual(self.store.data['alerts']['collector:codex:codex:300']['seen'],[25,20,15,10,5,3])
 def test_cross_thresholds_merge(self):
  self.usage(0);self.usage(99);self.assertEqual(len(self.store.pending_alerts),1)
 def test_small_reset_timestamp_drift_does_not_repeat(self):
  reset=self.now+1800;self.usage(95,reset=reset);self.store.pending_alerts.clear()
  self.usage(95,reset=reset+1);self.assertFalse(self.store.pending_alerts)
 def test_multiple_windows_merge(self):
  self.store.usage_submit(dict(source='collector',account_scope='a',records=[self.record(95),self.record(99,10080)]))
  self.assertEqual(len(self.store.pending_alerts),1);self.assertIn('5h',self.store.pending_alerts[0]['message']);self.assertIn('周',self.store.pending_alerts[0]['message'])
 def test_error_never_becomes_zero(self):
  self.usage(70);self.store.usage_error('collector');self.assertEqual(self.store.pending_alerts,[])
  self.assertEqual(self.store.sections()['usage'][0]['used_percent'],70)
 def test_stale_never_alerts(self):
  r=self.record(100);r['observed_at']-=601
  self.store.usage_submit(dict(source='collector',account_scope='a',records=[r]));self.assertFalse(self.store.pending_alerts)
 def test_account_change_and_new_window(self):
  self.usage(95);self.store.pending_alerts.clear()
  self.store.usage_submit(dict(source='collector',account_scope='account2',records=[self.record(95)]));self.assertEqual(len(self.store.pending_alerts),1)
 def test_persist_dedup_and_interrupted_operation(self):
  with tempfile.TemporaryDirectory() as d:
   self.store=HubStore(Path(d)/'state.json',lambda:self.now);self.usage(99);self.store.result('op','running')
   self.store=HubStore(Path(d)/'state.json',lambda:self.now);self.usage(99)
   self.assertEqual(self.store.pending_alerts,[]);self.assertEqual(self.store.data['operations']['op']['state'],'unconfirmed')
 def test_reset_confirmed_recovery(self):
  old=self.now+100;self.usage(100,reset=old);self.store.pending_alerts.clear();self.now=old
  self.store.expire();self.assertEqual(self.store.data['usage'][0]['used_percent'],100)
  self.usage(0,reset=old+1800);self.assertEqual(self.store.pending_alerts[0]['title'],'额度恢复')
 def test_window_order_and_legacy(self):
  primary={'usedPercent':30,'windowDurationMins':10080,'resetsAt':self.now+99}
  secondary={'usedPercent':20,'windowDurationMins':300,'resetsAt':self.now+88}
  for p in [dict(rateLimits={'limitId':'codex','primary':primary,'secondary':secondary}),dict(rateLimitsByLimitId={'codex':{'primary':primary,'secondary':secondary}},rateLimits={'primary':secondary})]:
   records=normalize_limits(p,self.now);self.assertEqual([r['window_minutes'] for r in records],[10080,300])
 def test_missing_fields_no_fresh_zero(self):
  r=normalize_limits({'rateLimits':{'primary':{'usedPercent':None,'windowDurationMins':300}}},self.now)[0]
  self.assertEqual(r['state'],'not_provided');self.assertIsNone(r['used_percent'])
 def test_arbitrary_actions_paths_rejected(self):
  for payload in [dict(operation_id='x',action='exec',target='ls'),dict(operation_id='x',action='audio_select',target='/tmp'),dict(operation_id='x',action='volume_set',value=101),dict(operation_id='x',action='volume_set',value=20,command='ls')]:
   with self.assertRaises(HubError):validate_action(payload)

class AsyncTests(unittest.IsolatedAsyncioTestCase):
 def hub_connection(self,bridge,capabilities,authenticated=True):
  connection=FakeConnection();bridge.clients.add(connection);bridge.ready_clients.add(connection)
  bridge.send_locks[connection]=asyncio.Lock();bridge.client_capabilities[connection]=frozenset(capabilities)
  if authenticated:bridge.authenticated_clients.add(connection)
  return connection
 async def test_batch_capability_routes_only_negotiated_authenticated_devices(self):
  with tempfile.TemporaryDirectory() as d:
   bridge=Bridge(WallpaperLibrary(Path(d)),pairing_key=b'x'*16)
   legacy=self.hub_connection(bridge,{CAPABILITY})
   batched=self.hub_connection(bridge,{CAPABILITY,BATCH_CAPABILITY})
   batch_only=self.hub_connection(bridge,{BATCH_CAPABILITY})
   unpaired=self.hub_connection(bridge,{CAPABILITY,BATCH_CAPABILITY},False)
   for connection in (legacy,batched,batch_only,unpaired):
    hello=parse_device_message(json.dumps(dict(v=1,type='hello',device='batch-test',firmware='test',
          width=480,height=320,capabilities=list(bridge.client_capabilities[connection]))))
    await bridge.handle_device_message(connection,hello);connection.sent.clear()
   bridge.hub.control=[dict(kind='wifi',id=f'n{i}',name=f'Network {i}') for i in range(64)]
   with mock.patch('bridge.asyncio.sleep',new=mock.AsyncMock()):
    await asyncio.gather(*(bridge._send_hub(c) for c in (legacy,batched,batch_only,unpaired)))
    for connection,count,stride in ((legacy,64,1),(batched,16,4)):
     packets=[json.loads(raw) for raw in connection.sent]
     control=[p for p in packets if p['section']=='control']
     self.assertEqual(len(control),count)
     self.assertEqual([p['page'] for p in control],list(range(count)))
     self.assertTrue(all(p['pages']==count and p['total']==64 and len(p['items'])==stride for p in control))
     self.assertEqual([item for p in control for item in p['items']],bridge.hub.control)
     self.assertTrue(all(len(p['items'])<=1 for p in packets if p['section']!='control'))
     self.assertTrue(all(len(raw.encode('utf-8'))<=PACKET_LIMIT for raw in connection.sent))
     connection.sent.clear()
    self.assertFalse(batch_only.sent);self.assertFalse(unpaired.sent)
    await asyncio.gather(bridge._send_hub(legacy),bridge._send_hub(batched))
    self.assertFalse(legacy.sent);self.assertFalse(batched.sent)
    bridge.hub.task_update(dict(id='active',source='wrapper',title='Test',state='running',seq=1,updated_at=int(time.time())))
    await asyncio.gather(bridge._send_hub(legacy),bridge._send_hub(batched))
    for connection in (legacy,batched):
     self.assertEqual({json.loads(raw)['section'] for raw in connection.sent},{'tasks'})
 async def test_batch_revisions_finish_before_new_control_and_retry_after_interruption(self):
  with tempfile.TemporaryDirectory() as d:
   bridge=Bridge(WallpaperLibrary(Path(d)),pairing_key=b'x'*16)
   connection=self.hub_connection(bridge,{CAPABILITY,BATCH_CAPABILITY})
   bridge.hub.control=[dict(kind='audio',id=f'a{i}',name='Before') for i in range(9)]
   revision=bridge.hub.data['revision'];started=asyncio.Event();resume=asyncio.Event()
   original_send=connection.send
   async def paused_send(raw):
    await original_send(raw)
    packet=json.loads(raw)
    if packet['section']=='control' and packet['revision']==revision and packet['page']==0:
     started.set();await resume.wait()
   connection.send=paused_send
   with mock.patch('bridge.asyncio.sleep',new=mock.AsyncMock()):
    first=asyncio.create_task(bridge._send_hub(connection))
    await asyncio.wait_for(started.wait(),1)
    bridge.hub.control[0]['name']='After';bridge.hub.changed()
    second=asyncio.create_task(bridge._send_hub(connection));resume.set()
    await asyncio.gather(first,second)
    control=[json.loads(raw) for raw in connection.sent if json.loads(raw)['section']=='control']
    self.assertEqual([(p['revision'],p['page']) for p in control],[(revision,p) for p in range(3)]+[(revision+1,p) for p in range(3)])
    self.assertEqual(control[0]['items'][0]['name'],'Before');self.assertEqual(control[3]['items'][0]['name'],'After')
    previous=bridge.hub_sent_sections[connection]['control']
    bridge.hub.control[0]['name']='Retry';bridge.hub.changed();connection.sent.clear()
    async def interrupted_send(raw):
     packet=json.loads(raw)
     if packet['section']=='control' and packet['page']==1:raise RuntimeError('fixture interruption')
     await original_send(raw)
    connection.send=interrupted_send
    with self.assertRaises(RuntimeError):await bridge._send_hub(connection)
    self.assertEqual(bridge.hub_sent_sections[connection]['control'],previous)
    connection.send=original_send;connection.sent.clear();await bridge._send_hub(connection)
    self.assertEqual([json.loads(raw)['page'] for raw in connection.sent],[0,1,2])
    self.assertNotEqual(bridge.hub_sent_sections[connection]['control'],previous)
 async def test_operation_result_precedes_waiting_snapshot_lock(self):
  with tempfile.TemporaryDirectory() as d:
   bridge=Bridge(WallpaperLibrary(Path(d)),pairing_key=b'x'*16)
   connection=self.hub_connection(bridge,{CAPABILITY,BATCH_CAPABILITY})
   result=dict(v=1,type='hub_result',operation_id='op',state='succeeded',reason='Confirmed')
   bridge.hub_controller.execute=mock.AsyncMock(return_value=result)
   lock=bridge.hub_send_locks.setdefault(connection,asyncio.Lock());await lock.acquire()
   delivered=asyncio.Event();original_send=connection.send
   async def observe_result(raw):
    await original_send(raw)
    if json.loads(raw)['type']=='hub_result':delivered.set()
   connection.send=observe_result
   with mock.patch('bridge.asyncio.sleep',new=mock.AsyncMock()):
    pending=asyncio.create_task(bridge._hub_device_action(connection,dict(operation_id='op',action='airdrop_open')))
    try:
     await asyncio.wait_for(delivered.wait(),1)
     self.assertEqual(json.loads(connection.sent[0]),result);self.assertTrue(lock.locked())
    finally:
     lock.release();await pending
 async def test_duplicate_operation_executes_once(self):
  class Native:
   def __init__(self):self.calls=[]
   async def call(self,action,**args):self.calls.append(action);await asyncio.sleep(.005);return {'state':'succeeded'}
  native=Native();store=HubStore();controller=HubController(store,native)
  payload=dict(operation_id='same',action='airdrop_open')
  await asyncio.gather(controller.execute(payload),controller.execute(payload));await controller.execute(payload)
  self.assertEqual(native.calls,['airdrop_open']);self.assertEqual(store.data['operations']['same']['state'],'succeeded')
 async def test_wrong_target_kind_fails(self):
  controller=HubController(HubStore(),object());controller.targets['foo']=('wifi','net')
  result=await controller.execute(dict(operation_id='op',action='audio_select',target='foo'))
  self.assertEqual(result['state'],'failed')
 async def test_failed_native_control_refreshes_actual_output(self):
  class Native:
   async def call(self,action,**args):
    if action!='state':return {'state':'failed','reason':'headset unavailable'}
    if args['surface']=='audio':return {'items':[{'kind':'status','state':'fresh','volume':50,'volume_supported':True},
      {'kind':'audio','native_id':'speaker','name':'Speaker','selected':True}]}
    return {'items':[]}
  for name in ['volume_set','audio_select','bluetooth_connect']:
   with self.subTest(action=name):
    store=HubStore();store.control=[{'kind':'audio','id':'old','name':'Headset','selected':True}]
    controller=HubController(store,Native());controller.targets['old']=('bluetooth' if name=='bluetooth_connect' else 'audio','headset')
    payload=dict(operation_id='op',action=name)
    if name=='volume_set':payload['value']=40
    else:payload['target']='old'
    result=await controller.execute(payload)
    self.assertEqual(result['state'],'failed');self.assertEqual(result['reason'],'headset unavailable')
    self.assertEqual([row['name'] for row in store.control if row['kind']=='audio'],['Speaker'])
    self.assertNotIn('old',controller.targets)
 async def test_retired_operation_does_not_replay(self):
  store=HubStore();store.result('original','succeeded')
  for i in range(129):store.result(f'op{i}','succeeded')
  class Native:
   async def call(self,*args,**kw):raise AssertionError('retired action replayed')
  result=await HubController(store,Native()).execute(dict(operation_id='original',action='airdrop_open'))
  self.assertEqual(result['state'],'unconfirmed');self.assertIn('original',store.data['retired_operations'])
 async def test_moved_file_reports_failure(self):
  with tempfile.TemporaryDirectory() as d:
   path=Path(d)/'share.txt';path.write_text('fixture');store=HubStore()
   card=store.card(dict(kind='file',title='file'),str(path));path.unlink()
   result=await HubController(store,object()).execute(dict(operation_id='op',action='file_share',target=card['file_ref']))
   self.assertEqual(result['state'],'failed');self.assertIn('moved or deleted',result['reason'])
 async def test_http_operation_capacity_is_terminal(self):
  with tempfile.TemporaryDirectory() as d:
   bridge=Bridge(WallpaperLibrary(Path(d)));bridge.hub.data['retired_operations']=['retired']*8192
   result=await bridge.hub_api('actions','POST',dict(operation_id='new',action='airdrop_open'))
   self.assertEqual(result['state'],'failed');self.assertFalse(bridge.hub_jobs)
 async def test_unpaired_and_old_device_hub_gates(self):
  with tempfile.TemporaryDirectory() as d:
   bridge=Bridge(WallpaperLibrary(Path(d)),pairing_key=b'x'*16)
   c=FakeConnection();bridge.clients.add(c);bridge.ready_clients.add(c);bridge.send_locks[c]=asyncio.Lock();bridge.client_capabilities[c]=frozenset({'desktop_hub_v1'})
   payload=dict(v=1,type='hub_action',operation_id='op',action='airdrop_open')
   await bridge.handle_device_message(c,payload);await bridge._send_hub(c);self.assertFalse(c.sent)
   bridge.authenticated_clients.add(c);bridge.client_capabilities[c]=frozenset({'host_status'})
   await bridge.handle_device_message(c,payload);await bridge._send_hub(c);self.assertFalse(c.sent)
   bridge.client_capabilities[c]=frozenset({'desktop_hub_v1'});await bridge._send_hub(c)
   self.assertTrue(any(json.loads(x)['type']=='hub_snapshot' for x in c.sent))
 async def test_read_only_method_guard(self):
  collector=CodexUsageCollector(HubStore())
  with self.assertRaises(HubError):await collector.rpc('turn/start')
 async def test_snapshot_versions_do_not_interleave_or_repeat_unchanged_control(self):
  with tempfile.TemporaryDirectory() as d:
   bridge=Bridge(WallpaperLibrary(Path(d)),pairing_key=b'x'*16)
   c=FakeConnection();bridge.clients.add(c);bridge.ready_clients.add(c);bridge.authenticated_clients.add(c)
   bridge.send_locks[c]=asyncio.Lock();bridge.client_capabilities[c]=frozenset({'desktop_hub_v1'})
   bridge.hub.control=[dict(kind='wifi',id=f'n{i}',name=f'Network {i}') for i in range(4)]
   await asyncio.gather(bridge._send_hub(c),bridge._send_hub(c))
   packets=[json.loads(x) for x in c.sent];control=[p for p in packets if p['section']=='control']
   self.assertEqual([p['page'] for p in control],[0,1,2,3]);self.assertEqual(len({p['revision'] for p in control}),1)
   self.assertEqual(packets[0]['section'],'usage')
   c.sent.clear();bridge.hub.task_update(dict(id='active',source='wrapper',title='Test',state='running',seq=1,updated_at=int(time.time())))
   await bridge._send_hub(c)
   sections={json.loads(x)['section'] for x in c.sent}
   self.assertIn('tasks',sections);self.assertNotIn('control',sections)
   bridge._discard_client(c);self.assertNotIn(c,bridge.hub_sent_sections)
 def test_new_protocol_and_large_packets(self):
  p=dict(v=1,type='hub_action',operation_id='op',action='volume_set',value=20)
  self.assertEqual(parse_device_message(json.dumps(p)),p)
  with self.assertRaises(ProtocolError):parse_device_message(json.dumps(p)+(' '*2048))
