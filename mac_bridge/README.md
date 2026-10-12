# macOS Bridge

Bridge 提供局域网设备 WebSocket、本地壁纸管理网页、视频转码、聚合系统状态和桌面 Hub。可选 Swift 助手实现 Mac 音频、蓝牙、已保存 Wi-Fi、文件选择和分享面板；可选 hooks 提供 Codex / Claude Code 进度。

从仓库根目录运行：

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r mac_bridge/requirements.txt
scripts/build_native_helper.sh
.venv/bin/python mac_bridge/bridge.py run
```

视频导入需要 `ffmpeg` 和 `ffprobe`；Mac 控制需要 Swift 工具链和对应系统授权。助手由本机脚本构建并 ad hoc 签名，仓库不包含分发签名或公证的 App。

- 设备 WebSocket 默认监听 `0.0.0.0:8765`，用于可信局域网。
- 管理页面固定在 `http://127.0.0.1:8766/`，仅本机访问。
- 运行数据默认在忽略的 `mac_bridge/.runtime/` 内；可用 `--library-root` 指定自己的壁纸库。不要同时启动两个占用同一端口的 Bridge。
- 真实网络、配对、蓝牙设备、任务、用量和个人壁纸均在运行时生成，不随源码公开。

```sh
.venv/bin/python mac_bridge/bridge.py --help
.venv/bin/python mac_bridge/bridge.py pair
.venv/bin/python mac_bridge/bridge.py ota .pio/build/deskpet_sensors/firmware.bin
```

OTA 需在设备屏幕物理长按确认，收到健康启动结果后才报告安装完成。Codex 用量采集只读，不创建模型任务；Claude 用量适配在本快照中仍保留入口，缺失时显示不可用。

详细步骤见 [项目 README](../README.md)、[桌面 Hub](../docs/DESKTOP_HUB.md)、[Codex 进度](../docs/CODEX_PROGRESS.md)、[Claude Code](../docs/CLAUDE_CODE.md) 和 [OTA](../docs/OTA.md)。
