# 给组员的交接说明

更新：2026-11-27。先看本文，再看 [未完成问题清单](OPEN_ISSUES.md)。
R2 包已补齐 27 次固定输入 Quality 复测、独立审计、更新图表与演讲附录，以及 1–7 演示预设和 F2 状态显示。旧版包保留。
这是本地工作快照，不代表整个小组项目已完成，也没有自动发给任何人。

## 发哪个包

- `StarTraveler-team-full-2026-11-27-r2.zip`：完整交接包，包含源码、可运行程序、
  所需项目资产和上采样运行 DLL、报告、PPTX、讲稿、精选结果和新版备份视频。
  要接手演示或希望一次拿齐资料的组员用这个包。
- `StarTraveler-team-source-2026-11-27-r2.zip`：轻量源码与结果包，包含源码、资产、
  文档、PPTX、精选结果和未完成清单，不含编译好的程序、供应商运行 DLL和视频。
  只需要审阅、合并代码的组员可以先拿这个包。
- 压缩包旁有 `SHA256SUMS.txt` 和打包校验记录。两个包各自解压到独立文件夹即可，
  不需要先装到原作者的目录。请勿把整个压缩包覆盖到组员现有开发仓库。

下载后可用 PowerShell 的 `Get-FileHash 压缩包.zip -Algorithm SHA256` 对照
`SHA256SUMS.txt`。解压后，安装 Python 的组员还可以运行 `python VERIFY_PACKAGE.py`
核对包内每个文件；这只是传输完整性校验，不是重新运行全部科学评测。

## 首先看什么

1. [未完成问题和优先级](OPEN_ISSUES.md)：明确哪些结论可以说、哪些不能说。
2. [降噪报告](docs/final-denoiser-report.md)：当前固定曝光实验的正式结论。
3. [图表、R2 可编辑 PPTX（12 页主讲 + 2 页附录）和讲稿](docs/final-materials/README.md)。
4. [新版窗口彩排与备份视频说明](docs/final-window-rehearsal.md)。
5. [原始项目任务](TODO.md)：包含其他成员负责、当前仍未完成的项目目标。

源代码入口是 `StarTraveler/src/`（游戏和窗口彩排）、`StarCore/src/gpu/`
（渲染器、降噪及 DLSS）、`StarCore/examples/render.cpp`（离屏命令行）、
`StarTraveler/tools/eval/`（评测、画图、录制和校验）。

## 直接运行完整包

需要 Windows x64；这组 DLSS/RR 对照需要支持 Vulkan ray query 和 DLSS-RR 的
NVIDIA RTX 显卡与相应驱动。程序还依赖 Visual C++ x64 运行库。包里没有携带
Windows 系统 DLL、NVIDIA 驱动或 `_nvngx.dll`；这些由接收电脑提供。
如果缺少 `MSVCP140*.dll`、`VCRUNTIME140*.dll`，请安装/修复 Visual C++ x64
运行库，不要从不明网站单独下载 DLL。

完整解压后，双击 `01-Play.cmd` 可以手动操作。它选择普通 DLSS K / 固定 SR
曝光 1；普通交互模式保留游戏其他默认设置。若要手动对齐报告设置，还需在
Render 中选 Full、关闭自动曝光并设为 1、选 ACES、关闭后期效果与粒子等。
普通交互模式的时间步仍随实际帧间隔变化，不能当作正式评测复现。

双击 `02-Fixed-Exposure-Flight.cmd` 会自动打开游戏并运行七阶段彩排，约 140 秒
后自动结束。这条路线内部固定 SR/色调映射曝光 1、ACES、1 spp、seed 29 和
1000/60 ms 时间输入，关闭后期效果与粒子。RR 的 SR 专用曝光字段不生效，RR
仍使用原来的 unity 输入。彩排是自动飞行，不是手工键盘验证，也不是性能测试。

双击 `03-Open-Backup.cmd` 用系统播放器打开新版无声备份视频。轻量源码包中
这些程序和视频不在包内，需要完整包或自行构建。

