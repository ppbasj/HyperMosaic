# HyperMosaic

HyperMosaic 是一个基于 C++ 与 FFmpeg 的命令行视频脱敏工具，面向人脸或车牌场景进行自动追踪与马赛克渲染，并输出处理后视频。目前仅实现人脸识别。

项目当前聚焦于 Windows 10/11 + MSVC + CMake 工作流，已实现 Demux -> Decode -> Track/Render -> Encode 四级流水线。

优点：通过多线程实现流水线，提高效率

## 1. 核心能力

- 输入视频，按目标类型（face 或 plate）进行自动打码。
- 支持四级流水线并发处理，阶段间通过有界队列解耦。
- 使用 RAII 封装 FFmpeg 资源，降低内存泄漏与异常路径资源泄漏风险。
- 人脸模式优先使用 OpenCV CascadeClassifier；不可用时自动回退启发式检测器。
- 追踪阶段维护目标轨迹状态，检测帧和预测帧结合，减少检测调用频率。

## 2. 技术栈与依赖

- 语言: C++
- 构建系统: CMake
- 编译器: MSVC
- 核心依赖: FFmpeg (libavcodec/libavformat/libavutil)
- 依赖: OpenCV

## 3. 架构总览

### 3.1 四级流水线

1. Demux 阶段
2. Decode 阶段
3. Track & Render 阶段
4. Encode 阶段

### 3.2 线程间数据流向

1. Demux 线程读取输入容器并产出 unique_ptr<AVPacket>
2. unique_ptr<AVPacket> 进入 MPSCQueue<Packet>
3. Decode 分发器按轮询将包分发到 worker 私有队列
4. Decode worker 使用各自 AVCodecContext 解码，产出 shared_ptr<AVFrameWrapper>
5. Track & Render 线程消费帧，执行 检测/预测/关联，随后在 Y 平面渲染马赛克
6. Encode 线程消费渲染后帧，编码并复用器封装到输出文件

### 3.3 关键并发模型

- 队列类型: MPSCQueue<T>（有界、阻塞、支持 Close 与 stop 标志）
- Demux -> Decode: unique_ptr<AVPacket>
- Decode -> TrackRender: shared_ptr<AVFrameWrapper>
- TrackRender -> Encode: shared_ptr<AVFrameWrapper>

## 4. 核心模块职责与接口设计思路

### 4.1 FFmpeg RAII 封装层

位置: src/ffmpeg/ffmpeg_raii.h, src/ffmpeg/ffmpeg_raii.cpp

职责:

- 为 AVFormatContext/AVCodecContext/AVCodecParameters/AVFrame/AVPacket 提供自定义 deleter。
- 通过 unique_ptr 别名统一资源生命周期管理。
- 提供 MakePacket/MakeFrame/MakeCodecContext 等工厂函数。
- 提供 AvErrorToString 统一错误码可读化。

接口设计要点:

- 资源创建失败返回空智能指针，调用方显式判空。
- 所有释放逻辑收敛在 deleter，避免跨模块重复清理代码。

### 4.2 DemuxStage

位置: src/pipeline/demux_stage.h, src/pipeline/demux_stage.cpp

职责:

- 打开输入文件并读取流信息。
- 自动定位最佳视频流，复制 codec 参数供下游解码使用。
- 将视频包推送到下游队列，忽略非视频流。

接口设计要点:

- Prepare 负责输入打开与流探测，Run 负责持续读包。
- 公开 video_stream_index/video_time_base/video_codec_parameters 供 Decode 初始化。

### 4.3 DecodeThreadPool

位置: src/pipeline/decode_stage.h, src/pipeline/decode_stage.cpp

职责:

- 管理解码工作线程与每线程 codec context。
- 接收 AVPacket 并输出 AVFrameWrapper（含 PTS、解码顺序、worker id）。

接口设计要点:

- ValidateOptions 先做参数防御。
- 通过 frame wrapper 保留时间戳与诊断信息，减少跨阶段上下文丢失。
- 当前版本为了跨帧参考正确性，worker 数实际强制回退为 1（即使传入更大值）。

### 4.4 TrackRenderStage

