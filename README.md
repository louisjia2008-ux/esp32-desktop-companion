# ESP32 Desktop Companion

一个放在桌面上的 ESP32-S3 触摸小屏：显示动态壁纸和时钟，查看本地 CO₂、温湿度与人体存在状态，也能连接 Mac 查看系统状态、任务进度并操作部分桌面功能。

工程包含 **ESP32 固件、macOS Bridge、本地网页管理界面和离线测试**。默认固件源码为 **v0.9.2**，使用 Arduino + PlatformIO，界面为 `480 × 320` 横屏。

> **实机状态（2026-10-20）：** 设备已刷入 v0.9.2，LD2410C 人体存在雷达和 SCD41 CO₂ 传感器都已接入并正常运行；一米内感应亮屏和空气曲线页在设备上使用正常（作者实机确认）。演示视频尚未录制，运行日志没有归档到仓库；七天历史的长期写入和断电恢复尚未专门验收，供电与电气测量仍未完成。详见 [验证范围](docs/VALIDATION.md)。

## 功能

| 功能 | 运行条件 | 行为 |
| --- | --- | --- |
| 时钟、日期、亮度 | 时钟先由 Bridge 校时 | 昼夜 / 闲置亮度，断开 Bridge 后时钟继续运行 |
| 动态壁纸 | 首次导入需要 Mac；离线播放需要 microSD | 浏览器选择本地视频，Mac 转码后传到 SD，设备自行循环播放 |
| 实时环境 | SCD41 | CO₂、温度、湿度；过期或缺失数据明确显示不可用 |
| 感应亮屏 | LD2410C | 一米内的新鲜有效目标维持亮屏，五分钟无近距离目标后调暗 |
| CO₂ 提醒 | 双传感器 | 有人且连续五分钟达到 1500 ppm 时本地提示，有回落阈值和提醒间隔 |
| 环境历史 | SCD41、microSD、已经校时 | 逐分钟汇总，最多七天曲线；安装后逐渐积累，断档不补造读数 |
| Mac 状态 | Bridge 在线 | CPU、内存、运行时间；区分断线和过期 |
| Mac 控制 | Bridge + 原生助手 | 音量、输出设备、已配对蓝牙、已保存 Wi-Fi、AirDrop / 分享面板 |
| 信息卡与任务 | Bridge | 临时文字 / 链接 / 文件卡、二维码，以及主动登记的终端任务 |
| AI 状态与用量 | 对应本地工具及可选集成 | Codex / Claude Code 进度；Codex 用量只读采集，Claude 用量保留适配入口 |
| 无线升级 | 首次 USB 安装后、已配对 Bridge | 屏幕物理长按确认，等待新固件健康启动，异常启动支持回滚 |

主页和工具页共用壁纸背景；在空白区域左右滑动导航。默认顺序为：首页 → Mac 系统 → 本地环境 → Mac 控制 → 信息卡 → 任务 → AI 用量。传感器、SD 本地播放和已校时的时钟可在 Bridge 离线时继续工作；Mac 控制、任务更新和 AI 采集依赖 Mac。

## 硬件与配件清单

以下是项目实际使用的硬件和复现所需配件。没有公开购物订单、成交价格、商店链接或地址。

| 配件 | 数量 | 用途 / 选型要求 |
| --- | ---: | --- |
| ESP32-S3 触摸显示开发板 | 1 | 实物 PCB 丝印 **SC3248W535**，N16R8：16 MB Flash、8 MB octal PSRAM |
| AXS15231B QSPI 触摸屏 | 板载 1 套 | 原生 `320 × 480`；横屏 `480 × 320`，触摸地址 `0x3B` |
| HLK-LD2410C 人体存在雷达模块 | 1 | 读取运动 / 静止存在、距离、能量，用于感应亮屏 |
| Sensirion SCD41 CO₂ 模块 | 1 | CO₂、温度、相对湿度 |
| microSD / TF 卡 | 1 | 保存动态壁纸和环境历史，需适配所用板卡 |
| USB 数据 / 供电线 | 1 | 供电、首次安装及串口调试，需要支持数据传输 |
| 模块连接线 / 适配线 | 按接口准备 | 按实际板卡和模块核对接口、间距与电平 |

