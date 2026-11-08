# DM8168 Spatial Audio

[![Portable prototype checks](https://github.com/mambaout666/dm8168-spatial-audio/actions/workflows/ci.yml/badge.svg)](https://github.com/mambaout666/dm8168-spatial-audio/actions/workflows/ci.yml)

基于课程 EZSDK、RPE 和 Qt 路线的双耳空间音频实验项目。

**v0.1 是可运行的 CPU 原型与板端接入基础**：包含连续 Q15 双耳 FIR、方向切换、文件/FIFO 后端、独立 Qt 客户端、HRIR 导入工具和分段测试。真实 TI RPE 绑定及开发板上的听感、实时性和 30 分钟稳定性验证尚未完成。

项目从一个单声源生成左右耳信号：

```text
ALSA capture -> audio FIFO -> ARM backend -> CPU / real RPE plugin -> audio FIFO -> ALSA playback
                                  ^             |
                                  |             +-- DSP: stateful two-ear FIR
                                  |
                           control / telemetry FIFOs
                                  |
                           independent Qt client
```

Qt 不读取主音频 FIFO，不与播放进程争抢音频数据。预览采用单独、非阻塞的低频支路。

## 快速开始

电脑端需要 Python 3.8+ 和 C99 编译器（GCC、Clang 或 Zig），Python 工具无需第三方包。

```sh
python tools/build.py --test
python -m unittest discover -s tests -p 'test_*.py' -v
python tools/demo.py
```

Windows 可指定 Zig：

```powershell
python tools/build.py --zig C:\path\to\zig.exe --test
python tools/demo.py
```

打开 `results/demo/direction_a.wav` 和 `direction_b.wav` 查看两方向输出，`silence.wav` 应保持全零。`summary.json` 保存实际运行信息，每个方向的 CSV 包含逐块处理耗时。

**默认系数仅为原创的合成延迟/增益滤波器，标记为 `synthetic=1`，用于验证数据流和数值行为，不是实测 HRIR，也不构成真实空间定位效果验证。**

Linux 也可使用 `make test`。旧课程虚拟机无需安装现代 Python，沿用已有交叉编译器即可：

```sh
make CC=arm-none-linux-gnueabi-gcc BUILD=build-arm
```

通用 ARM 编译通过不等于兼容实际板上根文件系统，部署时须核对本机 EZSDK 工具链与运行库。

## 音频与 HRIR

- 默认音频为 **48 kHz、S16_LE、双声道交织 PCM**，每次最多处理 1024 帧，默认滤波器 512 抽头。
- 双声道输入用宽累加求平均形成单声源，左右耳分别滤波；旁路输出同一单声源的双声道副本。
- FIR 使用 64 位累加、Q15 半值离零舍入和 int16 饱和，历史样本跨块保留。
- 方向/旁路切换默认用 1024 帧交叉淡化，输出与输入等长，不自动追加卷积尾巴。
- 支持最多 1024 抽头，便于保留重采样后的完整 HRIR；1024 帧与 512 抽头是不同概念。

```sh
python tools/audio_tools.py generate --output results/source.wav
python tools/audio_tools.py convert --input results/source.wav --output results/source.pcm
python tools/audio_tools.py import-hrir \
  --a-left external/hrir/a-left.wav --a-right external/hrir/a-right.wav \
  --b-left external/hrir/b-left.wav --b-right external/hrir/b-right.wav \
  --output assets/imported/course.hrir --source "Dataset name and direction metadata" \
  --license "Dataset license or local-use restriction"
```

四份 HRIR 必须是同采样率的 16 位单声道 WAV。非 48 kHz 数据默认拒绝导入；确认来源与采样率后，可显式加 `--resample` 进行离线 windowed-sinc 系数转换。工具保留完整长度、记录源文件 SHA256、采样率、转换方式和来源说明，超出最大抽头数时拒绝导入，不静默截断。

```sh
./build/spatial-audio --input results/source.pcm --output results/rendered.pcm \
  --bank assets/imported/course.hrir --direction 0 --stats results/blocks.csv
python tools/audio_tools.py convert --input results/rendered.pcm --output results/rendered.wav
python tools/summarize.py results/blocks.csv --output results/summary.json
```

Windows 将可执行文件替换为 `build\spatial-audio.exe`。原始 PCM 不含采样率元数据，运行前须自行确认，WAV 转换工具会检查格式。

## Linux 实时链路与 Qt

先参考 [开发板准备](docs/board-bringup.md) 验证声卡和接口，再进行短时 CPU 录放测试：

```sh
sh scripts/run-audio.sh --backend cpu --duration 30
```

脚本打印本次会话的 runtime directory，录放、后台进程和 CSV/日志分别保存在 `results/board/`。CPU 实时性需要实测，未达到块预算时不应宣称实时合格。

构建 Qt（旧板优先现有 Qt4.6；普通 Linux 可用 Qt5）：

```sh
mkdir -p build/qt
cd build/qt
qmake ../../qt/spatial-audio.pro
make
./dm8168-audio-qt --runtime-dir /tmp/printed-session-directory
```

板上 Qt4 额外加 `-qws`；Qt5 根据已配置环境选择 `-platform linuxfb`，不要混用两套部署方式。GUI 提供恢复/静音输出、方向 A/B、旁路、波形预览与状态。静音保留音频流，结束启动脚本才释放 ALSA 设备。

当前缺少显示器时，先完成文件与音频后端阶段；板上 Qt 演示需要外接显示器，常见笔记本 HDMI 接口是输出端。

下面是 GitHub Actions 实际构建运行生成的 Qt5/Linux 界面，展示未连接后端时的状态，板端显示仍需单独验证。

![Qt client disconnected state](docs/images/qt-preview.png)

## RPE 接入状态

`--backend rpe` 需要真实课程 RPE 插件，缺失时明确失败，不会自动用 CPU 冒充 DSP。仓库提供主机插件 ABI 与可移植 DSP 算法包装层，未包含课程原始 RPE 工程、TI 二进制库或已经完成的 TI xDM 适配。

请按 [RPE 接入说明](docs/rpe-integration.md) 核对课程 `rpe_1_00_00_12/tests`，先跑 COPY，再用本项目算法替换 DSP 处理回调，最后封装 ARM 插件。课程资源放在被忽略的 `external/` 中。

## 验证与后续迭代

| 内容 | v0.1 状态 |
| --- | --- |
| CPU 文件处理、Q15 参考、分块连续性、淡化 | 有可执行测试与独立参考 |
| Linux FIFO、插件 ABI、Qt5、通用 ARM 构建 | 由 GitHub Actions 检查 |
| 课程 Qt4 / EZSDK 工具链 ABI | 待本地课程环境验证 |
| 真实 RPE / C674x 运行 | 待取得课程工程并适配 |
| 实测 HRIR 听感、ALSA 欠载与 30 分钟稳定性 | 待连接开发板验收 |

`process_us` 表示 CPU 核心调用或插件同步处理调用耗时，不等于纯 DSP 计算耗时或端到端音频延迟。ALSA 日志中的欠载/溢出需要单独检查，汇总工具不会把缺失日志当成零次事件。

后续按顺序完成：课程环境清单 → RPE COPY → 导入真实 HRIR → DSP 离线对照 → 实时链路 → 板上 Qt → 30 分钟验收。

- [架构与协议](docs/architecture.md)
- [开发板准备](docs/board-bringup.md)
- [RPE 接入说明](docs/rpe-integration.md)
- [测试与验收](docs/validation.md)
- [Qt 客户端说明](qt/README.md)

本仓库只提交自行编写的工程与原创合成测试素材生成代码，课件、SDK、虚拟机和来源未确认可分发的 HRIR/音频通过外部依赖说明提供。
