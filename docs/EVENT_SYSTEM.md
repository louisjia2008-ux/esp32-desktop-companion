# Generic interactive events — protocol v1

This phase adds Mac-initiated events and device choices to the existing Bridge,
Wi-Fi, display, touch sampler, video wallpaper and CPU/RAM page. The physical
panel is 320×480; the working product remains 480×320 landscape. The new card
layout is calculated from the display geometry and is also host-tested at
320×480 portrait. This does not rotate or replace the existing drivers.

## Base view and Overlay

The local base view remains `WALLPAPER` or `MAC_STATUS`. Remote events occupy
an independent Overlay. Removing the last event restores the current base
view; the status page's existing 15-second timeout continues while obscured.
An event cannot open a status dashboard by itself.

Overlay priority is ACTION_REQUIRED > ERROR > WARNING > SUCCESS > INFO.
An active action request, including Sending/Unconfirmed/Rejected, cannot be
overwritten by another event. Higher priority events can preempt notifications;
preempted notifications return to the queue. Notifications do not acknowledge
actions. Legacy `status` is converted into an event with an internal ID.

During any Overlay, JPEG decoding pauses and the last wallpaper is dimmed.
The network keeps only the newest compressed video frame for resumption.
Modal rendering is capped at 10 FPS; event/choice changes redraw immediately.
The original 60 ms base rendering cadence is unchanged. The measured hardware
FPS and touch delay must be taken from the actual serial log, not these caps.

## Mac → ESP32

All text packets are bounded to 2048 bytes with integer `"v":1`. Unknown
types, event levels, action fields, IDs, versions, invalid lengths, fragmented
frames, embedded nulls, malformed/trailing JSON and more than three actions
are rejected. Event text uses the existing built-in bitmap font; full CJK/emoji
font rendering is not part of this phase. Identifiers use ASCII letters,
digits and `_.:-`, with a maximum of 48 bytes (32 for action IDs).

```json
{"v":1,"type":"notification","event_id":"evt-001","level":"success","source":"demo","title":"Build Complete","message":"The task finished successfully.","auto_dismiss_ms":4000}
```

Levels: `info`, `success`, `warning`, `error`. `source` is required (32 bytes),
title is bounded to 48 bytes, message to 128 bytes. Received time is recorded
on ESP32. Omitted auto-dismiss defaults to 4000 ms for INFO/SUCCESS and zero
for WARNING/ERROR. Explicit zero means manual Dismiss; nonzero durations must
be 1000–600000 ms. All notifications have a visible Dismiss button. Optional
ERROR Details UI is deferred; the supplied message is wrapped/truncated.

```json
{"v":1,"type":"action_required","request_id":"demo-001","source":"demo","title":"Choose a plan","message":"How should the task continue?","actions":[{"id":"minimal","label":"Minimal fix"},{"id":"refactor","label":"Refactor"},{"id":"cancel","label":"Cancel"}]}
```

One, two or three unique actions are supported. Firmware that advertises
`events_v2` also accepts four actions and an optional per-action
`"hold_ms"` (300–2000) that requires a press-and-hold to confirm; the Bridge
never sends such packets to other firmware. Action labels are bounded to
48 bytes. Action IDs have no executable meaning in this project.

```json
{"v":1,"type":"action_ack","request_id":"demo-001","action_id":"refactor","accepted":true}
```

Only an ACK matching both selected IDs can confirm an action. `accepted=true`
closes that request and advances the queue/restores the base view.
`accepted=false` displays rejection, unlocks the original choices, and adds
Dismiss. ACKs for other/completed requests are ignored.

```json
{"v":1,"type":"dismiss","request_id":"demo-001"}
```

Existing explicit cancellation remains available. Its identifier can target
a queued/active request or notification ID. Cancellation records `dismissed`,
never `accepted`. An empty/mismatched ID cannot cancel a local Mac status page.

Existing `video_wallpaper`, binary DWP1 frames and `host_status` are unchanged.
Their latest-only receive slots do not displace events or ACKs.

## ESP32 → Mac

```json
{"v":1,"type":"action","request_id":"demo-001","action_id":"refactor"}
```

The action schema permits exactly these four fields. A debounced physical
gesture emits once and locks every action button while waiting for ACK. Held
fingers, old buffered input, movement off a button, I2C errors and queue-gap
resynchronization cannot dispatch another choice. A complete sampled short
tap also works when rendering delayed its processing.

If the Bridge is already offline at selection, the chosen ID is retained and
the UI displays Connection lost / Action not confirmed; no action is emitted.
A send failure, disconnect while awaiting ACK, or 4000 ms ACK timeout enters
Unconfirmed with Retry and Dismiss. Retry uses exactly the original pair and
locks again. A late matching ACK may confirm that same choice.