详细 GPIO 表、信号接线和供电限制见 **[硬件说明](docs/HARDWARE.md)**。`jc3248w535` 是构建环境名称，不能仅凭名称认定所有 SC / JC 板或同外观商品都兼容。

**供电限制：** 主板 3.3 V 稳压器给传感器供电的余量、模块电源针脚与独立电气测量尚未完成确认。先按实际模块资料核对供电、电平与极性，再断电接线；逻辑 GPIO 表不替代电源接线资料。

## 软件准备

- macOS：运行 Bridge；Mac 控制还需要 Xcode Command Line Tools / Swift。
- Python 3.10 或更新版本、PlatformIO。
- `ffmpeg` / `ffprobe`：导入视频使用。
- ESP32 和 Mac 位于可信、可互通的局域网；ESP32 使用 2.4 GHz Wi-Fi。
- 普通 C / C++ 编译器：运行主机侧离线测试时使用。

```sh
git clone https://github.com/louisjia2008-ux/esp32-desktop-companion.git
cd esp32-desktop-companion
python3 scripts/install_privacy_guard.py
python3 -m venv .venv
.venv/bin/python -m pip install platformio
.venv/bin/python -m pip install -r mac_bridge/requirements.txt
brew install ffmpeg
```

最后一行是 Homebrew 安装方式，也可自行提供 `ffmpeg` / `ffprobe`。

隐私检查安装器为当前克隆安装 `pre-commit` / `pre-push` hooks，并下载校验固定版本的 Gitleaks。**每个新克隆都需要执行安装命令**；发现密钥、Wi-Fi / 配对凭据、运行日志或其他受保护内容时，本地检查会阻止提交或推送，检查工具缺失时也会阻止。安装步骤、离线安装、规则和限制见 [自动隐私检查](docs/PRIVACY_GUARD.md)。

## 构建与首次安装

```sh
# 默认双传感器日常服务
.venv/bin/pio run -e deskpet_sensors
# 不接传感器时使用的基础显示 / 桌面 Hub 版本
.venv/bin/pio run -e jc3248w535
```

`platformio.ini` 固定了 ESP32 平台与核心库版本。诊断环境 `ld2410c_rx_probe`、`scd41_probe`、`dual_sensor_probe` 用于逐项排查接线，正式服务和诊断探针不可同时拥有传感器总线。

首次给新板安装，先核对板型、引脚和内存，再枚举实际端口：

```sh
.venv/bin/pio device list
# 把占位端口换成上一步列出的实际端口。
.venv/bin/pio run -e deskpet_sensors --target upload --upload-port /dev/cu.usbmodemXXXX
```

已有固件、配网和重要数据的设备，先保存备份并检查实际分区；不要照抄其他设备的应用地址或整片擦除。首次安装可用 USB；之后推荐按 [OTA 说明](docs/OTA.md) 使用配对和物理确认流程。

## 配网与启动 Bridge

1. 在仓库根目录构建本机助手并启动 Bridge：

   ```sh
   scripts/build_native_helper.sh
   .venv/bin/python mac_bridge/bridge.py run
   ```

2. 设备首次启动进入 Setup Mode。连接屏幕上显示的临时 `DevCompanion-XXXX` 热点，随机热点密码也显示在设备上。
3. 打开 `http://192.168.4.1`，选择设备能看到的 **2.4 GHz Wi-Fi**，填写自己的密码、Mac 的局域网 IPv4 和端口 `8765`。
4. 在 Mac 打开 `http://127.0.0.1:8766/`，按提示完成配对，然后导入壁纸、调整显示设置或添加信息卡。

