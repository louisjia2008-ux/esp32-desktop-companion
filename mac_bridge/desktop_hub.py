"""Bounded desktop hub state. Only registered references cross the device boundary."""
from __future__ import annotations
import asyncio
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import re
import secrets
import time
from urllib.parse import urlsplit

CAPABILITY = 'desktop_hub_v1'
BATCH_CAPABILITY = 'desktop_hub_batch_v1'
PACKET_LIMIT = 2048
CONTROL_LIMIT = 64
THRESHOLDS = (25, 20, 15, 10, 5, 3, 1, 0)
TASK_STATES = {'running', 'waiting_input', 'success', 'failed', 'stale'}
TERMINAL = {'success', 'failed'}
ACTIONS = {'volume_set', 'mute_set', 'audio_select', 'bluetooth_connect',
           'bluetooth_disconnect', 'wifi_select', 'airdrop_open', 'file_share',
           'card_pin', 'card_clear', 'task_viewed', 'details_open',
           'usage_refresh', 'control_refresh', 'hub_sync', 'operation_read', 'settings_open'}

class HubError(ValueError): pass

def text(value, maximum, *, empty=False):
    if not isinstance(value, str) or (not empty and not value): raise HubError('invalid text')
    if len(value.encode('utf-8')) > maximum or any(ord(c) < 32 and c not in '\n\t' for c in value):
        raise HubError('text exceeds limit or contains control characters')
    return value

def ref(value):
    if not isinstance(value,str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,32}',value): raise HubError('invalid reference')
    return value

def http_url(value):
    value=text(value,512)
    parsed=urlsplit(value)
    if parsed.scheme not in {'http','https'} or not parsed.hostname or parsed.username or parsed.password:
        raise HubError('only HTTP/HTTPS links without credentials are supported')
    return value

def validate_action(payload):
    if not isinstance(payload,dict) or set(payload)-{'v','type','operation_id','action','target','value'}:
        raise HubError('unsupported action fields')
    ref(payload.get('operation_id'))
    action=payload.get('action')
    if action not in ACTIONS: raise HubError('unsupported action')
    target=payload.get('target','')
    if target: ref(target)
    if action in {'audio_select','bluetooth_connect','bluetooth_disconnect','wifi_select',
                  'file_share','card_pin','card_clear','task_viewed','details_open','operation_read'} and not target:
        raise HubError('registered target required')
    value=payload.get('value')
    if action=='volume_set':
        if type(value) not in (int,float) or not math.isfinite(value) or not 0<=value<=100: raise HubError('volume must be 0..100')
    elif action in {'mute_set','card_pin'}:
        if type(value) is not bool: raise HubError('boolean value required')
    elif value is not None: raise HubError('unexpected value')
    if action=='settings_open' and target not in {'audio','bluetooth','wifi','privacy'}:raise HubError('unsupported settings entry')
    return {'operation_id':payload['operation_id'],'action':action,'target':target,'value':value}

