# 自动隐私检查

项目使用本地 Git hooks、Gitleaks 和项目隐私规则，在提交及推送时检查公开内容。GitHub 的内置 push protection 和 Actions 工作流提供额外检查。自动扫描不能识别所有未知格式的密钥或个人信息，公开更新仍应先查看暂存差异。

## 每个克隆安装一次

在仓库根目录运行，要求 Python 3.10 或更新版本和 Git：

```sh
python3 scripts/install_privacy_guard.py
```

安装器支持 macOS / Linux 的 arm64 和 x64，默认从 Gitleaks 官方 GitHub Release 下载 **8.30.1**，按照安装器内固定的 SHA256 校验对应平台归档，再提取并检查版本。工具和下载归档放在当前仓库的 Git common directory 中的 `privacy-tools/`；不安装到系统目录，也不提交到仓库。

安装器为当前克隆配置 `core.hooksPath=.githooks`。已有其他自定义 hooks 路径或默认目录中的可执行 hooks 时，安装器会停止并保留现有配置；先将 `.githooks/pre-commit` 和 `.githooks/pre-push` 的调用整合到现有 hooks，再使用。不要直接覆盖其他检查。

Git 不会随 `clone` 自动启用仓库中的 hooks，**每个新克隆都需要执行上面的安装命令**。切换电脑或另外克隆仓库也需要重新安装。

## 离线安装与手动检查

预先下载 Gitleaks 官方 8.30.1 对应系统和架构的归档。以下为 macOS arm64 示例，其他平台替换为对应归档的实际路径：

```sh
python3 scripts/install_privacy_guard.py --offline --gitleaks-archive ./gitleaks_8.30.1_darwin_arm64.tar.gz
```

该方式仍会验证固定 SHA256。已有验证过的归档缓存时，可以只运行 `python3 scripts/install_privacy_guard.py --offline`。`--gitleaks-bin <audited-executable>` 是显式信任已有本地可执行文件的选项，只检查其版本，无法单凭可执行文件确认来源；仅在已独立核实该文件时使用。

可以随时手动运行与 hooks 相同的检查：

```sh
# 扫描整个 Git 暂存区，包括部分暂存文件的实际暂存版本
python3 scripts/privacy_guard.py --staged
# 扫描本地所有引用和 HEAD 的完整可达历史
python3 scripts/privacy_guard.py --history
```

`pre-commit` 扫描实际 Git index，不以工作目录当前内容替代暂存内容。`pre-push` 读取 Git 提供的本次推送引用，检查每个非删除目标提交及其全部可达祖先；新分支、新标签、强制推送和后续已删除的文件也在检查范围内。仅删除引用不公开新对象。不能解析为完整提交历史的引用会被拒绝。

历史检查直接读取 Git 文件对象以及提交和附注标签的元数据，再交给项目规则和 Gitleaks；不依赖 Git diff 的展示结果，避免 `.gitattributes` 把文本标成二进制后漏检。

## 检查规则

通用扫描由固定版本 Gitleaks 提供，项目规则补充以下内容：

- **凭据内容：** API key、访问令牌、私钥、Wi-Fi 密码与 SSID、配对 key / code、密码赋值、编译参数中的凭据、硬编码配网调用，以及带凭据的 URL。
- **个人与设备信息：** 个人绝对路径、实际私网地址、设备 MAC、固定串口号、个人邮箱、手机号和已知运行记录格式。设备固定 Setup AP 地址、全零 MAC、GitHub noreply 提交邮箱和明确占位符按规则处理。
- **私有目录与文件：** `.runtime/`、`output/`、`evidence/`、备份 / 恢复目录、工具登录状态目录、密钥 / 本地配置文件、日志和会话文件、Flash / NVS 镜像、个人媒体和生成产物。这些路径即使使用 `git add -f` 也会被检查。
- **未审查内容：** 不在明确允许范围内的二进制、过大的未审查文本、不支持的 Git 文件类型，以及未经审查的 Gitleaks 覆盖配置。Git 历史检查包含可达文件版本和提交 / 标签元数据。

项目规则以 [扫描器](../scripts/privacy_guard.py) 和 [策略文件](../.privacy-policy.json) 为准。缺少 Gitleaks、版本不匹配、策略缺失 / 无效、浅克隆或无法读取完整历史、工具超时或其他扫描错误均会阻止操作；不会在无法完成检查时继续提交或推送。

## 测试数据和字体例外

仓库保留少量此前审查过的固定合成测试输入。策略只按 **文件路径 + 规则 ID + 匹配值的 SHA256** 精确允许这些输入，不对整个测试目录或某类密钥一概放行。替换合成值、移动文件或增加真实数据，都需要重新检查。不要为了通过扫描而给真实凭据添加例外。

只有两份已有字体使用明确的二进制允许项：

- `tools/fonts/InterDisplay-SemiBold.ttf`
- `tools/fonts/NotoSansSC.ttf`

两份字体都要求与策略中的固定 SHA256 完全一致；修改内容或添加其他二进制不会自动获得允许。更新字体和测试例外时，应独立核查来源与内容，并在审查中说明策略变更原因。仓库不接受通过 `.gitleaksignore`、`.gitleaks.toml` 或行内允许标记跳过通用扫描。

## 被阻止时如何处理

失败输出只包含脱敏的诊断元数据，例如文件路径、行号、规则 ID 和 Git 对象标识；不打印匹配值、代码片段或外部扫描器的原始输出。扫描临时文件在本机临时目录中处理并清理，不作为 Actions artifact 上传。

检查出问题后，先从暂存区或待推送历史中移除真实隐私内容，再重新检查。仅在后续提交删除文件，不能消除早期提交中的泄露；推送前历史检查仍会阻止这种情况。真实凭据一旦到达公开远端，应先撤销或轮换，再处理公开历史和相关副本；不要将凭据粘贴到 Issue、日志或求助消息中。

工具缺失时重新运行安装命令。浅克隆需要先取得完整历史，例如 `git fetch --unshallow`，再重新检查。自定义 hooks 冲突需要整合检查，不能通过覆盖或跳过来解决。

## GitHub 上的检查与限制

仓库启用了 GitHub 内置 secret scanning / push protection，用于在 GitHub 支持的凭据类型命中时提供额外阻止。它不等同于项目的 Wi-Fi、配对信息和运行日志规则。

[Privacy guard 工作流](../.github/workflows/privacy.yml) 在 push、pull request 和手动触发时取得完整历史，安装校验过的固定扫描器，运行项目历史检查和隐私回归测试。测试使用临时本地仓库验证实际提交 / 推送拦截。工作流仅有读取仓库内容的权限，不请求项目 secrets，不上传扫描报告或输入文件。

**Actions 在上传之后才运行，不能撤回已经公开的内容。** 上传前的项目阻止依赖当前克隆已启用的本地 hooks。`--no-verify`、修改 hooks 或扫描策略、网页编辑和 API 上传都可能绕过本地检查；GitHub 内置保护也不能覆盖所有未知密钥、编码形式或个人信息。保持 hooks 开启，并在发布前查看暂存差异和检查结果。

本页描述检查行为和使用方法；实际验证结果以对应提交的检查记录和 GitHub 工作流结果为准。
