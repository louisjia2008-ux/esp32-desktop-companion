# Claude Code integration — hook behaviour (phase 0 findings)

Measured on 2026-10-01 with Claude Code 2.1.286 (`claude-haiku-4-5`),
using a logging hook registered for every event through `--settings`.
Headless runs used `claude -p`; interactive runs drove the real TUI
through a pseudo-terminal. These results define the integration design;
re-check them after a major Claude Code upgrade.

## Events and their input fields

| Event | Fired | Fields used by the Bridge |
| --- | --- | --- |
| `SessionStart` | session start | `session_id`, `cwd`, `source` |
| `UserPromptSubmit` | prompt sent | `session_id`, `cwd` (never forward `prompt`) |
| `PreToolUse` | before every tool | `session_id`, `tool_name`, `tool_use_id` |
| `PermissionRequest` | only when a dialog would be shown | `session_id`, `tool_name`, `tool_input`, `permission_suggestions` (no `tool_use_id`) |
| `PostToolUse` | after a tool ran | `session_id`, `tool_name`, `tool_use_id`, `duration_ms` |
| `Stop` | turn finished | `session_id`, `last_assistant_message` (never forward) |
| `SessionEnd` | session end | `session_id`, `reason` |

## Permission decisions

A `PermissionRequest` hook answers with:

```json
{"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{"behavior":"allow"}}}
{"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{"behavior":"deny","message":"Denied on ESP32 device"}}}
```

- `allow` runs the tool (verified: the Bash command created its file).
- `deny` blocks it; Claude receives `message` as the reason.
- Exiting 0 with no output leaves the normal flow untouched: the terminal
  dialog decides (headless mode treats it as a denial).

## AskUserQuestion

- The tool does not exist in headless `-p` mode; interactive sessions only.
- It triggers `PermissionRequest` with `tool_name: "AskUserQuestion"` and
  `tool_input.questions[]` (`question`, `header`, `options[].label`,
  `options[].description`, `multiSelect`).
- Answering works by allowing with an updated input:

  ```json
  {"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{
    "behavior":"allow",
    "updatedInput":{"questions":[...unchanged...],
                    "answers":{"Which color do you prefer?":"Blue"}}}}}
  ```

  Verified: the hook chose the second option and Claude replied `PICKED=Blue`.
  Answers are keyed by the exact question text; values are option labels.

## Concurrency with the terminal

- While a `PermissionRequest` hook is still running, the terminal already
  shows the normal dialog. Both can answer; the first answer wins.
- Answering in the terminal does **not** terminate the hook. In the test the
  tool ran 0.6 s after the terminal answer, the hook kept sleeping and exited
  4 s later, and its late result was ignored.

Consequences for the design:

1. The hook must not assume it is the only decision path. The Bridge treats
   a pending device request as resolved elsewhere as soon as the same
   session produces any later event (`PostToolUse`, `PreToolUse` of another
   call, `Stop`, `UserPromptSubmit`, `SessionEnd`). It then sends `dismiss`
   to the ESP32 and answers the waiting hook with "cancelled", and the hook
   exits without output.
2. A hook must never print a decision after it was cancelled, and it keeps a
   bounded overall timeout so it cannot outlive the request.
3. There is no need to block the terminal: the ESP32 is an additional,
   faster path, not a replacement for the dialog.

## Reproducing

The experiment used a hook that appends each stdin JSON to a log and, for
`PermissionRequest`, prints a decision chosen by `HOOK_DECISION`
(`allow` / `deny` / `none`), optionally after `HOOK_SLEEP` seconds. Run it
with `claude --settings <file> --setting-sources ""` in a scratch directory
so personal permission rules cannot approve tools on their own.

# Using the companion (phase 1: Mac side)

## Setup

```sh
.venv/bin/python mac_bridge/bridge.py pair          # prints the pairing code
.venv/bin/python mac_bridge/bridge.py claude-hooks  # prints a settings fragment
```

Merge the printed `hooks` object into `~/.claude/settings.json` yourself; the
Bridge never edits Claude Code settings. The hook uses only the Python
standard library. `DEVCOMPANION_BRIDGE_URL` (default `http://127.0.0.1:8766`)
and `DEVCOMPANION_DECISION_TIMEOUT` (default 120 s) can be set in the
environment Claude Code runs in.

Pair the device once (firmware 0.6.0 or later):

1. Copy the code without displaying it:
   `.venv/bin/python mac_bridge/bridge.py pair | tail -1 | tr -d '\n' | pbcopy`
2. Hold BOOT for 1.5–5 s and release. The screen shows SETUP MODE with a
   fresh hotspot password (forget the network on the phone if it remembered
   an older password).
3. Join the hotspot, open `http://192.168.4.1`, paste the code into
   **Bridge pairing code** (case, spaces and dashes do not matter; leave it
   empty later to keep it) and save. The device restarts.