位置: src/pipeline/track_render_stage.h, src/pipeline/track_render_stage.cpp

职责:

- 在检测间隔帧调用 detector，非检测帧进行轨迹预测。
- 使用 TrackManager 维护轨迹并输出当前活跃框。
- 直接修改 AVFrame 的亮度平面（Y）进行块状马赛克渲染。

接口设计要点:

- 通过 detectorInterval 控制检测开销。
- face 与 plate 支持不同 mosaic block 大小。
- 统计输出帧数、检测调用次数、最大活跃轨迹数用于运行观测。

### 4.5 EncodeStage

位置: src/pipeline/encode_stage.h, src/pipeline/encode_stage.cpp

职责:

- 首帧驱动编码器初始化（分辨率、像素格式、time base、muxer header）。
- 消费渲染后 AVFrame，处理 PTS 单调性并编码写包。
- flush 编码器并写 trailer。

接口设计要点:

- 根据输入帧来源时间基进行 PTS 重采样。
- 检测到回退时间戳时做单调修正，避免封装器时间戳错误。
- 当前实现会记录硬件编码参数，但编码器路径仍以软件编码为主。

### 4.6 检测与追踪模块

位置: src/tracker/tracker_core.h, src/tracker/tracker_core.cpp

职责:

- IDetector 抽象检测器接口。
- HeuristicDetector 提供轻量回退检测策略（face/plate）。
- OpenCvCascadeDetector 提供级联人脸检测（可选编译）。
- TrackManager 提供简化运动预测与最近邻匹配，多目标轨迹维护。

接口设计要点:

- 检测器可插拔，TrackRender 只依赖 IDetector。
- 轨迹状态携带速度、漏检计数、置信度，便于短时遮挡与噪声抑制。

### 4.7 启动与编排

位置: src/main.cpp

职责:

- CLI 解析与参数校验。
- 输入输出路径检查、输出目录准备、信号中断桥接。
- 统一装配四个 stage 并管理线程生命周期。

## 5. 构建指南

## 5.1 前置环境

- Windows 10/11
- Visual Studio 2022 (含 MSVC x64 工具链)
- CMake 3.20+
- FFmpeg 头文件与库（可使用仓库中的 ffmpeg-prebuilt 或自行构建）
- 可选: OpenCV（用于更真实的人脸检测）

### 5.2 方式 A：使用预编译 FFmpeg

示例命令（PowerShell）:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DHYPERMOSAIC_FFMPEG_ROOT="E:/HyperMosaic/ffmpeg-prebuilt" -DHYPERMOSAIC_ENABLE_OPENCV_CASCADE=ON

cmake --build build --config Release
```

说明:

- HYPERMOSAIC_FFMPEG_ROOT 需要指向含 include/lib/bin 的 FFmpeg 根目录。
- 如果开启 HYPERMOSAIC_COPY_FFMPEG_DLLS，构建后会尝试复制 FFmpeg DLL 到可执行文件目录。


### 5.3 方式 B: 本地编译 FFmpeg（仓库脚本）

项目提供脚本: scripts/build_ffmpeg_msvc.ps1

用途:

- 在 bash + make 环境下以 msvc toolchain 编译 FFmpeg。
- 默认安装到 ffmpeg-8.1/local-install。

执行示例:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build_ffmpeg_msvc.ps1
```

完成后配置 CMake:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DHYPERMOSAIC_FFMPEG_ROOT="E:/HyperMosaic/ffmpeg-8.1/local-install"

