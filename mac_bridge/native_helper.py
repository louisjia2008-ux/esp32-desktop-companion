"""Long-lived native AppKit helper, with a bounded NDJSON response channel."""
import asyncio,json
from pathlib import Path
from desktop_hub import HubError
class NativeHelper:
    def __init__(self,binary=None):
        self.binary=Path(binary) if binary else Path(__file__).parent/'native/build/DesktopHubHelper.app/Contents/MacOS/DesktopHubHelper'
        self.proc=None;self.lock=asyncio.Lock();self.counter=0
    async def call(self,action,_timeout=None,**args):
        async with self.lock:
            if not self.binary.is_file():raise HubError('Build native helper with swift build in mac_bridge/native')
            if not self.proc or self.proc.returncode is not None:
                self.proc=await asyncio.create_subprocess_exec(str(self.binary),stdin=asyncio.subprocess.PIPE,stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.DEVNULL,limit=65536)
            self.counter+=1;request={'id':self.counter,'action':action,**args}
            self.proc.stdin.write((json.dumps(request)+'\n').encode());await self.proc.stdin.drain()
            try:
                while True:
                    raw=await asyncio.wait_for(self.proc.stdout.readline(),_timeout or (300 if action=='file_pick' else 35))
                    if not raw:raise HubError('Mac native helper closed')
                    result=json.loads(raw)
                    if result.get('id')==self.counter:return result
            except (asyncio.TimeoutError,ValueError):
                await self.close();raise asyncio.TimeoutError('Mac result not confirmed; refresh actual state')
    async def close(self):
        if self.proc and self.proc.returncode is None:
            self.proc.terminate()
            try:await asyncio.wait_for(self.proc.wait(),3)
            except asyncio.TimeoutError:self.proc.kill();await self.proc.wait()
        self.proc=None
