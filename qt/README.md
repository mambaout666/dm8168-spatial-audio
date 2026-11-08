# Qt 独立控制界面

该目录提供一个面向 Linux 的 Qt 客户端，兼容 Qt 4.6 API 和 Qt 5。它只通过命名管道发送控制命令、读取状态和少量波形预览，不负责启动 ALSA、加载 DSP 固件或执行音频处理。界面以后台进程的状态为准；未连接后端时显示断线和空波形，不生成演示波形。

## 构建

使用与目标板 Qt 库配套的 qmake 和交叉编译工具链。在当前目录之外建立构建目录，例如：

```sh
mkdir -p build/qt
cd build/qt
/path/to/target-qt/bin/qmake ../../qt/spatial-audio.pro
make
```

上述相对路径以从项目根目录执行为前提，生成文件位于项目的 `build/qt`。桌面 Linux 验证可将 qmake 替换为系统 Qt 5 的 qmake。Qt 4 分支的源码不使用 C++11；Qt 5 分支启用其工具链需要的 C++11 编译选项。此项目依赖 Linux/POSIX 的 FIFO API，不面向原生 Windows 构建。

## 运行

先按项目根目录说明启动后台会话，再运行界面。默认运行目录为 `/tmp/dm8168-spatial-audio`，也可用 `--runtime-dir` 指定另一个运行目录，必须与后端一致：

```sh
./dm8168-audio-qt --runtime-dir /tmp/dm8168-spatial-audio
```

使用 Qt 4 Embedded/QWS 构建且目标板需要本程序成为 QWS 服务器时，增加 `-qws`：

```sh
./dm8168-audio-qt -qws --runtime-dir /tmp/dm8168-spatial-audio
```

`-qws` 仅适用于相应的 Qt 4 Embedded 构建，Qt 4 X11 版和 Qt 5 都不使用该参数。Qt 5 直接访问 Linux framebuffer 需已安装匹配的 linuxfb 平台插件，例如：

```sh
QT_QPA_PLATFORM=linuxfb ./dm8168-audio-qt --runtime-dir /tmp/dm8168-spatial-audio
```

Qt 5 桌面 X11 会话一般直接使用默认平台插件。板上显示驱动和运行时插件的准备由目标板环境负责。

## 按钮与显示

- **Resume output / Mute output**：分别发送 `START` 和 `STOP`；停止含义是静音而非退出进程，音频流保持打开。真正结束会话并释放 ALSA 设备需结束后台启动脚本。
- **Direction A / Direction B**：分别发送方向编号 0 和 1；方向对应的 HRIR 由后端配置，界面不假设它们的角度。
- **Bypass**：开启或关闭空间渲染旁路。
- **Backend**：显示实际后端为 CPU reference 或 RPE / DSP，二者不混用。
- **Filter bank**：后端标记 `synthetic=1` 时明确显示 Synthetic test，表示当前使用教学用合成系数；`synthetic=0` 显示 Imported data。该字段描述滤波系数库，不代表音频输入来源，也不能单独证明系数是实测 HRIR。
- **波形**：只展示后台提供的每通道16个有符号16位采样点，属于抽样预览，不能用于判断完整频谱或证明音效正确性。
- **断线**：超过1.5秒没有合法 STATUS 时显示 Disconnected，清空过期波形和指标并禁用控制；后端恢复或重新创建 FIFO 后自动重连。

## 通信约定

所有管道均由后端或会话启动脚本创建，客户端不会创建或删除它们。仅运行一个界面读取 `telemetry.fifo`；多个 FIFO 读者会分走数据，不能实现广播。

客户端每100毫秒读取一次 `telemetry.fifo`，使用 `O_RDONLY | O_NONBLOCK`，读取预算每次最多64 KiB。没有写入者时保留读端，允许后端以后用非阻塞写入方式连接。读取过程缓存分段到达的文本，完整一行后再解析；丢弃过长或格式错误的消息。音频采样不经过这个管道。

状态行使用下列字段，计数器为非负64位整数，布尔值和方向只能是0或1，`process_us` 为非负数字：

```text
STATUS blocks=100 frames=102400 running=1 direction=0 bypass=0 process_us=412.5 deadline_misses=0 visual_drops=0 synthetic=0 backend=cpu
WAVE <16 input samples> | <16 output-left samples> | <16 output-right samples>
```

`backend` 只接受 `cpu` 或 `rpe`。WAVE 三组数据均为空格分隔的16个整数，取值范围为 -32768 到 32767。每行都以换行结束，后端应控制发送频率不超过10 Hz，单次原子写入不超过 `PIPE_BUF`，并以非阻塞方式丢弃来不及显示的预览。

控制命令使用 `O_WRONLY | O_NONBLOCK` 打开独立的 `control.fifo`，一次写入一行，不等待后端应答；界面收到下一条 STATUS 后显示实际状态。命令为：

```text
START
STOP
DIRECTION 0
DIRECTION 1
BYPASS 0
BYPASS 1
```

控制管道没有读者或暂时写满时，界面报告命令未发送，不阻塞音频或图形事件循环。写端消失导致的 SIGPIPE 被转换为普通错误处理。

## 建议验证

1. 没有后端时启动界面，确认窗口可操作、状态为断线、三个波形区域为空。
2. 启动后端，确认状态、CPU/RPE标识与合成系数标识正确，波形刷新不超过10 Hz。
3. 依次测试静音、恢复、方向切换和旁路，确认命令与后续 STATUS 一致。
4. 结束后端，确认最多1.5秒后清空状态；在界面不退出的情况下重新启动后端，确认能恢复。
5. 验证界面关闭、暂停或预览消费者变慢时，后台音频主链路仍可继续运行。