4. The Bridge log shows `AUTH_OK claude_companion=true` and `/api/status`
   reports `claude.paired_clients: 1`.

The code is stored in the device NVS (`pair_key`, 16 bytes) and in
`mac_bridge/.runtime/pairing.key` (0600); only an HMAC of a fresh nonce
crosses the LAN. Holding BOOT for 5 s erases it with the other settings.
Without a paired device every request returns `no_device` within
milliseconds and the terminal dialog is the only path, exactly as without
the hook.

## Data flow and privacy

| Hook event | Sent to the Bridge |
| --- | --- |
| status events | event name, 8-hex hash of the session id, `cwd` basename, tool name, 16-hex fingerprint of the tool input |
| `PermissionRequest` (tools) | tool name and a short summary: first line of a Bash command, file basename for file tools, host for WebFetch, query for WebSearch, nothing for other tools |
| `PermissionRequest` (AskUserQuestion) | question headers, question texts and option labels |

Prompts, assistant messages, tool output, file contents and full paths are
never sent. The device receives at most 32-byte project/tool names, a
128-byte summary and 48-byte labels. Claude requests are kept only in Bridge
memory (never the persistent event ledger).

## Loopback API (127.0.0.1 only, JSON, same-origin checks)

| Method and path | Purpose |
| --- | --- |
| `POST /api/claude/status` | session state update; may dismiss cards answered elsewhere |
| `POST /api/claude/requests` | open a device card: `pending` + `request_id`, or `no_device` / `unsupported` |
| `GET /api/claude/requests/<cc-12hex>` | poll: `pending`, `answered` (`behavior` or `answers` indexes), `cancelled`, `expired`, `unknown` |

## Device protocol additions

- `hello` capabilities `claude_companion` (implies `events_v2` and
  `agent_status`).
- Pairing: Bridge sends `{"v":1,"type":"auth_challenge","nonce":"<32hex>"}`;
  the device answers `{"v":1,"type":"auth_response","mac":"<64hex>"}` with
  `HMAC-SHA256(key = 16 bytes decoded from the pairing code, message = the
  32 ASCII nonce characters)`. Only a paired connection receives Claude
  cards and `agent_status`, and only it may answer `cc-*` requests.
  The LAN link is unencrypted: pairing stops forged answers from other
  devices but not an attacker able to hijack the TCP session.
- Cards are ordinary `action_required` packets with `request_id`
  `cc-<12hex>-<question index>`. Permission cards offer `allow` (with
  `hold_ms: 800`) and `deny`; question cards offer `o0`–`o3`. Multi-question
  requests show one card per question; the next card follows the ACK.
- `agent_status`, at most every 0.5 s on change and every 5 s otherwise:

  ```json
  {"v":1,"type":"agent_status","sessions":[{"id":"a1b2c3d4","project":"eps32",
   "state":"working","tool":"Edit","elapsed_s":42,"age_s":3,"pending":0}]}
  ```

  States are `waiting` (a card is pending), `working`, and `done` (kept for
  10 s after `Stop`; `elapsed_s` is then the turn duration). At most four
  sessions, most urgent first.

## Rules

- A card closes when the device answers, when the same session produces
  `PostToolUse` with the same tool-input fingerprint, `Stop`,
  `UserPromptSubmit` or `SessionEnd` (answered in the terminal), or when the
  hook stops polling for 3 s.
- Multi-select questions and more than eight pending requests stay in the
  terminal (`unsupported`).

## End-to-end check (loopback only)

Verified on 2026-10-01 against a real interactive Claude Code session with an
isolated Bridge on ports 18765/18766 and the simulator as the paired device:

1. Bash permission: the card showed the command summary, the simulator chose
   `allow`, the command ran.
2. Two Chinese questions in one AskUserQuestion: two cards in order, the
   simulator chose option 2 twice, Claude replied `PICKED=蓝色,狗`.
3. Terminal answered first: the card was dismissed and the hook exited.
4. No paired device or no Bridge: the hook returned in 0.07 s with no output.

On the real device (2026-10-01, firmware 0.6.0): after pairing, the HUD
showed `eps32  Bash` with a running timer over the playing wallpaper, the
permission card rendered in Chinese with a hold-to-confirm 允许, and holding
it made the real hook print `{"decision":{"behavior":"allow"}}`.

```sh
.venv/bin/python mac_bridge/bridge.py run --host 127.0.0.1 --port 18765 \
    --editor-port 18766 --library-root /tmp/cc-test/wallpapers --no-cli
.venv/bin/python mac_bridge/simulate_device.py --once-claude \
    --uri ws://127.0.0.1:18765 --pairing-key-file /tmp/cc-test/pairing.key --choose allow
```
