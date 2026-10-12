#!/usr/bin/env python3
"""Read-only stdio probe. Never print account identities, tokens or raw responses."""
import asyncio,json,shutil
async def main():
 proc=await asyncio.create_subprocess_exec(shutil.which("codex") or "/opt/homebrew/bin/codex","app-server","--stdio",stdin=asyncio.subprocess.PIPE,stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.DEVNULL,limit=1048576)
 async def rpc(mid,method,params=None):
  req={"id":mid,"method":method}
  if params is not None:req["params"]=params
  proc.stdin.write((json.dumps(req)+"\n").encode());await proc.stdin.drain()
  for _ in range(200):
   line=await asyncio.wait_for(proc.stdout.readline(),20)
   if not line:raise RuntimeError("server_closed")
   msg=json.loads(line)
   if msg.get("id")==mid:
    if "error" in msg:raise RuntimeError("rpc_error")
    return msg.get("result",{})
  raise RuntimeError("response_limit")
 try:
  await rpc(1,"initialize",{"clientInfo":{"name":"esp32-hub-probe","version":"0.8.0"}})
  proc.stdin.write(b'{"method":"initialized"}\n');await proc.stdin.drain()
  account=await rpc(2,"account/read",{"refreshToken":False})
  limits=await rpc(3,"account/rateLimits/read")
  buckets=limits.get("rateLimitsByLimitId") or {"legacy":limits.get("rateLimits") or {}}
  windows=[]
  for lid,bucket in buckets.items():
   for position in ("primary","secondary"):
    w=bucket.get(position)
    if isinstance(w,dict):windows.append({"limit_id":lid,"position":position,"window_minutes":w.get("windowDurationMins"),"used_percent":w.get("usedPercent"),"resets_at":w.get("resetsAt")})
  print(json.dumps({"initialize":"passed","account_type":(account.get("account") or {}).get("type"),"rate_limits_read":"passed","windows":windows}))
 except Exception as e:print(json.dumps({"probe":"failed","reason":type(e).__name__+":"+str(e)[:40]}))
 finally:
  if proc.returncode is None:proc.terminate()
  try:await asyncio.wait_for(proc.wait(),3)
  except asyncio.TimeoutError:proc.kill();await proc.wait()
asyncio.run(main())