class HubStore:
    def __init__(self,path:Path|None=None,clock=time.time):
        self.path=path; self.clock=clock
        self.data={'revision':1,'cards':[],'tasks':[],'usage':[], 'operations':{},
                   'files':{},'details':{},'alerts':{},'accounts':{},'retired_operations':[]}
        if path and path.exists():
            if path.stat().st_size>1024*1024:raise HubError('hub state exceeds size limit')
            loaded=json.loads(path.read_text())
            if not isinstance(loaded,dict): raise HubError('hub state is invalid')
            self.data.update(loaded)
        # A process crash cannot prove a native side effect did not occur.
        for op in self.data['operations'].values():
            if op['state']=='running': op.update(state='unconfirmed',reason='Bridge restarted; refresh actual state')
        self.control=[{'kind':'status','state':'not_provided','reason':'Mac controls not read yet'}]
        self.pending_alerts=[]
        self.save()

    def save(self):
        if not self.path:return
        self.path.parent.mkdir(parents=True,exist_ok=True)
        temp=self.path.with_suffix('.tmp')
        fd=os.open(temp,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600)
        with os.fdopen(fd,'w') as stream:
            json.dump(self.data,stream,ensure_ascii=False,separators=(',',':'));stream.flush();os.fsync(stream.fileno())
        os.replace(temp,self.path)
        os.chmod(self.path,0o600)

    def changed(self):
        self.data['revision']=(self.data['revision']+1)&0xffffffff or 1
        self.save()

    def expire(self):
        now=self.clock(); old=len(self.data['cards'])
        self.data['cards']=[c for c in self.data['cards'] if c['pinned'] or c['expires_at']>now]
        modified=len(self.data['cards'])!=old
        keep={c.get('file_ref') for c in self.data['cards']}
        self.data['files']={k:v for k,v in self.data['files'].items() if k in keep}
        for task in self.data['tasks']:
            if task['state'] in {'running','waiting_input'} and now-task['received_at']>=45:
                task['state']='stale';modified=True
        for record in self.data['usage']:
            if record['state']=='fresh' and now-record['observed_at']>600:
                record['state']='stale';modified=True
        if modified:self.changed()

    def card(self,payload,file_path=None):
        if not isinstance(payload,dict) or set(payload)-{'kind','title','body','url','ttl_minutes'}:raise HubError('unsupported card fields')
        kind=payload.get('kind'); title=text(payload.get('title'),48)
        ttl=payload.get('ttl_minutes',30)
        if type(ttl) is not int or ttl not in {5,30,120}:raise HubError('unsupported expiry')
        card={'id':secrets.token_hex(8),'kind':kind,'title':title,'body':'','url':'',
              'pinned':False,'created_at':int(self.clock()),'expires_at':int(self.clock()+ttl*60)}
        if kind=='text':card['body']=text(payload.get('body',''),512,empty=True)
        elif kind=='link':card['url']=http_url(payload.get('url'))
        elif kind=='file' and file_path:
            path=Path(file_path).resolve()
            if not path.is_file():raise HubError('selected file unavailable')
            card['file_ref']=secrets.token_hex(8);card['body']=text(path.name,512)
        else:raise HubError('file cards require native Mac picker')
        self.expire()
        cards=self.data['cards']
        if len(cards)>=5:
            victim=next((c for c in reversed(cards) if not c['pinned']),None)
            if victim is None:raise HubError('five pinned cards; clear one first')
            cards.remove(victim)
            self.data['files'].pop(victim.get('file_ref'),None)
        if kind=='file':self.data['files'][card['file_ref']]=str(path)
        cards.insert(0,card);self.changed();return copy.deepcopy(card)

    def card_action(self,action,target,value=None):
        card=next((c for c in self.data['cards'] if c['id']==target),None)
        if card is None:raise HubError('card no longer available')
        if action=='card_clear':
            self.data['cards'].remove(card);self.data['files'].pop(card.get('file_ref'),None)
        else:
            card['pinned']=value
            if not value:card['expires_at']=int(self.clock()+1800)
        self.changed()

    def task_update(self,payload):
        fields={'id','source','title','state','seq','updated_at','summary','details_url'}
        if not isinstance(payload,dict) or set(payload)-fields:raise HubError('unsupported task fields')
        tid=ref(payload.get('id'));source=ref(payload.get('source'))
        title=text(payload.get('title'),48);summary=text(payload.get('summary',''),128,empty=True)
        state=payload.get('state');seq=payload.get('seq');updated=payload.get('updated_at')
        if state not in TASK_STATES or type(seq) is not int or not 0<=seq<=0xffffffff:raise HubError('invalid task state/sequence')
        if type(updated) not in (int,float) or not math.isfinite(updated) or abs(updated-self.clock())>300:raise HubError('invalid task update time')
        old=next((t for t in self.data['tasks'] if t['id']==tid),None)
        if old and old['source']!=source:raise HubError('task source cannot change')
        if old and seq<=old['seq']:return {'accepted':False,'reason':'old_sequence'}
        if old and old.get('terminal_state') and state not in TERMINAL:raise HubError('terminal task cannot resume; create new id')
        detail=payload.get('details_url')
        if detail:self.data['details'][tid]=http_url(detail)
        task={'id':tid,'source':source,'title':title,'state':state,'seq':seq,'updated_at':int(updated),
              'received_at':int(self.clock()),'summary':summary,'viewed':old.get('viewed',False) if old and old['state']==state else False,
              'has_details':tid in self.data['details']}
        if state in TERMINAL:task['terminal_state']=state
        previous=(old or {}).get('terminal_state') or (old or {}).get('state')
        if old:self.data['tasks'].remove(old)
        self.data['tasks'].insert(0,task)
        if len(self.data['tasks'])>32:
            victim=next((t for t in reversed(self.data['tasks']) if t['viewed'] and t['state'] in TERMINAL),self.data['tasks'][-1])
            self.data['tasks'].remove(victim);self.data['details'].pop(victim['id'],None)
        self.changed()
        if previous!=state and state in TERMINAL|{'waiting_input'}:
            self.pending_alerts.append({'kind':'task','level':'error' if state=='failed' else 'warning' if state=='waiting_input' else 'success',
                                        'title':title,'message':summary or {'failed':'命令退出失败','success':'命令正常退出','waiting_input':'任务等待输入'}[state]})
        return {'accepted':True,'id':tid}

    def task_viewed(self,target):
        task=next((t for t in self.data['tasks'] if t['id']==target),None)
        if not task:raise HubError('task unavailable')
        task['viewed']=True;self.changed()

    def usage_submit(self,payload):
        if not isinstance(payload,dict) or set(payload)-{'source','account_scope','records'}:raise HubError('unsupported usage fields')
        source=ref(payload.get('source'));account=payload.get('account_scope','default')
        text(account,256)
        records=payload.get('records')
        if not isinstance(records,list) or len(records)>16:raise HubError('invalid usage record count')
        validated=[];keys=set();now=self.clock()
        for r in records:
            if not isinstance(r,dict) or set(r)!={'provider','limit_id','window_minutes','used_percent','resets_at','observed_at','state'}:raise HubError('invalid usage record fields')
            provider=ref(r['provider']);lid=text(r['limit_id'],48)
            if r['state'] not in {'fresh','stale','error','not_provided'}:raise HubError('invalid usage data state')
            window=r['window_minutes'];used=r['used_percent'];reset=r['resets_at'];observed=r['observed_at']
            if window is not None and (type(window) is not int or not 1<=window<=525600):raise HubError('invalid quota duration')
            if used is not None and (type(used) not in (int,float) or not math.isfinite(used) or not 0<=used<=100):raise HubError('invalid quota percent')
            if reset is not None and (type(reset) is not int or not 0<reset<=0xffffffff):raise HubError('invalid quota reset')
            if type(observed) not in (int,float) or not math.isfinite(observed) or abs(observed-now)>86400:raise HubError('invalid quota observation')
            if r['state']=='fresh' and (window is None or used is None or reset is None):raise HubError('fresh quota requires complete fields')
            key=(provider,lid,window)
            if key in keys:raise HubError('duplicate quota window')
            keys.add(key);record=dict(r,observed_at=int(observed));record['_source']=source
            if record['state']=='fresh' and now-observed>600:record['state']='stale'
            validated.append(record)
        # Reject late collector writes before changing state or alert dedup.
        prior=[r for r in self.data['usage'] if r.get('_source')==source]
        if prior and validated and max(r['observed_at'] for r in validated)<max(r['observed_at'] for r in prior):
            return {'accepted':False,'reason':'old_observation'}
        previous_account=self.data['accounts'].get(source)
        account_hash=hashlib.sha256(account.encode()).hexdigest()
        if previous_account!=account_hash:
            self.data['alerts']={k:v for k,v in self.data['alerts'].items() if not k.startswith(source+':')}
        self.data['accounts'][source]=account_hash
        notices=[];recovered=[]
        for r in validated:
            if r['state']!='fresh':continue
            remaining=100-r['used_percent'];key=f"{source}:{r['provider']}:{r['limit_id']}:{r['window_minutes']}"
            entry=self.data['alerts'].get(key)
            if r['resets_at']<=now:continue # reset deadline is not proof of recovery
            window_changed=bool(entry and r['resets_at']>entry['resets_at']+60 and (now>=entry['resets_at'] or remaining>entry.get('last_remaining',remaining)))
            if entry and r['resets_at']<entry['resets_at']-60:continue
            if window_changed and entry.get('exhausted') and remaining>0:recovered.append(r)
            if not entry or window_changed:
                entry={'resets_at':r['resets_at'],'seen':[],'exhausted':False};self.data['alerts'][key]=entry
                reached=[t for t in THRESHOLDS if remaining<=t]
                if reached:
                    current=min(reached);entry['seen']=reached;notices.append((r,current))
            else:
                reached=[t for t in THRESHOLDS if remaining<=t and t not in entry['seen']]
                if reached:entry['seen']+=reached;notices.append((r,min(reached)))
                if entry.get('exhausted') and remaining>0 and now>=entry['resets_at'] and not entry.get('recovered'):
                    recovered.append(r);entry['recovered']=True
            entry['last_remaining']=remaining
            if remaining<=0:entry['exhausted']=True
        self.data['usage']=[r for r in self.data['usage'] if r.get('_source')!=source]+validated
        self.changed()
        def label(r):return f"{r['provider']} {'5h' if r['window_minutes']==300 else '周' if r['window_minutes']==10080 else str(r['window_minutes'])+'m'}"
        if notices:
            self.pending_alerts.append({'kind':'usage','level':'warning','title':'用量提示',
              'message':'；'.join(label(r)+('：对应额度窗口耗尽' if t==0 else f'：剩余 {100-r["used_percent"]:g}%') for r,t in notices)})
        if recovered:self.pending_alerts.append({'kind':'usage','level':'info','title':'额度恢复','message':'；'.join(label(r)+'：已确认恢复' for r in recovered)})
        return {'accepted':True,'records':len(validated)}

    def usage_error(self,source='codex-app-server'):
        existing=[dict(r,state='error',observed_at=int(self.clock())) for r in self.data['usage'] if r.get('_source')==source]
        if not existing:existing=[{'provider':'codex','limit_id':'codex','window_minutes':None,'used_percent':None,'resets_at':None,'observed_at':int(self.clock()),'state':'error','_source':source}]
        self.data['usage']=[r for r in self.data['usage'] if r.get('_source')!=source]+existing;self.changed()

    def sections(self,sessions=()):
        self.expire()
        tasks=[{k:v for k,v in t.items() if k not in {'received_at','terminal_state'}} for t in self.data['tasks']]
        tasks+=list(sessions)
        tasks.sort(key=lambda t:({'waiting_input':0,'failed':1,'running':2,'stale':3}.get(t['state'],4),-t['updated_at']))
        usage=[{k:v for k,v in r.items() if not k.startswith('_')} for r in self.data['usage']]
        if not any(r['provider']=='codex' and r['window_minutes']==300 for r in usage):
            observed=max((r['observed_at'] for r in usage if r['provider']=='codex'),default=0)
            usage.append({'provider':'codex','limit_id':'codex','window_minutes':300,'used_percent':None,'resets_at':None,'observed_at':observed,'state':'not_provided'})
        # Reserved UI slots are never collected, persisted or fed to alerts.
        for duration in (300,10080):
            if not any(r['provider']=='claude' and r['window_minutes']==duration for r in usage):
                usage.append({'provider':'claude','limit_id':'claude','window_minutes':duration,'used_percent':None,'resets_at':None,'observed_at':0,'state':'not_provided'})
        return {'control':copy.deepcopy(self.control[:CONTROL_LIMIT]),'cards':copy.deepcopy(self.data['cards']),
                'tasks':tasks[:8],'usage':usage[:16]}

    def packets(self,sessions=(),*,batch_control=False):
        sections=self.sections(sessions);revision=self.data['revision']
        observed_at=int(self.clock())
        packets=[]
        def section_pages(section,items,stride):
            pages=max(1,(len(items)+stride-1)//stride)
            return [{'v':1,'type':'hub_snapshot','section':section,'revision':revision,'page':page,
                     'pages':pages,'total':len(items),'observed_at':observed_at,
                     'items':items[page*stride:(page+1)*stride]} for page in range(pages)]
        def oversized(packet):
            return len(json.dumps(packet,ensure_ascii=False,separators=(',',':')).encode('utf-8'))>PACKET_LIMIT
        for section,items in sections.items():
            stride=4 if batch_control and section=='control' else 1
            section_packets=section_pages(section,items,stride)
            # The receiver derives a fixed stride from pages/total. If any
            # four-row page is too large, fall back for the entire section;
            # shrinking only that page would shift every following row.
            if stride==4 and any(oversized(packet) for packet in section_packets):
                section_packets=section_pages(section,items,1)
            if any(oversized(packet) for packet in section_packets):
                raise HubError('snapshot row exceeds packet limit')
            packets.extend(section_packets)
        return packets

    def operation_lookup(self,operation_id):
        old=self.data['operations'].get(operation_id)
        if old:return copy.deepcopy(old)
        if operation_id in self.data['retired_operations']:
            return {'v':1,'type':'hub_result','operation_id':operation_id,'state':'unconfirmed','reason':'历史操作结果已归档；刷新实际状态，操作不会重放'}
        return None

    def result(self,operation_id,state,reason='',**extra):
        packet={'v':1,'type':'hub_result','operation_id':operation_id,'state':state,'reason':text(reason,128,empty=True),**extra}
        self.data['operations'][operation_id]=packet
        while len(self.data['operations'])>128:
            old_id=next(iter(self.data['operations']))
            self.data['operations'].pop(old_id)
            if old_id not in self.data['retired_operations']:self.data['retired_operations'].append(old_id)
        self.save();return copy.deepcopy(packet)

class HubController:
    def __init__(self,store,native=None,refresh_usage=None):
        self.store=store;self.native=native;self.refresh_usage=refresh_usage
        self.targets={};self.lock=asyncio.Lock();self.busy=0

    async def refresh_control(self):
        if not self.native:
            self.store.control=[{'kind':'status','state':'error','reason':'Native helper unavailable; build on Mac'}]
            self.store.changed();return
        rows=[]
        for surface in ('audio','bluetooth','wifi'):
            try:
                response=await self.native.call('state',surface=surface,_timeout=12)
                rows+=response.get('items',[])
            except (HubError,OSError,asyncio.TimeoutError):
                rows.append({'kind':'permission' if surface!='audio' else 'status','name':surface,
                             'state':'error','reason':'Mac '+surface+' 读取未完成；在系统设置检查授权','settings':surface})
        targets={};public=[]
        rows=[r for r in rows if r['kind'] in {'status','permission'}]+[r for r in rows if r['kind'] not in {'status','permission'}]
        if len(rows)>CONTROL_LIMIT:
            rows=rows[:CONTROL_LIMIT-1]+[{'kind':'permission','name':'列表','state':'not_provided','reason':'设备列表超过 64 项；其余设备请在 Mac 设置操作','settings':'wifi'}]
        for row in rows:
            row=dict(row)
            if row.get('native_id'):
                rid=hashlib.sha256((row['kind']+':'+str(row['native_id'])).encode()).hexdigest()[:16]
                targets[rid]=(row['kind'],row.pop('native_id'));row['id']=rid
            if 'name' in row:
                raw=str(row['name']).encode()[:48];row['name']=raw.decode('utf-8','ignore')
            public.append(row)
        self.targets=targets;self.store.control=public or [{'kind':'status','state':'error','reason':response.get('reason','No Mac state')}]
        self.store.changed()

    async def execute(self,payload):
        action=validate_action(payload);oid=action['operation_id']
        # Serialized registration; a duplicate returns the persisted outcome.
        async with self.lock:
            previous=self.store.operation_lookup(oid)
            if previous:return copy.deepcopy(previous)
            if len(self.store.data['retired_operations'])>=8192:
                return {'v':1,'type':'hub_result','operation_id':oid,'state':'failed','reason':'操作记录已满；在 Mac 管理历史后再操作'}
            self.store.result(oid,'running','操作进行中')
        self.busy+=1
        try:
            name=action['action'];target=action['target'];value=action['value']
            if name in {'card_pin','card_clear'}:self.store.card_action(name,target,value)
            elif name=='task_viewed':self.store.task_viewed(target)
            elif name=='operation_read':
                result=self.store.operation_lookup(target)
                if not result:raise HubError('operation unavailable')
                return self.store.result(oid,'succeeded',result['state'],original=result)
            elif name=='usage_refresh':
                if self.refresh_usage:await self.refresh_usage()
                else:raise HubError('quota collector unavailable')
            elif name in {'hub_sync','control_refresh'}:await self.refresh_control()
            else:
                if not self.native:raise HubError('Mac native helper unavailable')
                args={}
                if name in {'audio_select','bluetooth_connect','bluetooth_disconnect','wifi_select'}:
                    if target not in self.targets:raise HubError('registered target unavailable; refresh first')
                    kind,native_id=self.targets[target]
                    if kind!={'audio_select':'audio','bluetooth_connect':'bluetooth','bluetooth_disconnect':'bluetooth','wifi_select':'wifi'}[name]:raise HubError('target kind mismatch')
                    args['target']=native_id
                elif name=='settings_open':args['target']=target
                elif name=='file_share':
                    path=self.store.data['files'].get(target)
                    if not path or not Path(path).is_file():raise HubError('File moved or deleted; select it again on Mac')
                    args['path']=path
                elif name=='details_open':
                    url=self.store.data['details'].get(target)
                    if not url:raise HubError('No registered task details')
                    args['url']=url
                if value is not None:args['value']=value
                response=await self.native.call(name,**args)
                if name in {'volume_set','mute_set','audio_select','bluetooth_connect','bluetooth_disconnect','wifi_select'}:
                    # Failed or unconfirmed operations may still change the
                    # live device list (e.g. a headset disconnects mid-write).
                    await self.refresh_control()
                if response.get('state') not in {'succeeded','unconfirmed'}:raise HubError(response.get('reason','Mac action failed'))
                return self.store.result(oid,response['state'],response.get('reason','操作成功'))
            return self.store.result(oid,'succeeded','操作成功')
        except (HubError,OSError,asyncio.TimeoutError) as e:
            return self.store.result(oid,'unconfirmed' if isinstance(e,asyncio.TimeoutError) else 'failed',str(e).encode()[:128].decode('utf-8','ignore'))
        except Exception:
            return self.store.result(oid,'unconfirmed','Mac 操作未能读取完整结果；刷新实际状态')
        finally:self.busy-=1
