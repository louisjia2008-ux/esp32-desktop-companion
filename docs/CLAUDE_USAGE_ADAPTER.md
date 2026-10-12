# Claude Code 后续用量采集器接入

本次没有实现或查询 Claude 额度。设备与 Mac 网页已经预留 Claude 的五小时/周窗口，公共数据模型、页面、八档提醒和持久化去重可复用。原有 Claude 会话进度 hooks 不提供额度百分比，不能用会话次数或时长推算额度。

## 本机 API

所有 API 仍在 loopback `http://127.0.0.1:8766`；沿用 Host、Origin、Sec-Fetch-Site 校验及 16 KiB 请求体上限。采集器要禁用代理，不能新增公开的局域网写入口。

| 接口 | GET | POST |
|---|---|---|
| `/api/hub/cards` | 当前卡片 | 普通创建；`operation: pin/clear/file_pick` |
| `/api/hub/tasks` | 普通任务与现有 AI 会话 | 带序号的任务更新；`operation: viewed` |
| `/api/hub/usage` | 公共用量记录 | source/account_scope/records 原子快照 |
| `/api/hub/actions` | 列表，或 `?operation_id=` 查结果 | 固定动作和已登记 target |
| `/api/hub/control` | 原生控制状态与已登记设备 | 只读；刷新通过动作接口 |

## 用量 DTO

未来采集器提交：

```json
{
  "source": "claude-code",
  "account_scope": "collector-local-opaque-account-id",
  "records": [
    {
      "provider": "claude",
      "limit_id": "claude",
      "window_minutes": 300,
      "used_percent": 40,
      "resets_at": 1800003000,
      "observed_at": 1800000000,
      "state": "fresh"
    },
    {
      "provider": "claude",
      "limit_id": "claude",
      "window_minutes": 10080,
      "used_percent": null,
      "resets_at": null,
      "observed_at": 1800000000,
      "state": "not_provided"
    }
  ]
}
```

这是结构示例，时间和百分比是固定示例值，不能当作真实当前额度发送。`observed_at` 必须是来源实际读取时间的 Unix 秒；允许与 Mac 当前时间差最多一天。相同 source 应提交当前完整快照，来源中缺失的旧窗口随该快照移除；其他 source 的记录保留。提交旧观察时间不能覆盖更新数据。

每条记录严格包含以上七个字段：

- provider：Claude 使用 `claude`，不要写 `claude-code`。
- limit_id：来源真实桶标识，最多 48 UTF-8 字节，多个桶可共存。
- window_minutes：来源实际窗口长度，1–525600；未知可为 null，不能按字段位置猜测五小时/周。
- used_percent：**已用**百分比 0–100，可为 null；屏幕用 `100 − used_percent` 算剩余。若来源给的是剩余比例，先正确转换。
- resets_at：实际重置 Unix 秒；未知为 null。
- observed_at：有效有限时间值；不使用设备时间代替来源采集时间。
- state：`fresh`、`stale`、`error`、`not_provided`。fresh 必须具备有效窗口、百分比、重置时间。超过十分钟的 fresh 自动降为 stale。

source 和 provider 使用 1–32 位字母、数字、下划线或连字符。一次最多 16 条。account_scope 仅用于本机账户切换和提醒去重，Mac 持久化的是散列，不向 ESP32 返回。不要传入邮箱、token、完整账户资料或其他账户秘密。来源尚未提供的窗口填 null，失败保留 error；不能把失败转换成已用 100%。

公共快照中的 observed_at=0 Claude 预留项由 Bridge 生成，用于界面“尚未读取”。采集器不能照搬 0 提交；有真实相同 provider/window 的记录后预留项自动让位。不要篡改 `hub-state.json`，统一走 API。

## 最小提交代码

```python
import json
from urllib.request import Request, build_opener, ProxyHandler

def publish_usage(snapshot):
    # snapshot 来自实际采集器；这里不进行登录、额度读取或数值推断。
    request = Request(
        "http://127.0.0.1:8766/api/hub/usage",
        data=json.dumps(snapshot).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with build_opener(ProxyHandler({})).open(request, timeout=3) as response:
        return json.load(response)
```

Bridge 负责阈值计算、跨档合并、账户/窗口切换、重启去重及恢复通知。后续 Claude 采集器只负责合法、只读、有来源的额度读取及提交，不自行重复弹出提醒。新增授权、认证或采集方式需要按真实 Claude 接口确认，不能把本文件当作开放任意读取用户文件或对外发送数据的授权。

## 任务 DTO

主动任务更新必需 id、source、title、state、seq、updated_at，可选 summary、details_url。id/source 是不透明引用，title 最大 48 字节，summary 最大 128 字节，seq 为非负递增整数，时间为 Unix 秒。状态 `running/waiting_input/success/failed/stale`；终态来自真实退出结果，等待来自真实输入请求或主动声明。已存在任务的来源不能更换，旧序号不能覆盖新状态。详情 URL 仅在 Mac 登记，设备只拿 has_details 和任务引用。

固定动作包括 volume_set、mute_set、audio_select、bluetooth_connect/disconnect、wifi_select、airdrop_open、file_share、card_pin/clear、task_viewed、details_open、usage_refresh、control_refresh、hub_sync、operation_read、settings_open。value/target 均按动作校验；不要新增通用 exec 动作或接受设备传入路径。