请接收方检查实际启用的是 DLSS/DLSS-RR，不能把回退结果算成 RR。其他机器上的
显卡、驱动、性能和投影均未由本机验证。打包后的本机搬移测试见
`handoff-validation/`（完整包）；它也不能替代全新机器测试。

本机搬移测试已通过：游戏 `--help` 正常，普通 DLSS K 和 DLSS-RR 各渲染了
384×216 的 3 帧，实际方法正确且分别加载搬移目录里的 DLSS/RR DLL；SR 固定
曝光为 1，RR 的 SR 字段为 0，固定时间输入均正确。该小测试不评价最终画质、
不测性能、不代表全套 GPU 回归通过，也没有验证其他电脑的运行环境。

## 继续开发或重新构建

这个工作目录来自解压快照，没有 `.git`，因此交接包没有分支历史、提交记录或
可直接应用的 Git 补丁。请在小组真实 Git 仓库中新建分支，比较并合并改动，
不要直接替换其他成员已经修改的文件，也不要覆盖 main。

按 [构建说明](docs/building.md) 安装 C++ 工具链、CMake 和 vcpkg 后重新配置。
已包含 `StarCore/vcpkg.json` 及版本配置，不包含庞大的 vcpkg/编译缓存。首次
构建需要获取依赖，不能把附带的运行程序目录当作完整 CMake 构建树继续编译。
例如使用 Visual Studio 2022 时：

```powershell
cmake --preset msvc-vs2022
cmake --build build/msvc-vs2022 --config Release
```

必须先设置好 `VCPKG_ROOT`，并遵循构建文档中的其他要求。以上是交接构建入口，
不是“已在组员的新机器验证通过”的声明。

Python 评测依赖可在仓库内新建环境：

```powershell
uv venv .venv
uv pip install --python .venv/Scripts/python.exe -r StarTraveler/tools/eval/requirements.txt
# 需要重新录制视频时再加：
uv pip install --python .venv/Scripts/python.exe -r StarTraveler/tools/eval/requirements-video.txt
```

历史文档中的 `../.build-tools/eval-venv/Scripts/python.exe` 是原作者机器的路径。
在组员电脑上改用 `.venv/Scripts/python.exe`；`--exe`、输出目录等也应指向本机。
`build_final_deck.mjs` 依赖原作者的 Codex 演示运行环境，不能原样在任意电脑执行；
现成交付的 PPTX 可以直接在演示软件里编辑，绘图 Python 脚本则读取相对仓库路径。

## 数据范围与不可误用之处

包内保留了精选表格、曲线、审计 JSON、截图和新版视频，支持查看与讨论结果。
没有附带本机约 25 GB 的整个 `renders/`、逐帧 PNG、所有参考图、所有失败尝试、
旧视频和完整编译目录。精确收入的文件由 `PACKAGE-MANIFEST-*.json` 列出。

因此这不是完整的逐帧重算归档。历史校验记录中的绝对路径和哈希按原样保留，
部分文档链接指向未收入的大文件或诊断目录；不能将文件缺失误判成源实验失败，
也不能声称拿轻量交接包就重新核验过全部逐帧证据。需要重算或审计原始数据时，
再单独索取相应原始数据目录，或者按协议在新目录重新采集。

目前可用的正式新结论是：RR 在三个镜头更早进入各自稳定误差范围；Full+DLSS K
在 Anvil 和星体的最终误差更低，镜面最终值接近。各方法稳定范围不同，不能说
成统一画质目标下的延迟，也不能套用旧 Quality GPU 耗时。
新固定输入 27 次运动测量另外表明：RR 在三个镜头运动结束时 FLIP 更低，
但 GPU 耗时更高；它与静止后的最终误差是不同指标。见新报告和 R2 附录。完整问题见清单。

## 发送方式

将需要的 ZIP 和 `SHA256SUMS.txt` 发到小组文件区或网盘，再把本文和问题清单
一并转给组员。源码接手者可先拿轻量包，演讲接手者拿完整包。不要发送
`.build-tools/`、`.git` 凭据、个人配置、编译缓存或整个原始渲染目录。
本次只生成本地交接文件，没有上传、推送仓库或给任何人发送消息。
