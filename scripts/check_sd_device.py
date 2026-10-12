#!/usr/bin/env python3
"""Bounded live SD playback soak; sends notifications, never physical actions."""
import argparse
import json
from pathlib import Path
import time
from urllib.request import Request, urlopen


def status():
    with urlopen('http://127.0.0.1:8766/api/status', timeout=5) as response:
        return json.load(response)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--duration', type=float, default=320)
    parser.add_argument('--output', type=Path, default=Path('output/sd-wallpaper/stress.json'))
    args = parser.parse_args()
    initial = status()
    if initial['playback_mode'] != 'sd_local' or initial['wallpaper_state'] != 'playing':
        raise RuntimeError('Wait for confirmed SD playback before starting soak')
    started = time.monotonic()
    sent = []
    snapshots = []
    prefix = f'sd-soak-{time.time_ns():x}'
    while time.monotonic() - started < args.duration:
        elapsed = time.monotonic() - started
        if len(sent) < 20 and elapsed >= len(sent) * 6:
            event_id = f'{prefix}-{len(sent):02d}'
            event = {'v':1, 'type':'notification', 'event_id':event_id,
                     'level':'success' if len(sent) % 2 else 'info', 'source':'sd-test',
                     'title':'Local playback check', 'message':'Wallpaper resumes from SD after this card.',
                     'auto_dismiss_ms':1000}
            request = Request('http://127.0.0.1:8766/api/events',
                              data=json.dumps(event).encode(), method='POST',
                              headers={'Content-Type':'application/json'})
            with urlopen(request,timeout=5) as response:
                result=json.load(response)
            if result['delivered_clients'] != 1: raise RuntimeError('Physical client not ready')
            sent.append(event_id)
        current = status()
        if current['ready_clients'] != 1 or current['wallpaper_state'] != 'playing':
            raise RuntimeError('SD playback or Bridge connection changed during soak')
        snapshots.append({'elapsed':round(elapsed,2), 'wallpaper_state':current['wallpaper_state'],
                          'streamed_frame_packets':current['streamed_frame_packets']})
        if current['streamed_frame_packets'] != initial['streamed_frame_packets']:
            raise RuntimeError('Unexpected continuous video frame transmission')
        time.sleep(min(5,max(0,args.duration-(time.monotonic()-started))))
    result={'seconds':round(time.monotonic()-started,2),'notification_count':len(sent),
            'event_ids':sent,'status_samples':snapshots,
            'streamed_frame_packets_delta':status()['streamed_frame_packets']-initial['streamed_frame_packets'],
            'physical_actions':0,'result':'PASS'}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({key:value for key,value in result.items() if key not in {'event_ids','status_samples'}}))


if __name__ == '__main__': main()
