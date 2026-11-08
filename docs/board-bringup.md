# DM8168 板端分阶段验证

本流程从已能启动的课程开发板和现有 EZSDK 环境开始，逐层验证连接、交叉编译、音频、RPE和显示。v0.1 的主机 CPU 实验可以先完成，缺少 HDMI 显示器不阻塞离线数值验证。

课程资料使用 `ti-ezsdk_dm816x-evm_5_05_01_04`。以下 `SDK_ROOT` 是可调整的示例路径，应指向本机已经部署并能工作的 SDK。优先复用现有环境和课程原始示例，不以重刷镜像作为第一步。

## 1 记录现有系统与接口

笔记本负责串口终端、文件传输与编译，板端负责实际运行。串口连接器、电平、电源和波特率以具体开发板手册为准，USB转串口适配器应匹配板端接口。

在已启动的目标板终端执行并保存日志：

```sh
uname -a
cat /proc/version
cat /proc/meminfo
cat /proc/asound/cards
arecord -l
aplay -l
ifconfig
```

记录板卡型号、SDK实际版本、工具链版本、启动介质、文件传输方式、声卡录放设备、Qt版本与显示设备。若已经使用 NFS 根文件系统，沿用当前有效挂载，不替换整个文件系统。

本流程不修改 U-Boot 参数、Linux `mem=` 配置或板上固件服务。旧 SysLink 教程中为特定内存映射采取的 `mem=169M`、卸载显示固件或停止 `load-hd-firmware` 等操作，不可直接套用到当前 RPE与Qt共存流程。

## 2 验证交叉编译与文件传输

在 SDK 的 Linux 编译环境中设置实际工具链路径，例如：

```sh
export SDK_ROOT="$HOME/ti-ezsdk_dm816x-evm_5_05_01_04"
export CROSS_COMPILE="/path/to/installed/toolchain/bin/arm-none-linux-gnueabi-"
"${CROSS_COMPILE}gcc" --version
```

先使用课程已经配置的 Hello World 示例验证编译与运行，再构建本项目。现代主机上的 Python 素材工具需要 Python 3.8或更新版本；原始 Ubuntu 虚拟机若只有 Python 2，可以使用项目 Makefile 编译 C 程序，板端运行本身不要求 Python。

在项目根目录使用现有工具链构建：

```sh
make CC="${CROSS_COMPILE}gcc" BUILD=build-arm
```

`BUILD` 是输出目录变量，交叉编译时不要在宿主直接运行ARM测试程序。此构建生成的是ARM Linux程序，CPU后端仍在ARM执行，不表示DSP加速已经接入。保持ARM Linux ABI、浮点ABI、动态加载器和目标根文件系统一致，避免混用另一套现代ARM编译器及其运行库。旧glibc的 `clock_gettime` 链接可能需要 `-lrt`，应保留项目针对Linux的链接配置。

通过已经验证的NFS、SCP或可移动介质传输项目生成的可执行文件与测试素材，板端放在单独的应用目录中。若使用实时启动脚本，保留项目目录结构，将 `build-arm/spatial-audio` 部署为板端项目的 `build/spatial-audio`。不要覆盖现有启动文件、系统库或课程原始例程。通过 `file` 检查ARM可执行文件和动态加载器依赖；若工具不在板上，可在编译宿主上检查。

## 3 先完成板端 CPU 离线运行

在现代主机准备一个短的原创测试音和显式标记的合成系数库：

```sh
python3 tools/audio_tools.py generate --output results/input.pcm --duration 2
python3 tools/audio_tools.py make-demo-bank --output results/synthetic.hrir
```

将 `input.pcm`、`synthetic.hrir` 与 ARM 可执行文件放到板端应用目录，在该目录运行：

```sh
./spatial-audio --backend cpu --input input.pcm --output output-a.pcm --bank synthetic.hrir --direction 0 --stats cpu-a.csv
./spatial-audio --backend cpu --input input.pcm --output output-b.pcm --bank synthetic.hrir --direction 1 --stats cpu-b.csv
```

将输出取回现代主机转换为 WAV 并与 Python 参考结果比较。日志中的 `SYNTHETIC_TEST_NOT_MEASURED_HRIR` 是预期标记，表示当前只验证软件链路。

## 4 单独验证 ALSA 录放

先用 `arecord -l`、`aplay -l` 确认实际设备名称。以下设备标识需要替换为本板对应的录音与播放端，不假设两者相同：

```sh
export CAPTURE_DEVICE="hw:0,0"
export PLAYBACK_DEVICE="hw:0,0"
arecord -D "$CAPTURE_DEVICE" -t raw -f S16_LE -c 2 -r 48000 -d 5 capture.pcm 2>capture.log
aplay -D "$PLAYBACK_DEVICE" -t raw -f S16_LE -c 2 -r 48000 capture.pcm 2>playback.log
```

确认硬件确实接受48000 Hz、S16_LE和双通道，录放日志无协商失败。`hw:` 通常直接反映硬件支持能力；若改用可做格式转换的 `plughw:`，应记录转换，不把它当成硬件原生支持的证明。

测试信号先用较低幅度，播放设备音量从低档开始。先确保录放直通正确，再加入音频 FIFO和后端处理。课程部分录放示例使用44100 Hz，本项目统一48000 Hz；不能只改变 PCM 文件名或播放参数来替代实际转换。

## 5 分开验证 RPE 与显示

按 [RPE 集成步骤](rpe-integration.md) 先运行原始 copy 示例，再验证简单计算、FIR和连续跨块历史。在完成真实 RPE 适配前继续使用 `--backend cpu`；不要通过修改界面文字或遥测后端名称宣称 DSP 已工作。

Qt 4.6优先使用课程已配套的目标板 qmake、库和显示驱动。先运行课程中已知可用的 Qt 小程序，再运行本项目客户端。Qt4 Embedded 的 `-qws` 与 Qt5 的 `linuxfb` 是不同机制，具体命令见 [Qt说明](../qt/README.md)。

缺少显示器时，可以完成板上命令行运行、离线处理、RPE通信和日志记录；Qt 板端显示验收暂记为待实测。桌面 Qt 窗口运行也不能替代板上 framebuffer 与显示驱动验证。

## 6 最后串起实时会话

使用项目启动脚本建立“arecord → 独占输入FIFO → 后端 → 独占输出FIFO → aplay”，Qt只连接控制与遥测管道。在板端项目根目录先运行一个30秒CPU会话：

```sh
sh scripts/run-audio.sh --backend cpu --bank synthetic.hrir --capture-device "$CAPTURE_DEVICE" --playback-device "$PLAYBACK_DEVICE" --duration 30
```

`--duration` 的单位为秒。脚本会打印唯一运行目录，Qt的 `--runtime-dir` 必须使用该值；也可以给脚本和Qt显式指定相同目录。日志与CSV位于 `results/board`，下一次会话会写入相同文件名，应在重复测试前归档需要保留的结果。真实RPE会话增加 `--backend rpe --rpe-plugin` 和已完成适配的共享库路径，当前仓库不提供该库。

依次验证录放直通、CPU旁路、CPU FIR、RPE FIR、Qt切换和界面断开。每一级保存命令、stderr、系数元数据及统计文件，发生问题时回到上一层已通过的单独测试，而非同时修改采集、固件和显示。

最后执行 [30分钟稳定性验收](validation.md)，如实记录未完成或未达到目标的项目，不预填板端吞吐量或延迟数字。