Wi-Fi 凭据保存在设备 NVS，配对 / 壁纸 / 任务数据在本机运行目录生成。网页管理端固定在 loopback，WebSocket 用于可信局域网通信。不要将管理端口映射到公网。

屏幕连接点：灰色为 Wi-Fi 未连接，黄色为 Wi-Fi 已连接但 Bridge 未连接，绿色为 Bridge 已连接。BOOT 长按 1.5–5 秒后松开可打开设置；长按五秒会清除保存的网络配置。

## 壁纸与环境页

网页选择自己的 MP4 等视频后，Bridge 使用 `ffmpeg` 转码。源文件上限为 **800 MiB / 10 分钟**，支持 6 / 8 / 10 / 12 FPS，默认 10 FPS。SD 播放包上限为 128 MiB，每帧 JPEG 最多 14 KiB。较长视频的传输和逐帧验证需要等待；确认“SD 卡本地播放”后可断开 Bridge 继续播放。仓库没有附带个人视频或 SD 内容。

环境页包含实时、曲线和雷达视图。传感器缺失、数据过期、尚未校时或 SD 不可用分别有明确状态，未知读数不当作零。感应亮屏和 CO₂ 提醒可以独立开关并保存。详见 [传感器日常使用](docs/SENSORS_DAILY_USE.md)。

## 离线测试

测试使用模拟硬件与协议输入，不替代真机和电气验证。

```sh
PYTHONPATH=mac_bridge .venv/bin/python -m unittest discover -s mac_bridge/tests -v
.venv/bin/python -m unittest discover -s tests/network -v
.venv/bin/python -m unittest discover -s tests/sensors -p 'test_*.py' -v
.venv/bin/python scripts/run_sensor_tests.py --sanitize
.venv/bin/python scripts/run_sensor_service_tests.py --sanitize
.venv/bin/python scripts/run_sensor_ui_tests.py
.venv/bin/python scripts/run_air_history_tests.py --sanitize
.venv/bin/python scripts/run_runtime_log_tests.py
# 需要已构建默认环境，使用其固定 WebSockets 2.6.1 源码
.venv/bin/python scripts/run_network_recovery_tests.py --sanitize
```

其他 UI / Hub / SD / OTA 测试位于 `scripts/run_*_tests.py`；部分从基础环境 `.pio/libdeps/jc3248w535/` 读取 ArduinoJson，需先构建基础环境。此次公开副本的检查结果见 [验证状态](docs/VALIDATION.md)。

## 文档与目录

- [硬件 / 配件 / GPIO](docs/HARDWARE.md)、[传感器与历史曲线](docs/SENSORS_DAILY_USE.md)
- [桌面 Hub 与 Mac 控制](docs/DESKTOP_HUB.md)、[时钟与亮度](docs/AMBIENT_DISPLAY.md)
- [SD 壁纸协议](docs/SD_WALLPAPER.md)、[事件卡片](docs/EVENT_SYSTEM.md)、[OTA 和回滚](docs/OTA.md)
- [Codex 进度](docs/CODEX_PROGRESS.md)、[Claude Code 集成](docs/CLAUDE_CODE.md)
- [隐私与公开范围](docs/PRIVACY.md)、[自动隐私检查](docs/PRIVACY_GUARD.md)
- [验证状态](docs/VALIDATION.md)、[第三方声明](THIRD_PARTY_NOTICES.md)

```text
include/       板级 GPIO、版本和触摸接口
src/           固件：显示、UI、网络、传感器、存储、协议
mac_bridge/    Python Bridge、本地网页与 Swift 原生助手
scripts/       构建、离线回归、可选设备诊断与集成安装
tests/         主机侧测试与硬件替身
tools/         字体生成工具、字体与原始许可证
docs/          通用使用、硬件和协议说明
```

这是个人硬件项目，公开的是可阅读、可构建的源码快照。第三方代码与字体的原始许可证随文件保留；项目自有代码尚未另外指定许可证。
