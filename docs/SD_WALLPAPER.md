# SD 卡本地动态壁纸

电脑负责选视频、转换和更新；ESP32 从 SD 卡读取 JPEG 动画包并循环播放。
正常播放不依赖电脑持续发送视频帧。CPU/RAM 和事件继续使用现有 WebSocket。

源视频上限为 **800 MB（按 800 MiB，即 838,860,800 字节计算）、10 分钟**。
6/8/10/12 FPS 均支持完整 10 分钟；需要 0.8.1 或更新固件。

## 硬件与存储

- ESP32-S3 / AXS15231B，保留现有 480×320 横屏显示与触摸方向。
- 厂家 DEMO_PIC 的 SD_MMC 单线引脚：CLK=12、CMD=11、D0=13。
- 使用 20 MHz、1-bit 模式；不使用与 QSPI 屏幕重叠的额外数据线。
- `format_if_mount_failed=false`，不会因挂载失败而格式化。
- 真机已识别容量 1,004,535,808 bytes；挂载前文件系统总量
  983,957,504 bytes，已用 26,775,552 bytes。
- 仅操作 `/companion-wallpaper/` 中自己的固定文件，不接收网络文件路径。

引脚来源：[厂家示例镜像](https://raw.githubusercontent.com/NorthernMan54/JC3248W535EN/main/JC3248W535EN/1-Demo/Demo_Arduino/DEMO_PIC/pincfg.h)。
API 使用项目固定 Arduino-ESP32 2.0.17 的
[SD_MMC](https://github.com/espressif/arduino-esp32/blob/2.0.17/libraries/SD_MMC/src/SD_MMC.h)。

## 更新与恢复

1. 复用 Mac 壁纸库和 ffmpeg，生成原生 320×480 JPEG 帧。
2. Mac 缓存一个 DWC1 文件，最大 128 MiB，最多 7200 帧（12 FPS × 600 秒）；其他帧率按 600 秒限制，每帧最大 14 KiB。
3. 通过现有 WebSocket 上传到 SD 的 `staging.part`，每块最多 12 KiB。
4. 每块检查 transfer ID、顺序偏移和 CRC，写入后 ACK；同一块重试不重复写入。
5. 收齐后读回文件，每个主循环校验一帧，继续服务触摸、事件和当前壁纸。
6. 检查头部 CRC、全部帧边界、JPEG 标记、总长度、数据 CRC 和完整文件 CRC。
7. 新壁纸首帧成功解码后，写入并读回新活动记录，再清理旧包。

较长壁纸在传输后仍需逐帧校验，期间继续显示旧壁纸，可能需要数分钟。
Bridge 校验等待按帧数增加，最低 180 秒、每帧预算 0.25 秒并加 60 秒余量，
最大 7200 帧对应 1860 秒；断线或另选壁纸仍会取消。播放包格式与缓冲大小保持不变。

使用两个数据槽和两个带 generation/CRC 的活动记录。更新过程中，当前槽保持
有效；上传中断、校验失败或记录写入失败保留旧壁纸。重启时选择有效的最新记录，
重新校验文件后播放。仅清理本模块的未完成临时文件；不扫描删除其他 SD 文件。
记录不是每帧写入，SD 在播放期间只读，也不逐事件写 NVS。

重连时，匹配 wallpaper ID、文件大小与 CRC 的已有缓存返回 `cached`，无需重新
上传。重新选择已损坏或不同的壁纸会上传新包。新包无法提交时，编辑器显示保存
失败；Mac 中选中的目标和 ESP32 上仍在播放的旧缓存可能暂时不同。

## v1 消息

Mac → ESP32：

```json
{"v":1,"type":"wallpaper_sync","op":"begin","transfer_id":"0123456789abcdef","wallpaper_id":"fedcba9876543210","bytes":2257895,"crc32":123456789}
```

`op` 为 `begin`、`commit`、`abort` 或 `clear`。只有 begin 接收 bytes/crc32。
所有 ID 必须为 16 位小写十六进制，未知字段、未知操作及越界长度拒绝。
clear 必须匹配当前缓存 ID。

ESP32 → Mac：

```json
{"v":1,"type":"wallpaper_sync_ack","transfer_id":"0123456789abcdef","wallpaper_id":"fedcba9876543210","state":"progress","offset":12288,"bytes":2257895,"reason":"","sd_capacity_bytes":1004535808}
```

ACK 状态：ready / progress / verifying / complete / cached / error / aborted / cleared。
complete 表示 SD 读回校验、首帧解码和活动记录提交已完成；正常播放仍另外报告
`wallpaper_status: playing`。选择界面收到 HTTP 成功不等于设备已经完成保存。

上传使用 stop-and-wait；每块等待 ACK，超时最多重发同一块三次。取消选择或
断开连接会终止未完成上传。固件中未完成上传在 15 秒无活动后结束。
校验期间保留旧壁纸；事件 Overlay 继续暂停背景播放，新包切换等待 Overlay 结束。

## 二进制格式

DWC1 文件头 40 bytes，全部整数为 little-endian：

| Offset | 字段 |
|---|---|
| 0 | magic DWC1，4 bytes |
| 4 / 6 | uint16 version=1 / header_size=40 |
| 8 / 10 | uint16 width=320 / height=480 |
| 12 / 14 | uint16 FPS（6/8/10/12）/ reserved=0 |
| 16 / 20 | uint32 frame_count / total_file_bytes |
| 24 | uint64 wallpaper key |
| 32 | uint32 payload CRC32 |
| 36 | uint32 header CRC32，覆盖前 36 bytes |

随后顺序存储 `uint32 JPEG长度 + JPEG数据`，循环播放时 seek 回 offset 40。
不展开整个视频到 RAM，复用现有 JPEGDEC 与屏幕帧缓冲。

DWS1 上传块头 20 bytes：magic（4）、transfer key（uint64）、offset（uint32）、
chunk CRC32（uint32），随后最多 12 KiB 文件数据。
CRC 使用 CRC-32/ISO-HDLC，与 Python zlib.crc32 一致。

## 兼容与界面

- SD 可用时 hello 增加 `sd_wallpaper`，固件版本 0.5.0。
- SD 不可用时不声明该能力，继续兼容现有 DWP1 推流；不假装已保存。
- 正常模式保留动态壁纸；事件 Overlay、Mac Status 的超时和页面恢复逻辑保留。
- 已配置网络但暂时断网时，已验证的 SD 壁纸继续播放；BOOT 清除网络配置后仍
  显示原有配置界面。
- 删除活动 Mac 壁纸时向在线 SD 客户端发送 targeted clear；离线设备在重连时
  根据其实际播放 ID 清理。原电脑上的视频源文件不受影响。

## 验证命令

```sh
.venv/bin/python scripts/run_sd_tests.py
.venv/bin/python scripts/run_event_tests.py
.venv/bin/python scripts/run_touch_tests.py
.venv/bin/python scripts/run_wifi_tests.py
PYTHONPATH=mac_bridge .venv/bin/python -m unittest discover -s mac_bridge/tests -v
PLATFORMIO_SETTING_ENABLE_TELEMETRY=no .venv/bin/pio run
.venv/bin/pio run --target upload --upload-port /dev/cu.usbmodemXXXX
```

运行证据保存到 `output/sd-wallpaper/`（不提交日志、壁纸内容或运行记录）。
物理手指操作和屏幕像素检查仍需人工；串口 decode/render/SD 计数与浏览器状态
只能确认对应软件路径，不能替代实际看屏幕。
