# Codex 工作进度 / Codex progress

本机 Codex 生命周期 hooks 将状态发送到 Mac Bridge。Bridge 将其与现有
Claude Code 状态合并，经已配对的 WebSocket 发送给 ESP32 `0.7.3`。
设备仍使用 `480x320` 横向壁纸和底部单行透明浮层。适配器不调用模型。

## 安装和官方信任审核

在项目根目录预览配置，再按需安装：

```sh
.venv/bin/python scripts/install_codex_hooks.py
.venv/bin/python scripts/install_codex_hooks.py --apply
```

安装目标为 `~/.codex/hooks.json`。安装器保留其他 hooks，仅替换本适配器的
handler；配置已有内容时先建立权限 `0600` 的时间戳备份，再原子写入。
重复执行不会重复安装。`config.toml` 和已有 `notify` 设置不由安装器修改。
官方 CLI 的信任审核会保存 `hooks.state` 等运行状态；本次审核后确认所有
既有设置（包括 `notify`）仍然保留。
脚本使用本项目的绝对路径，移动项目后应重新生成配置并审核。

在 Codex CLI 输入 `/hooks`，检查新增的八条定义。它们都应只执行
`python3 '/absolute/project/path/mac_bridge/codex_hook.py'`，超时为一秒。
审核并信任这些定义后才能运行；已有其他 hooks 时应逐项检查。
这是 [Codex 官方的 hook 信任要求](https://learn.chatgpt.com/docs/hooks#review-and-trust-hooks)。

这里覆盖本机 Codex 与本机 Work 执行路径；云端编排不运行这些用户级命令
hooks。运行中的桌面聊天是否已经加载配置，需要以实际事件确认，不能由
配置文件存在推断。新开或恢复本机聊天后，应提交一轮真实工作验收。
支持范围参见 [官方 Hooks 文档](https://learn.chatgpt.com/docs/hooks)。
请用本机真实事件确认 hooks 是否加载；屏幕像素和触摸仍需单独验收。公开快照的验证范围见 [验证状态](VALIDATION.md)。

Mac Bridge 按既有方式运行。`DEVCOMPANION_BRIDGE_URL` 可覆盖默认本机
`http://127.0.0.1:8766`，只允许 HTTP loopback origin，拒绝代理转发、远程
主机和带凭据的网址。发送超时为 `300 ms`，Bridge 不在线时正常退出。

## 事件与数据边界

| Event | Display effect |
| --- | --- |
| `SessionStart` | Register session; resume/compact preserves the active turn timer |
| `UserPromptSubmit` | Start a new turn; duplicate events preserve running calls |
| `PreToolUse` | Track the individual active call and current operation |
| `PostToolUse` | Retire only that call; no remaining call means thinking |
| `PermissionRequest` | Waiting for confirmation in Codex |
| `Stop` | Finished turn, eligible for display for four seconds |
| `Interrupt` | Stopped turn, eligible for display for four seconds |
| `SessionEnd` | Remove the session immediately |

`POST /api/codex/status` 仅监听本机，沿用 JSON、Host/Origin 和 `8 KiB`
请求限制。字段只允许 `v`、`event`、`session`、`project`、`stamp_ns` 及
对应事件的 `origin`、`turn`、`tool`、`call`、`fingerprint`。

会话 ID 哈希截取八位；回合、调用 ID 和审批关联指纹截取十六位。
`PermissionRequest` 没有调用 ID，适配器用工具名和输入的哈希指纹与
Pre/Post 关联，剔除只用于描述审批原因的 `description`。原始输入不会
发送给 Bridge；该指纹也不会出现在设备的显示协议里。
提示词、命令文本、工具结果、完整路径和凭据均不进入状态 payload。
项目只取目录名，发送到设备的项目和工具字符串各限制为 `32 UTF-8 bytes`。

跨进程排序使用 `clock_gettime_ns(CLOCK_MONOTONIC)`。macOS Python 3.9 的
`time.monotonic()` 起点属于单个进程，不能用来比较不同 hook 进程。
Bridge 的回合耗时使用其自身单调时钟。晚到的已完成调用和已关闭回合事件
不会重启回合；并行工具分别跟踪。Codex 最多缓存三十二个会话，以保留
未入选四个显示槽位的回合计时。活动调用和终止记录也有数量上限，
并沿用 Bridge 六小时的会话缓存期限。静默不会变成“已完成”。

## 显示与兼容

设备的 `hello` 增加 `agent_status_v2` capability。只有完成既有配对验证的
v2 设备才接收合并状态，每个 session 增加 `source: claude | codex`。
旧设备继续接收原来的 Claude 状态包，字段和 Claude 审批 broker 不变。

浮层显示来源、项目、操作、耗时和 `1/1` 等任务序号。工具 `Bash` 显示
“执行命令”，`apply_patch` 显示“修改文件”，没有活动工具时显示“思考中”。
长中文项目先截断项目名，保留来源和操作。最多显示四条；两种来源都有
活动任务时各占至少一条，其余先选等待任务，再按最近活动选择。

设备每四秒轮播；刷新不重置轮播相位。新出现的等待任务立即显示。
弹窗、升级浮层和离开壁纸页面时暂停轮播，返回后继续剩余时间。
Bridge 超过三十秒没有状态更新时清除浮层。结束/中断的四秒期限使用
状态年龄计算，收到 `SessionEnd` 时可以更早撤除。

Codex 的确认和回答仍在 App/CLI 完成。设备不显示无法验证的完成百分比，
也不将 Codex 请求转交 Claude 的审批流程。

## 排查与验证

`GET /api/status` 增加：

- `codex.sessions / received_events / ignored_events / paired_clients`
- `agents.visible_sessions / paired_clients`
- `devices[].agent_status_v2`

任务数表示当前选中的显示槽位，不是历史会话总数。设备版本、配对、
OTA 结果和壁纸状态仍可通过同一接口读取。

```sh
PYTHONPATH=mac_bridge .venv/bin/python -m unittest discover -s mac_bridge/tests -v
.venv/bin/python scripts/run_event_tests.py
.venv/bin/python scripts/run_touch_tests.py
.venv/bin/python scripts/run_glass_tests.py
.venv/bin/pio run
```

Host tests cover bounded metadata, parallel tools, waiting, late events,
turn closure, resume/compact, cross-process timestamps, mixed-source selection,
protocol compatibility, UTF-8 fitting, rotation and modal pause. Actual App
hooks, physical display, touch and OTA boot identity require runtime evidence.
See [the implementation acceptance record](VALIDATION.md).

## 升级和回滚

通过既有 [OTA 流程](OTA.md) 安装，设备仍要求长按确认。
Bridge 现在发送 `1 KiB` 数据块，设备保持原来的 `12 KiB` 接收上限。
最终成功以配对设备回报的版本、准确镜像身份与启动健康状态判断。

回退前请自行保存并确认旧固件镜像，再按 OTA 流程选择准确文件。私有的配网、配对材料、固件备份和原始运行快照没有随仓库发布；不要把这些内容加入公开报告。
