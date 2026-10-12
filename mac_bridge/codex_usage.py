"""Read-only Codex App Server client. No turns, login, reset or model requests."""
from __future__ import annotations
import asyncio
import hashlib
import json
import math
import shutil
import time
from desktop_hub import HubError

READ_METHODS={'initialize','account/read','account/rateLimits/read'}
SOURCE='codex-app-server'

def normalize_limits(payload,observed_at):
    if not isinstance(payload,dict):raise HubError('invalid rate limits response')
    buckets=payload.get('rateLimitsByLimitId')
    if not isinstance(buckets,dict) or not buckets:
        legacy=payload.get('rateLimits')
        buckets={legacy.get('limitId') or 'codex':legacy} if isinstance(legacy,dict) else {}
    records=[]
    for key,bucket in buckets.items():
        if not isinstance(bucket,dict):continue
        lid=bucket.get('limitId') or key
        for position in ('primary','secondary'):
            window=bucket.get(position)
            if not isinstance(window,dict):continue
            minutes=window.get('windowDurationMins');used=window.get('usedPercent');reset=window.get('resetsAt')
            minutes=minutes if type(minutes) is int and 1<=minutes<=525600 else None
            used=used if type(used) in (int,float) and math.isfinite(used) and 0<=used<=100 else None
            reset=reset if type(reset) is int and 0<reset<=0xffffffff else None
            records.append({'provider':'codex','limit_id':str(lid)[:48],'window_minutes':minutes,
                            'used_percent':used,'resets_at':reset,'observed_at':observed_at,
                            'state':'fresh' if None not in (minutes,used,reset) else 'not_provided'})
    if not records:records=[{'provider':'codex','limit_id':'codex','window_minutes':None,'used_percent':None,
                            'resets_at':None,'observed_at':observed_at,'state':'not_provided'}]
    return records

class CodexUsageCollector:
    def __init__(self,store,active=lambda:False,clock=time.time,command=None):
        self.store=store;self.active=active;self.clock=clock
        self.command=command or [shutil.which('codex') or '/opt/homebrew/bin/codex','app-server','--stdio']
        self.proc=None;self.reader=None;self.task=None;self.pending={};self.counter=0
        self.wake=asyncio.Event();self.lock=asyncio.Lock();self.last_refresh=0.;self.next_reset_poll=0.
        self.state='not_provided';self.last_error='';self.account_scope=''
        self.refresh_count=0;self.notification_count=0;self.page_active_until=0.

    async def _send(self,payload):
        self.proc.stdin.write((json.dumps(payload,separators=(',',':'))+'\n').encode());await self.proc.stdin.drain()

    async def _read(self):
        try:
            while True:
                raw=await self.proc.stdout.readline()
                if not raw:raise HubError('app_server_closed')
                message=json.loads(raw)
                if 'id' in message and 'method' not in message:
                    future=self.pending.get(message['id'])
                    if future and not future.done():
                        if 'error' in message:future.set_exception(HubError('app_server_rpc_failed'))
                        else:future.set_result(message.get('result',{}))
                elif message.get('method') in {'account/rateLimits/updated','account/updated'}:
                    self.notification_count+=1;self.wake.set()
                elif 'id' in message and 'method' in message:
                    # External token requests cannot extend this read-only client.
                    await self._send({'id':message['id'],'error':{'code':-32601,'message':'Read-only client'}})
        except asyncio.CancelledError:raise
        except Exception:
            for future in tuple(self.pending.values()):
                if not future.done():future.set_exception(HubError('app_server_reader_closed'))

    async def rpc(self,method,params=None):
        if method not in READ_METHODS:raise HubError('read-only method required')
        self.counter+=1;mid=self.counter;future=asyncio.get_running_loop().create_future();self.pending[mid]=future
        try:
            request={'method':method,'id':mid}
            if params is not None:request['params']=params
            await self._send(request)
            return await asyncio.wait_for(future,20)
        finally:self.pending.pop(mid,None)

    async def _connect(self):
        if self.proc and self.proc.returncode is None and self.reader and not self.reader.done():return
        await self._close_process()
        self.proc=await asyncio.create_subprocess_exec(*self.command,stdin=asyncio.subprocess.PIPE,
                    stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.DEVNULL,limit=1048576)
        self.reader=asyncio.create_task(self._read(),name='codex-usage-reader')
        await self.rpc('initialize',{'clientInfo':{'name':'esp32-desktop-hub','title':'ESP32 quota monitor','version':'0.8.0'}})
        await self._send({'method':'initialized'})

    async def refresh(self):
        async with self.lock:
            stage='connect'
            try:
                await self._connect()
                stage='account_read'
                account=(await self.rpc('account/read',{'refreshToken':False})).get('account')
                # Hash only identity fields. Never serialize identity into snapshots/logs.
                if not isinstance(account,dict):raise HubError('codex_account_unavailable')
                identity={k:account.get(k) for k in ('type','id','email','chatgptAccountId')}
                self.account_scope=hashlib.sha256(json.dumps(identity,sort_keys=True).encode()).hexdigest()
                stage='rate_limits_read'
                limits=await self.rpc('account/rateLimits/read')
                stage='normalize_and_store'
                records=normalize_limits(limits,int(self.clock()))
                self.store.usage_submit({'source':SOURCE,'account_scope':self.account_scope,'records':records})
                self.state='fresh';self.last_error='';self.refresh_count+=1
                return {'state':'fresh','records':len(records)}
            except (OSError,HubError,ValueError,asyncio.TimeoutError) as error:
                self.state='error';self.last_error=f'Codex collector {stage}: {type(error).__name__}'+(' '+str(error) if isinstance(error,HubError) else '')
                self.store.usage_error(SOURCE);await self._close_process()
                raise HubError(self.last_error)
            finally:self.last_refresh=self.clock()

    def usage_page(self):
        self.page_active_until=self.clock()+75;self.wake.set()

    async def start(self):
        if not self.task:self.task=asyncio.create_task(self.run(),name='codex-usage-collector')

    async def run(self):
        while True:
            self.wake.clear()
            try:await self.refresh()
            except HubError:pass
            self.wake.clear() # absorb notifications caused by this read
            # Refresh interval is reevaluated every second as activity changes.
            while not self.wake.is_set():
                now=self.clock();interval=60 if self.active() or now<self.page_active_until else 300
                resets=[r['resets_at'] for r in self.store.data['usage'] if r.get('_source')==SOURCE and r.get('resets_at')]
                deadline=any(self.last_refresh<reset<=now for reset in resets)
                if now-self.last_refresh>=interval or (deadline and now>=self.next_reset_poll):
                    if deadline:self.next_reset_poll=now+60
                    break
                try:await asyncio.wait_for(self.wake.wait(),1)
                except asyncio.TimeoutError:pass

    async def _close_process(self):
        if self.reader:
            self.reader.cancel();await asyncio.gather(self.reader,return_exceptions=True);self.reader=None
        if self.proc and self.proc.returncode is None:
            self.proc.terminate()
            try:await asyncio.wait_for(self.proc.wait(),3)
            except asyncio.TimeoutError:self.proc.kill();await self.proc.wait()
        self.proc=None

    async def close(self):
        if self.task:self.task.cancel();await asyncio.gather(self.task,return_exceptions=True);self.task=None
        await self._close_process()