cmake --build build --config Release
```


## 6. 运行指南

可执行文件位于:

- build/Release/hyper_mosaic.exe

### 6.1 基础示例

```powershell
.\build\Release\hyper_mosaic.exe --input test/test1.mp4 --target face --output test/test1_mosaic.mp4
```

### 6.2 人脸检测（显式指定 cascade）

```powershell
.\build\Release\hyper_mosaic.exe --input test/test1.mp4 --target face --face-cascade models/haarcascade_frontalface_default.xml --output test/test1_mosaic_opencv.mp4
```

### 6.3 车牌模式（暂未实现）

```powershell
.\build\Release\hyper_mosaic.exe --input test/test2.mp4 --target plate --output test/test2_mosaic.mp4
```

### 6.4 仅验证装配（不执行编解码）

```powershell
.\build\Release\hyper_mosaic.exe --input test/test1.mp4 --target face --dry-run
```

## 7. CLI 参数说明

必选参数:

- --input <path>: 输入视频文件路径
- --target <face|plate>: 打码目标类型（目前只可以选face）

可选参数:

- --output <path>: 输出视频路径；默认 input_stem_mosaic.原扩展名（无扩展名则 .mp4）
- --decode-workers <n>: 解码 worker 数，默认 1，范围 1-256
- --packet-queue <n>: Demux -> Decode 队列容量，默认 512，范围 64-8192
- --frame-queue <n>: Decode -> Track 与 Track -> Encode 队列容量，默认 256，范围 32-8192
- --detector-interval <n>: 检测间隔帧，默认 6，范围 1-240
- --face-cascade <path>: OpenCV Cascade 模型路径（face 模式可选）
- --hw-encode: 打开硬件编码开关
- --no-hw-encode: 显式关闭硬件编码
- --hw-device <name>: 指定硬件设备名（如 d3d11va，同时打开硬件编码开关）
- --dry-run: 仅验证配置与流水线装配
- -h, --help: 帮助信息

位置参数模式:

- hyper_mosaic <input> <face|plate> [output]

## 8. 关键实现要点

1. 资源生命周期

- FFmpeg 核心对象都封装到 unique_ptr + custom deleter，避免手工 free 漏洞。

2. 解码与时间戳

- Decode 输出 frame wrapper，保留 best_effort_pts、packet_pts、decode_order 等元数据。
- Encode 阶段进行时间基换算和单调性修正，避免回退 PTS 破坏封装。

3. 追踪与渲染

- 检测只在间隔帧执行，其他帧依赖预测，兼顾实时性和稳定性。
- 马赛克直接作用于 Y 平面，降低计算复杂度并避免额外滤镜链依赖。

4. 容错与降级

- OpenCV 不可用时自动回退启发式检测器，保证流程可运行。
- 解码阶段统一首错误码回传，主流程按阶段输出明确退出码。

## 9. 潜在技术难点与当前方案

1. B 帧与参考帧导致的多线程解码一致性问题

- 现状: decode worker 参数可配置，但实际回退为 1，优先保证正确性。
- 后续方向: 基于 GOP/关键帧分段或单上下文串行送包+并行后处理实现更安全扩展。

2. 检测精度与实时性平衡

- 现状: detectorInterval + 轨迹预测降低检测频率。
- 后续方向: 引入更强检测器（如 DNN/ONNX）并做多级策略切换。

3. 编码器像素格式兼容性

- 现状: 若编码器不支持输入像素格式，直接报错退出。
- 后续方向: 引入 swscale 做像素格式转换与色彩空间适配。

4. 硬件编码路径

- 现状: 已提供参数入口，编码链路仍主要走软件编码。
- 后续方向: 接入 hw device context + hw frame 流程实现真正 GPU 编码。

## 10. 项目目录

```text
HyperMosaic/
|- CMakeLists.txt
|- README.md
|- scripts/
|  |- build_ffmpeg_msvc.ps1
|- src/
|  |- main.cpp
|  |- concurrent/
|  |  |- mpsc_queue.h
|  |- ffmpeg/
|  |  |- ffmpeg_raii.h
|  |  |- ffmpeg_raii.cpp
|  |- pipeline/
|  |  |- demux_stage.*
|  |  |- decode_stage.*
|  |  |- track_render_stage.*
|  |  |- encode_stage.*
|  |- tracker/
|     |- tracker_core.*
|- models/
|  |- haarcascade_frontalface_default.xml

```

## 11. 路线图建议

- 完整硬件编解码链路（D3D11VA / NVENC / QSV）
- 像素格式转换与分辨率自适配
- 更强检测器（ONNX Runtime）与多目标类型扩展
- 更细粒度性能指标与端到端 benchmark

## 12. 后续功能扩展方向考虑

因为 openCV 在某些情况下的鲁棒性不是很好，可以考虑接入ai。