`hello` adds `interactive_events` to existing capabilities. Firmware is 0.4.0.
Other device messages remain `heartbeat` and `wallpaper_status`. No new
command, URL, process, file, Accessibility, shell, Git or AI executor exists.

## Bounded storage

- ESP32 pending queue: eight events, plus one current Overlay. Equal priority
  events use arrival order. On overflow, evict the oldest event at the lowest
  strictly lower priority. If no lower priority victim exists, reject the new
  event and log it. Active requests are never overwritten. Every buffer is
  fixed-size; queue and history do not allocate per event.
- ESP32 recent IDs: 64 entries, distinguishing event IDs from request IDs;
  active/queued IDs are checked separately even after recent-cache rotation.
- ESP32 history: ten RAM entries, recording completion/dismissal/rejection
  and selected action IDs. Events do not write NVS or Flash.
- WebSocket receive controls: eight entries with ACK/cancellation preference
  and lower-priority eviction; video and telemetry stay latest-only.
- Mac ledger: at most 64 validated requests. Completed records are evicted
  first; 64 unresolved records apply explicit backpressure. Recent notification
  IDs and processed IDs are also bounded to 64. The request/accepted-choice
  ledger is atomically saved on the Mac with mode 0600 so restart retries stay
  idempotent. Dedup is bounded, not indefinite retention.

Mac choices are checked against offered IDs. Unknown requests/choices receive
accepted=false. An already accepted matching choice receives the original
positive ACK without processing again; a different choice is rejected. ACK
is sent only to the originating socket, never broadcast to other devices.
Accepting only records/logs the selection in this phase.

## Existing Bridge demo and local event API

```sh
.venv/bin/python mac_bridge/bridge.py demo
```

Menu: 1 INFO, 2 SUCCESS, 3 WARNING, 4 ERROR, 5 ACTION_REQUIRED, 6 Quit.
`burst` sends INFO/SUCCESS/ACTION_REQUIRED/ERROR, `d` cancels the last demo
request, `c` prints client counts. The first menu action uses `demo-001`;
subsequent requests use fresh IDs. Never reuse a completed ID to request a
second physical interaction.

With the Bridge already running:

```sh
.venv/bin/python mac_bridge/bridge.py send-demo --event action_required --request-id demo-001
.venv/bin/python mac_bridge/bridge.py send-demo --event success
```

`POST http://127.0.0.1:8766/api/events` accepts the notification, request and
explicit dismiss schemas above with `Content-Type: application/json`. Browser
requests must be same-origin; the endpoint reuses the loopback wallpaper HTTP
service. A delivered client count confirms socket send, not visible pixels or
physical acceptance. `GET /api/status` includes bounded choice counters.

## Evidence and real-device acceptance

Host tests compile the actual UI, event engine and ArduinoJson parser with
AddressSanitizer/UndefinedBehaviorSanitizer, and test both display dimensions:

```sh
.venv/bin/python scripts/run_event_tests.py
.venv/bin/python scripts/run_touch_tests.py
.venv/bin/python scripts/run_wifi_tests.py
PYTHONPATH=mac_bridge .venv/bin/python -m unittest discover -s mac_bridge/tests -v
```

The delivery/queue stress helper uses this existing Bridge and never synthesizes
a touch or action packet. Capture serial concurrently:

```sh
.venv/bin/python scripts/capture_boot.py /dev/cu.usbmodemXXXX --no-reset --seconds 600
.venv/bin/python scripts/run_event_device_checks.py --duration 320
```

Physical checks still required:

1. Verify wallpaper pixels/motion; tap to open CPU/RAM and tap back.
2. Send INFO, touch Dismiss, verify the prior view. Send SUCCESS and observe
   automatic dismissal; inspect WARNING and ERROR and touch their controls.
3. Send `demo-001`, touch Refactor. Confirm device `ACTION_SELECTED` and
   `ACTION_SENT`, Mac `ACTION RECEIVED request_id=demo-001 action_id=refactor`,
   `ACTION_ACK_SENT accepted=True`, device `ACTION_ACK accepted=1` and
   `UI_RESTORED base=WALLPAPER` or `MAC_STATUS` (subject to its timeout).
4. Display a fresh request, stop Bridge, touch a choice. Verify Unconfirmed,
   restart the same Bridge/ledger, touch Retry, and verify the same IDs/ACK.
5. Over at least five minutes, send at least 20 events, make at least ten
   distinct physical action choices, and stop/restart Bridge twice. Observe
   reboot/uptime, watchdog logs, heap/PSRAM, queue, sampling gaps and choice
   counters. Zero action packets before any physical choice is not proof of
   physical duplicate protection.

Serial evidence lives under ignored `output/event-system/`. Counters and a
successful simulator roundtrip are not substitutes for physical touch or
visual UI acceptance. The baseline source archive, original firmware binary,
hash manifest and parent Git index copy are retained there for provenance.
