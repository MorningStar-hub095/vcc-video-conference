# VCC视频会议 项目 WebRTC 集成实施方案

> **文档性质：** 技术实施方案与执行计划
> **编写日期：** 2026-07-31（v1.1 更新：2026-08-01，第八章静音检测功能已实施落地）
> **适用范围：** VCC视频会议 视频会议系统客户端 + 服务端
> **方案版本：** v1.1
> **前置条件：** HD 升级已完成（1280×720 摄像头 + 1920×1080 桌面共享）

---

## 目录

- [一、技术选型与架构设计](#一技术选型与架构设计)
- [二、开发环境搭建与依赖配置](#二开发环境搭建与依赖配置)
- [三、核心功能模块实现步骤](#三核心功能模块实现步骤)
- [四、性能优化策略](#四性能优化策略)
- [五、测试计划](#五测试计划)
- [六、项目时间轴与资源分配](#六项目时间轴与资源分配)
- [七、风险评估与应对措施](#七风险评估与应对措施)
- [八、基于 WebRTC 的静音检测功能实现](#八基于-webrtc-的静音检测功能实现)
  - [8.1 实现原理](#81-实现原理)
  - [8.2 音频流捕获与处理流程](#82-音频流捕获与处理流程)
  - [8.3 音量阈值设定与动态调整机制](#83-音量阈值设定与动态调整机制)
  - [8.4 静音状态判定逻辑与事件触发机制](#84-静音状态判定逻辑与事件触发机制)
  - [8.5 浏览器兼容性处理方案](#85-浏览器兼容性处理方案)
  - [8.6 性能优化建议](#86-性能优化建议)
  - [8.7 实施步骤与验证清单](#87-实施步骤与验证清单)

---

## 一、技术选型与架构设计

### 1.1 WebRTC 在 VCC视频会议 项目中的应用场景

| 应用场景 | 当前实现 | WebRTC 集成后 | 改进效果 |
|----------|----------|---------------|----------|
| **实时音视频通话** | TCP 传输 + H.264/Opus | RTP/SRTP 传输 + WebRTC 媒体引擎 | 延迟从 300-500ms 降至 100-200ms |
| **屏幕共享** | TCP 传输 + H.264 | RTP/SRTP 传输 + SVC 分层编码 | 弱网体验质变，抗 80% 丢包 |
| **多人会议** | 服务端 TCP 转发（全量复制） | SFU 选择性转发 + 拥塞控制 | 带宽节省 60%，支持 50+ 人 |
| **NAT 穿透** | 不支持（仅局域网） | ICE/STUN/TURN | 支持公网跨网段通信 |
| **数据通道** | 无 | WebRTC DataChannel | 支持低延迟文本/文件传输 |
| **安全加密** | 明文传输 + MD5 | DTLS-SRTP 端到端加密 | 全链路加密，防窃听篡改 |

### 1.2 现有架构分析

#### 1.2.1 客户端架构

```
┌──────────────────────────────────────────────────────┐
│                     Qt GUI 层                         │
│  LoginDialog / VCC视频会议Dialog / RoomDialog             │
├──────────────┬──────────────┬────────────────────────┤
│   视频模块    │   音频模块    │      网络模块          │
│  VideoRead   │  AudioRead   │   TcpClient            │
│  (OpenCV采集) │  (SDL采集)   │   TcpClientMediator    │
│      ↓       │      ↓       │      ↓                 │
│  H264Encoder │  Opus编码    │   packdef.h协议        │
│  (FFmpeg)    │              │   (自定义TCP包)         │
│      ↓       │      ↓       │      ↓                 │
│  H264Decoder │  Opus解码    │   Winsock2 TCP         │
│      ↓       │      ↓       │                        │
│  YuvGlWidget │  AudioWrite  │                        │
│  (OpenGL)    │  (SDL播放)   │                        │
├──────────────┴──────────────┴────────────────────────┤
│                   ckernel (核心调度)                   │
├──────────────────────────────────────────────────────┤
│  Qt 5.12 / MinGW / FFmpeg 4.2 / SDL2 / OpenCV / Opus │
└──────────────────────────────────────────────────────┘
```

#### 1.2.2 服务端架构

```
┌──────────────────────────────────────────────────────┐
│                 CLogic (业务逻辑层)                    │
│  LoginRq / RegisterRq / CreateRoomRq / JoinRoomRq    │
│  VideoFrameH264Rq (视频帧转发) / AudioFrameRq         │
├──────────────────────────────────────────────────────┤
│              TCPKernel (连接管理层)                    │
│  epoll 事件循环 + 线程池                               │
├──────────────────────────────────────────────────────┤
│           Block_Epoll_Net (网络层)                     │
│  Linux epoll + TCP Socket                            │
├──────────────────────────────────────────────────────┤
│              MySQL (数据持久化)                        │
└──────────────────────────────────────────────────────┘
```

#### 1.2.3 当前数据流

**视频流（发送）：**
```
OpenCV采集 → QImage → H264Encoder(FFmpeg) → H264码流
→ 分片(8KB) → packdef.h封装 → TCP发送 → 服务端转发
```

**视频流（接收）：**
```
服务端转发 → TCP接收 → 分片重组 → H264Decoder(FFmpeg)
→ AVFrame(YUV) → YuvGlWidget(OpenGL渲染)
```

**音频流（发送）：**
```
SDL采集 → PCM(48kHz/16bit) → Opus编码 → TCP发送 → 服务端转发
```

**音频流（接收）：**
```
服务端转发 → TCP接收 → Opus解码 → PCM → SDL播放
```

### 1.3 集成架构设计

采用**渐进式集成**策略，分三个阶段逐步替换，每个阶段可独立运行：

#### 阶段一：网络层替换（TCP → WebRTC PeerConnection）

```
┌──────────────────────────────────────────────────────┐
│                     Qt GUI 层（不变）                  │
├──────────────┬──────────────┬────────────────────────┤
│   视频模块    │   音频模块    │    WebRTC网络模块       │
│  VideoRead   │  AudioRead   │   PeerConnection       │
│  H264Encoder │  Opus编码    │   ↓                    │
│  H264Decoder │  Opus解码    │   RTP/SRTP传输         │
│  YuvGlWidget │  AudioWrite  │   ICE/STUN/TURN        │
│      ↑↓      │      ↑↓      │   ↓                    │
│   媒体桥接层   │   媒体桥接层  │   DTLS加密             │
│ (QImage↔VideoFrame) │ (PCM↔AudioFrame) │            │
├──────────────┴──────────────┴────────────────────────┤
│              WebRTC 信令适配层（新增）                 │
│         (复用现有TCP连接传输SDP/ICE候选)              │
├──────────────────────────────────────────────────────┤
│  Qt 5.12 / FFmpeg / SDL / WebRTC Native API          │
└──────────────────────────────────────────────────────┘
```

#### 阶段二：媒体引擎替换（FFmpeg → WebRTC 媒体引擎）

```
┌──────────────────────────────────────────────────────┐
│                     Qt GUI 层（不变）                  │
├──────────────┬──────────────┬────────────────────────┤
│  WebRTC视频   │  WebRTC音频   │    WebRTC网络模块       │
│  VideoTrack  │  AudioTrack  │   PeerConnection       │
│  (内置H.264)  │  (内置Opus   │   RTP/SRTP             │
│              │   +AEC3+NS)  │   ICE/STUN/TURN        │
│      ↓       │              │                        │
│  YuvGlWidget │              │                        │
│  (OpenGL不变) │              │                        │
├──────────────┴──────────────┴────────────────────────┤
│         WebRTC Native API (统一媒体引擎)              │
└──────────────────────────────────────────────────────┘
```

#### 阶段三：SFU 服务器部署（支持大房间）

```
┌──────────┐     ┌──────────┐     ┌──────────┐
│ Client A │◄───►│          │◄───►│ Client B │
│          │     │  SFU     │     │          │
│ WebRTC   │     │ Server   │     │ WebRTC   │
│ PeerConn │     │(mediasoup│     │ PeerConn │
└──────────┘     │ /LiveKit)│     └──────────┘
                 └────┬─────┘
                      │
                 ┌────┴─────┐
                 │ Client C │
                 │ WebRTC   │
                 └──────────┘
```

### 1.4 技术选型对比与决策

#### 1.4.1 WebRTC 获取方式对比

| 方案 | 描述 | 优势 | 劣势 | 推荐度 |
|------|------|------|------|--------|
| **A. 源码编译** | 从 Google 获取 WebRTC 源码，使用 depot_tools + gn + ninja 编译 | 完整功能，可定制裁剪 | 编译复杂（6-8小时），源码约 15GB | ★★★ |
| **B. 预编译库** | 使用第三方提供的预编译 WebRTC 库（如 webrtc-build-scripts） | 快速集成，无需编译 | 版本可能滞后，平台限制 | ★★★★ |
| **C. LiveKit SDK** | 使用 LiveKit 提供的 C++ SDK（封装了 WebRTC） | 开箱即用，含 SFU | 依赖第三方，学习性低 | ★★ |
| **D. 渐进提取** | 仅提取 WebRTC 的网络层和音频处理模块源码 | 轻量，可控 | 需手动处理依赖关系 | ★★★★ |

**决策：采用方案 B（预编译库）+ 方案 D（渐进提取音频处理模块）**

理由：
1. 方案 B 提供完整的 PeerConnection API，快速实现网络层替换
2. 方案 D 的音频处理模块（AEC3/NS/AGC）已在项目中部分引入（WebRtcMoudle 目录）
3. 符合学习项目的可控性要求

#### 1.4.2 信令传输方案对比

| 方案 | 描述 | 优势 | 劣势 | 推荐度 |
|------|------|------|------|--------|
| **A. 复用现有 TCP** | 在现有 TCP 连接上传输 SDP/ICE 候选 | 无需额外部署 | TCP 已有业务数据，需区分 | ★★★★ |
| **B. WebSocket** | 新增 WebSocket 信令通道 | 标准化，易调试 | 需额外部署 WS 服务 | ★★★ |
| **C. HTTP 轮询** | 通过 HTTP 接口交换信令 | 简单 | 延迟高，不推荐 | ★ |

**决策：采用方案 A（复用现有 TCP 连接传输信令）**

理由：
1. 现有 TCP 连接已稳定运行
2. 在 `packdef.h` 中新增信令协议包类型即可
3. 无需额外部署服务

#### 1.4.3 媒体服务器方案对比

| 方案 | 描述 | 支持人数 | 部署复杂度 | 推荐度 |
|------|------|----------|-----------|--------|
| **Mesh (P2P)** | 客户端之间直接连接 | 4-6 人 | 无需服务器 | ★★ |
| **SFU** | 选择性转发单元 | 50+ 人 | 中等 | ★★★★ |
| **MCU** | 多点控制单元（混流） | 100+ 人 | 高（需转码） | ★★ |

**决策：阶段一/二采用 Mesh（P2P，≤4人），阶段三部署 SFU（mediasoup，支持大房间）**

---

## 二、开发环境搭建与依赖配置

### 2.1 WebRTC 源码获取与编译

#### 2.1.1 获取预编译库（推荐方案）

```bash
# 方式1：使用 webrtc-build-scripts 获取预编译库
git clone https://github.com/primojs/webrtc-build-scripts.git
# 下载 Windows MinGW 预编译版本

# 方式2：使用 Stream Labs 的 prebuilt-webrtc
# 下载地址：https://github.com/stream-labs/prebuilt-webrtc/releases
# 选择 webrtc-streamer-x86_64-windows-mingw 版本
```

预编译库包含：
- `include/` — WebRTC 头文件（api/、rtc_base/、media/、pc/、call/ 等）
- `lib/` — 静态库（`libwebrtc.a`）或动态库（`webrtc.dll`）
- `bin/` — 运行时依赖 DLL

#### 2.1.2 自行编译 WebRTC（可选，用于深度定制）

```bash
# 1. 安装 depot_tools
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
export PATH=$PATH:/path/to/depot_tools

# 2. 获取 WebRTC 源码
mkdir webrtc-checkout
cd webrtc-checkout
fetch --nohooks webrtc
gclient sync

# 3. 切换到稳定分支
cd src
git checkout branch-heads/6261  # M126 稳定版

# 4. 生成构建文件（Windows MinGW）
gn gen out/Default --args='
  target_os="win"
  target_cpu="x64"
  is_debug=false
  is_component_build=false
  rtc_use_h264=true
  rtc_include_tests=false
  rtc_build_examples=false
  use_custom_libcxx=false
  treat_warnings_as_errors=false
'

# 5. 编译（约 2-4 小时）
ninja -C out/Default
```

编译产物：
- `out/Default/obj/libwebrtc.a` — WebRTC 静态库（约 800MB）
- `out/Default/gen/` — 生成的头文件

### 2.2 开发工具版本要求

| 工具 | 当前版本 | WebRTC 要求版本 | 操作 |
|------|----------|-----------------|------|
| **MinGW-w64** | 16.1.0 (MSYS2) | ≥ 11.0 | 无需升级 |
| **Qt** | 5.12.11 | ≥ 5.9 | 无需升级 |
| **CMake** | 未安装 | ≥ 3.16 | 需安装（WebRTC 构建依赖） |
| **Python** | 未安装 | ≥ 3.8 | 需安装（depot_tools 依赖） |
| **FFmpeg** | 4.2.2 | 兼容 | 无需升级 |
| **SDL2** | 2.0.10 | 兼容 | 无需升级 |
| **OpenCV** | 已安装 | 兼容 | 无需升级 |

### 2.3 依赖配置

#### 2.3.1 新增依赖清单

| 依赖 | 版本 | 用途 | 获取方式 |
|------|------|------|----------|
| **WebRTC Native API** | M126+ | 音视频传输 + ICE + SRTP | 预编译库 |
| **libsrtp** | 2.5+ | SRTP 加密（WebRTC 内含） | 随 WebRTC |
| **OpenSSL** | 1.1.1+ | DTLS 握手（WebRTC 内含） | 随 WebRTC |
| **libusrsctp** | 最新 | SCTP 数据通道（WebRTC 内含） | 随 WebRTC |
| **coturn** | 4.6+ | STUN/TURN 服务器 | 独立部署 |
| **mediasoup** | 3.12+ | SFU 媒体服务器（阶段三） | Node.js 部署 |

#### 2.3.2 目录结构规划

```
VCC视频会议\
├── webrtc/                    # 新增：WebRTC 依赖
│   ├── include/               # WebRTC 头文件
│   │   ├── api/               # PeerConnection API
│   │   ├── rtc_base/          # 基础工具
│   │   ├── media/             # 媒体引擎
│   │   ├── pc/                # PeerConnection 实现
│   │   └── call/              # 呼叫管理
│   └── lib/
│       ├── libwebrtc.a        # WebRTC 静态库
│       └── webrtc.dll         # WebRTC 动态库（可选）
├── WebRTCModule/              # 新增：WebRTC 集成模块
│   ├── webrtcpeer.h           # PeerConnection 封装
│   ├── webrtcpeer.cpp         # 实现
│   ├── signaling.h            # 信令适配层
│   ├── signaling.cpp          # 实现
│   ├── mediabridge.h          # 媒体桥接层（FFmpeg↔WebRTC）
│   ├── mediabridge.cpp        # 实现
│   └── WebRTCModule.pri       # 模块构建配置
├── AudioApi/                  # 已有：音频模块
├── VideoApi/                  # 已有：视频模块
├── VideoRender/               # 已有：渲染模块
├── netapi/                    # 已有：网络模块
└── VCC视频会议.pro                 # 修改：添加 WebRTC 模块
```

### 2.4 项目构建配置修改

#### 2.4.1 修改 VCC视频会议.pro 添加 WebRTC 依赖

```makefile
# 在 VCC视频会议.pro 中添加

# WebRTC 模块
include(WebRTCModule/WebRTCModule.pri)

# WebRTC 头文件路径
INCLUDEPATH += $$PWD/webrtc/include

# WebRTC 库链接
LIBS += -L$$PWD/webrtc/lib -lwebrtc

# WebRTC 依赖的系统库（Windows）
LIBS += -lwinmm -lsecur32 -liphlpapi -lstrmiids -ldmoguids
LIBS += -lmsdmo -lwmcodecdspuuid -lamstrmid

# WebRTC 依赖的系统库（Linux/服务端）
unix:LIBS += -lrt -ldl -lpthread
```

#### 2.4.2 创建 WebRTCModule.pri

```makefile
# WebRTCModule.pri

INCLUDEPATH += $$PWD
INCLUDEPATH += $$PWD/../webrtc/include

SOURCES += \
    $$PWD/webrtcpeer.cpp \
    $$PWD/signaling.cpp \
    $$PWD/mediabridge.cpp

HEADERS += \
    $$PWD/webrtcpeer.h \
    $$PWD/signaling.h \
    $$PWD/mediabridge.h
```

---

## 三、核心功能模块实现步骤

### 3.1 信令服务器设计

#### 3.1.1 信令协议扩展

在 `packdef.h` 中新增 WebRTC 信令协议包类型：

```cpp
// === WebRTC 信令协议包类型 ===
#define DEF_PACK_WEBRTC_OFFER       (DEF_PACK_BASE + 20)  // SDP Offer
#define DEF_PACK_WEBRTC_ANSWER      (DEF_PACK_BASE + 21)  // SDP Answer
#define DEF_PACK_WEBRTC_ICE         (DEF_PACK_BASE + 22)  // ICE 候选
#define DEF_PACK_WEBRTC_HANGUP      (DEF_PACK_BASE + 23)  // 挂断通知

// SDP 交换包结构（Offer/Answer 共用）
typedef struct STRU_WEBRTC_SDP
{
    STRU_WEBRTC_SDP()
    {
        m_nType = DEF_PACK_WEBRTC_OFFER;
        m_userid = 0;          // 发送者ID
        m_targetId = 0;        // 接收者ID
        m_sdpLength = 0;       // SDP 内容长度
    }
    PackType m_nType;
    int m_userid;
    int m_targetId;
    int m_sdpLength;
    // 后接 char m_sdpData[]（可变长 SDP 内容，UTF-8 编码）
}STRU_WEBRTC_SDP;

// ICE 候选交换包结构
typedef struct STRU_WEBRTC_ICE
{
    STRU_WEBRTC_ICE()
    {
        m_nType = DEF_PACK_WEBRTC_ICE;
        m_userid = 0;
        m_targetId = 0;
        m_iceLength = 0;
    }
    PackType m_nType;
    int m_userid;
    int m_targetId;
    int m_iceLength;
    // 后接 char m_iceData[]（可变长 ICE 候选 JSON）
}STRU_WEBRTC_ICE;
```

#### 3.1.2 信令流程

```
Client A                    Server                    Client B
    │                          │                          │
    │  CreateRoomRq/JoinRoomRq │                          │
    ├─────────────────────────►│                          │
    │                          │  RoomMember通知          │
    │                          ├─────────────────────────►│
    │                          │                          │
    │  ① WebRTC Offer(SDP)     │                          │
    ├─────────────────────────►│─────────────────────────►│
    │                          │                          │
    │                          │  ② WebRTC Answer(SDP)    │
    │◄─────────────────────────┼──────────────────────────┤
    │                          │                          │
    │  ③ ICE Candidate (双向交换)                        │
    ├─────────────────────────►│─────────────────────────►│
    │◄─────────────────────────┼──────────────────────────┤
    │                          │                          │
    │  ④ ICE 连通性检查（P2P/TURN）                      │
    │◄══════════════════════════════════════════════════►│
    │                                                     │
    │  ⑤ DTLS 握手 + SRTP 媒体流传输                      │
    │◄══════════════════════════════════════════════════►│
```

#### 3.1.3 服务端信令转发实现

在 `clogic.cpp` 中新增信令转发函数：

```cpp
// 信令协议映射（在 ckernel::setNetPackMap 中添加）
NetPackMap(DEF_PACK_WEBRTC_OFFER)  = &ckernel::slot_dealWebRTCOffer;
NetPackMap(DEF_PACK_WEBRTC_ANSWER) = &ckernel::slot_dealWebRTCAnswer;
NetPackMap(DEF_PACK_WEBRTC_ICE)    = &ckernel::slot_dealWebRTCIce;
NetPackMap(DEF_PACK_WEBRTC_HANGUP) = &ckernel::slot_dealWebRTCHangup;

// 信令转发函数（以 Offer 为例）
void ckernel::slot_dealWebRTCOffer(uint clientfd, char* szbuf, int nlen)
{
    STRU_WEBRTC_SDP* rq = (STRU_WEBRTC_SDP*)szbuf;
    // 查找目标用户的 socket
    int targetFd = m_mapIDToFd[rq->m_targetId];
    if (targetFd > 0) {
        // 直接转发给目标用户
        SendData(targetFd, szbuf, nlen);
    }
}
```

### 3.2 WebRTC PeerConnection 封装

#### 3.2.1 WebRTCPeer 类设计

```cpp
// webrtcpeer.h
#ifndef WEBRTCPEER_H
#define WEBRTCPEER_H

#include <QObject>
#include <memory>

// WebRTC 前向声明
namespace rtc
{
    class Thread;
}
namespace webrtc
{
    class PeerConnectionFactoryInterface;
    class PeerConnectionInterface;
    class VideoTrackInterface;
    class AudioTrackInterface;
    class VideoTrackSourceInterface;
    class AudioTrackSourceInterface;
    class MediaStreamInterface;
    class DataChannelInterface;
}

class WebRTCPeer : public QObject, public sigslot::has_slots<>
{
    Q_OBJECT
public:
    explicit WebRTCPeer(QObject *parent = nullptr);
    ~WebRTCPeer();

    // 初始化 WebRTC 环境（全局调用一次）
    static bool initGlobal();

    // 创建 PeerConnection
    bool createPeerConnection(const std::string& stunServer = "");

    // 创建 Offer（呼叫方）
    void createOffer();

    // 创建 Answer（被呼叫方）
    void createAnswer(const std::string& remoteSdp);

    // 设置远程 SDP
    bool setRemoteSDP(const std::string& sdp, bool isOffer);

    // 添加 ICE 候选
    void addIceCandidate(const std::string& candidate, const std::string& sdpMid, int sdpMLineIndex);

    // 添加本地视频轨
    void addLocalVideoTrack(rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source);

    // 添加本地音频轨
    void addLocalAudioTrack(rtc::scoped_refptr<webrtc::AudioTrackSourceInterface> source);

    // 创建数据通道
    void createDataChannel(const std::string& label);

    // 关闭连接
    void close();

signals:
    // SDP 生成完成（需要通过信令发送给远端）
    void SIG_localSdpCreated(const QString& sdp, bool isOffer);
    // ICE 候选生成（需要通过信令发送给远端）
    void SIG_iceCandidate(const QString& candidate, const QString& sdpMid, int sdpMLineIndex);
    // 连接状态变化
    void SIG_connectionState(int state);
    // 接收到远端视频帧
    void SIG_remoteVideoFrame(int width, int height, const uint8_t* yData, const uint8_t* uData, const uint8_t* vData);
    // 接收到远端音频帧
    void SIG_remoteAudioFrame(const int16_t* data, int sampleRate, int channels, int samples);
    // 数据通道消息
    void SIG_dataChannelMessage(const QString& message);

private:
    // PeerConnection 事件回调
    void onSignalingChange(webrtc::PeerConnectionInterface::SignalingState state);
    void onIceCandidate(const webrtc::IceCandidateInterface* candidate);
    void onAddStream(rtc::scoped_refptr<webrtc::MediaStreamInterface> stream);
    void onRemoveStream(rtc::scoped_refptr<webrtc::MediaStreamInterface> stream);
    void onDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> channel);
    void onRenegotiationNeeded();

    // CreateSessionDescriptionObserver 回调
    void onCreateSessionDescriptionSuccess(webrtc::SessionDescriptionInterface* desc);
    void onCreateSessionDescriptionFailure(const std::string& error);
    void onSetSessionDescriptionSuccess();
    void onSetSessionDescriptionFailure(const std::string& error);

private:
    static rtc::Thread* s_signalingThread;
    static rtc::Thread* s_workerThread;
    static rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> s_factory;

    rtc::scoped_refptr<webrtc::PeerConnectionInterface> m_peerConnection;
    rtc::scoped_refptr<webrtc::VideoTrackInterface> m_localVideoTrack;
    rtc::scoped_refptr<webrtc::AudioTrackInterface> m_localAudioTrack;
    rtc::scoped_refptr<webrtc::DataChannelInterface> m_dataChannel;
};

#endif // WEBRTCPEER_H
```

#### 3.2.2 关键实现

```cpp
// webrtcpeer.cpp（核心片段）

// 全局初始化
rtc::Thread* WebRTCPeer::s_signalingThread = nullptr;
rtc::Thread* WebRTCPeer::s_workerThread = nullptr;
rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> WebRTCPeer::s_factory = nullptr;

bool WebRTCPeer::initGlobal()
{
    if (s_factory) return true;

    // 创建 WebRTC 线程
    s_signalingThread = rtc::Thread::Create().release();
    s_signalingThread->Start();
    s_workerThread = rtc::Thread::Create().release();
    s_workerThread->Start();

    // 创建 PeerConnectionFactory
    s_factory = webrtc::CreatePeerConnectionFactory(
        s_workerThread, s_workerThread, s_signalingThread,
        nullptr,  // 默认音频设备模块
        webrtc::CreateBuiltinAudioEncoderFactory(),
        webrtc::CreateBuiltinAudioDecoderFactory(),
        webrtc::CreateBuiltinVideoEncoderFactory(),
        webrtc::CreateBuiltinVideoDecoderFactory(),
        nullptr, nullptr);

    return s_factory != nullptr;
}

bool WebRTCPeer::createPeerConnection(const std::string& stunServer)
{
    webrtc::PeerConnectionInterface::RTCConfiguration config;

    // 配置 STUN 服务器
    if (!stunServer.empty()) {
        webrtc::PeerConnectionInterface::IceServer server;
        server.uri = "stun:" + stunServer;
        config.servers.push_back(server);
    }

    // 配置 TURN 服务器（NAT 穿透）
    webrtc::PeerConnectionInterface::IceServer turnServer;
    turnServer.uri = "turn:turn.example.com:3478";
    turnServer.username = "user";
    turnServer.password = "pass";
    config.servers.push_back(turnServer);

    // 启用 ICE 收集策略
    config.type = webrtc::PeerConnectionInterface::kAll;

    // 创建 PeerConnection
    m_peerConnection = s_factory->CreatePeerConnection(
        config, nullptr, nullptr, this);

    return m_peerConnection != nullptr;
}

void WebRTCPeer::createOffer()
{
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
    options.offer_to_receive_video = true;
    options.offer_to_receive_audio = true;

    auto observer = rtc::make_ref_counted<CreateSessionDescriptionObserver>(this, true);
    m_peerConnection->CreateOffer(observer, options);
}
```

### 3.3 媒体流捕获与注入

#### 3.3.1 媒体桥接层设计

由于阶段一保留 FFmpeg 编解码，需要将 FFmpeg 的 AVFrame 桥接到 WebRTC 的 VideoFrame：

```cpp
// mediabridge.h
class MediaBridge : public rtc::VideoTrackSourceInterface
{
public:
    // 从 FFmpeg AVFrame 注入视频帧到 WebRTC
    void feedVideoFrame(const QImage& image);

    // 从 SDL PCM 注入音频帧到 WebRTC
    void feedAudioFrame(const int16_t* pcm, int sampleRate, int channels, int samples);

    // 从 WebRTC 接收远端视频帧（转发给 OpenGL 渲染）
    // 实现 SinkInterface::OnFrame
    void OnFrame(const webrtc::VideoFrame& frame) override;

private:
    rtc::VideoBroadcaster m_videoBroadcaster;
    // ...
};
```

#### 3.3.2 视频帧桥接实现

```cpp
// 将 QImage 转换为 WebRTC VideoFrame
void MediaBridge::feedVideoFrame(const QImage& image)
{
    // QImage(RGB) → I420(YYYY...UU...VV...)
    int width = image.width();
    int height = image.height();

    // 使用 libyuv 进行 RGB→I420 转换
    rtc::scoped_refptr<webrtc::I420Buffer> buffer =
        webrtc::I420Buffer::Create(width, height);

    libyuv::ARGBToI420(
        image.constBits(), image.bytesPerLine(),
        buffer->MutableDataY(), buffer->StrideY(),
        buffer->MutableDataU(), buffer->StrideU(),
        buffer->MutableDataV(), buffer->StrideV(),
        width, height);

    // 创建 VideoFrame 并广播
    webrtc::VideoFrame frame(buffer, webrtc::VideoRotation::kVideoRotation_0,
                             rtc::TimeMicros());
    m_videoBroadcaster.OnFrame(frame);
}
```

#### 3.3.3 音频帧桥接实现

```cpp
// 将 SDL PCM 注入 WebRTC 音频源
void MediaBridge::feedAudioFrame(const int16_t* pcm, int sampleRate,
                                  int channels, int samples)
{
    // 创建 WebRTC AudioFrame
    auto audioFrame = std::make_unique<webrtc::AudioFrame>();
    audioFrame->UpdateFrame(0, pcm, samples, sampleRate,
                            webrtc::AudioFrame::SpeechType::kNormalSpeech,
                            webrtc::AudioFrame::VADActivity::kVadActive,
                            channels);

    // 注入到 WebRTC 音频处理管道
    m_audioSource->OnAudioData(audioFrame.get());
}
```

### 3.4 P2P 连接建立流程

#### 3.4.1 完整连接建立时序

```
┌─────────┐                                          ┌─────────┐
│ Client A │                                          │ Client B │
│ (呼叫方) │                                          │(被呼叫方)│
└────┬────┘                                          └────┬────┘
     │                                                    │
     │ 1. createPeerConnection()                         │
     │ 2. addLocalVideoTrack()                           │
     │    addLocalAudioTrack()                           │
     │ 3. createOffer()                                  │
     │                                                    │
     │ 4. OnCreateSessionDescriptionSuccess(Offer SDP)   │
     │ 5. setLocalDescription(Offer)                     │
     │                                                    │
     │ 6. SIG_localSdpCreated(Offer)                     │
     ├───────────────[通过信令服务器转发]──────────────►│
     │                                                    │
     │                              7. setRemoteSDP(Offer)│
     │                              8. createAnswer()     │
     │                                                    │
     │        9. SIG_localSdpCreated(Answer)              │
     │◄───────────────[通过信令服务器转发]──────────────┤
     │                                                    │
     │ 10. setRemoteSDP(Answer)                          │
     │                                                    │
     │ 11. ICE 候选收集（双向并行）                      │
     │     OnIceCandidate(candidate)                     │
     │ 12. SIG_iceCandidate(candidate)                   │
     ├───────────────[通过信令服务器转发]──────────────►│
     │◄───────────────[通过信令服务器转发]──────────────┤
     │                                                    │
     │ 13. addIceCandidate(candidate)                    │
     │                              13. addIceCandidate()│
     │                                                    │
     │ 14. ICE 连通性检查（STUN binding）                │
     │◄════════════════════════════════════════════════►│
     │                                                    │
     │ 15. DTLS 握手                                     │
     │◄════════════════════════════════════════════════►│
     │                                                    │
     │ 16. SRTP 媒体流传输（双向）                       │
     │◄════════════════════════════════════════════════►│
     │                                                    │
```

#### 3.4.2 客户端集成代码

在 `ckernel.cpp` 中集成 WebRTC 连接管理：

```cpp
// ckernel.h 新增成员
#include "webrtcpeer.h"
#include "mediabridge.h"

class ckernel : public QObject
{
    // ...
private:
    std::map<int, WebRTCPeer*> m_mapIDToPeer;  // userId → WebRTCPeer
    MediaBridge* m_mediaBridge;                 // 媒体桥接层
};

// ckernel.cpp 新增：房间成员加入时创建 WebRTC 连接
void ckernel::slot_dealRoomMemberRq(uint clientfd, char* szbuf, int nlen)
{
    // 原有逻辑：更新房间成员列表...

    // 新增：为新成员创建 WebRTC 连接
    int newMemberId = /* 从协议包获取 */;
    if (m_mapIDToPeer.find(newMemberId) == m_mapIDToPeer.end()) {
        WebRTCPeer* peer = new WebRTCPeer(this);
        peer->createPeerConnection("stun:stun.l.google.com:19302");

        // 添加本地媒体轨
        peer->addLocalVideoTrack(m_mediaBridge->videoSource());
        peer->addLocalAudioTrack(m_mediaBridge->audioSource());

        // 连接信号
        connect(peer, &WebRTCPeer::SIG_localSdpCreated,
                this, &ckernel::slot_sendWebRTCSdp);
        connect(peer, &WebRTCPeer::SIG_iceCandidate,
                this, &ckernel::slot_sendWebRTCIce);
        connect(peer, &WebRTCPeer::SIG_remoteVideoFrame,
                this, &ckernel::slot_renderRemoteVideo);
        connect(peer, &WebRTCPeer::SIG_remoteAudioFrame,
                this, &ckernel::slot_playRemoteAudio);

        m_mapIDToPeer[newMemberId] = peer;

        // 作为呼叫方，发起 Offer
        peer->createOffer();
    }
}
```

### 3.5 NAT 穿透方案

#### 3.5.1 ICE 架构

```
                    ┌─────────────┐
                    │  STUN Server │
                    │ (公网IP:3478)│
                    └──────┬──────┘
                           │
        ┌──────────────────┼──────────────────┐
        │                  │                  │
   ┌────┴────┐       ┌────┴────┐       ┌────┴────┐
   │ Client A │       │ Client B │       │ Client C │
   │ (NAT后)  │       │ (公网)   │       │ (对称NAT)│
   └────┬────┘       └────┬────┘       └────┬────┘
        │                  │                  │
        │    ┌─────────────┴──────────────┐  │
        │    │      TURN Server           │  │
        │    │   (公网中继，备用方案)       │  │
        │    └────────────────────────────┘  │
        │                                     │
        └───────────TURN中继─────────────────┘
```

#### 3.5.2 STUN/TURN 服务器部署

**STUN 服务器**（使用 coturn，轻量级）：

```bash
# Ubuntu 安装 coturn
sudo apt install coturn

# 配置 /etc/turnserver.conf
listening-port=3478
listening-ip=0.0.0.0
external-ip=<公网IP>
relay-ip=<公网IP>
min-port=49152
max-port=65535
fingerprint
lt-cred-mech
user=webrtc:webrtc123
realm=vcc-video-conference.local
no-cli
```

```bash
# 启动 STUN/TURN 服务器
turnserver -c /etc/turnserver.conf --daemon
```

#### 3.5.3 ICE 候选类型

| 类型 | 描述 | 延迟 | 成功率 | 优先级 |
|------|------|------|--------|--------|
| **Host** | 本机 IP | 最低 | 仅局域网 | 最高 |
| **Server Reflexive (srflx)** | STUN 反射公网 IP | 低 | NAT 后可用 | 高 |
| **Peer Reflexive (prflx)** | 对端反射 IP | 低 | 偶发 | 中 |
| **Relay** | TURN 中继 | 最高 | 100% | 最低 |

ICE 会按优先级尝试所有候选，选择最优路径。

### 3.6 SFU 媒体服务器（多人房间）

#### 3.6.1 mediasoup 部署（阶段三）

```bash
# 安装 Node.js（mediasoup 依赖）
curl -fsSL https://deb.nodesource.com/setup_18.x | sudo -E bash -
sudo apt install -y nodejs

# 创建 mediasoup 应用
mkdir mediasoup-server
cd mediasoup-server
npm init -y
npm install mediasoup

# 创建 server.js
cat > server.js << 'EOF'
const mediasoup = require('mediasoup');

async function run() {
    const worker = await mediasoup.createWorker({
        logLevel: 'warn',
        rtcMinPort: 40000,
        rtcMaxPort: 49999,
    });

    const router = await worker.createRouter({
        mediaCodecs: [
            { kind: 'audio', mimeType: 'audio/opus', clockRate: 48000, channels: 2 },
            { kind: 'video', mimeType: 'video/H264', clockRate: 90000,
              parameters: { 'packetization-mode': 1, 'profile-level-id': '42e01f' } },
        ],
    });

    console.log('mediasoup SFU running, router ID:', router.id);
}
run();
EOF

node server.js
```

#### 3.6.2 SFU 转发架构

```
              ┌──────────────────────────────┐
              │        SFU Server            │
              │       (mediasoup)            │
              │                              │
  ┌───────────┤  Router                      ├───────────┐
  │           │  ├── Producer(A) [视频+音频]  │           │
  │           │  ├── Producer(B)              │           │
  │           │  ├── Producer(C)              │           │
  │           │  │                            │           │
  │  Push     │  ├── Consumer→A [B+C的流]     │    Pull   │
  │  媒体流    │  ├── Consumer→B [A+C的流]     │   媒体流  │
  │           │  └── Consumer→C [A+B的流]     │           │
  └───────────┤                              ├───────────┘
              └──────────────────────────────┘
         Client A                           Client B/C
```

---

## 四、性能优化策略

### 4.1 网络带宽优化

| 优化策略 | 技术手段 | 预期效果 | 实施阶段 |
|----------|----------|----------|----------|
| **GCC 拥塞控制** | WebRTC 内置 Google Congestion Control | 自适应带宽，避免拥塞 | 阶段一 |
| **SVC 分层编码** | H.264 + SVC（Temporal） | 低带宽用户收基础层，高带宽收增强层 | 阶段二 |
| **Simulcast** | 同时发送多分辨率流（720p/360p/180p） | SFU 按需选择最优层 | 阶段三 |
| **NACK 重传** | WebRTC 内置 NACK 丢包重传 | 丢包率 < 5% 时无画质影响 | 阶段一 |
| **FEC 前向纠错** | WebRTC 内置 FEC | 丢包率 5-15% 时维持画质 | 阶段一 |
| **带宽估计** | WebRTC TransportCC | 精确估计可用带宽，动态调整码率 | 阶段一 |

### 4.2 延迟优化

| 优化策略 | 当前延迟 | 目标延迟 | 技术手段 |
|----------|----------|----------|----------|
| **传输层** | 300-500ms (TCP) | 100-200ms (RTP/UDP) | UDP 替代 TCP，避免队头阻塞 |
| **编码延迟** | 50-100ms (FFmpeg) | 20-50ms (WebRTC 内置) | 使用 WebRTC 内置编码器，低延迟 preset |
| **抖动缓冲** | 无 | 20-50ms | WebRTC NetEQ（音频）+ JitterBuffer（视频） |
| **采集延迟** | 30-50ms (SDL/OpenCV) | 10-30ms | 直接使用 WebRTC 设备采集 |
| **渲染延迟** | 16ms (OpenGL) | 16ms (不变) | 保持 OpenGL 渲染 |
| **总端到端** | 400-700ms | 150-300ms | 综合优化 |

### 4.3 音视频质量优化

| 优化项 | 当前方案 | WebRTC 方案 | 改进效果 |
|--------|----------|-------------|----------|
| **回声消除** | 无 AEC | WebRTC AEC3 | 回声残留 < -30dB |
| **降噪** | 无 | WebRTC NS | 信噪比提升 15dB |
| **自动增益** | 无 | WebRTC AGC | 音量稳定，避免忽大忽小 |
| **丢包补偿** | 无 | WebRTC PLC + RED | 丢包率 10% 时仍可懂 |
| **视频抖动** | 无缓冲 | WebRTC JitterBuffer | 画面平滑，无卡顿 |
| **码率自适应** | 固定码率 | WebRTC GCC 动态码率 | 弱网自动降质保流畅 |

### 4.4 拥塞控制配置

```cpp
// 配置 WebRTC 拥塞控制
webrtc::PeerConnectionInterface::RTCConfiguration config;

// 启用 TransportCC（传输层拥塞控制）
config.enable_rtp_transport_cc = true;

// 设置带宽限制
config.max_bitrate_bps = 3000000;      // 最大 3Mbps（摄像头）
config.max_bitrate_bps_screen = 4000000; // 最大 4Mbps（屏幕共享）

// 配置抖动缓冲策略
config.screencast_min_bitrate_bps = 800000;

// 启用 NACK
config.enable_ice Renomination = true;
```

---

## 五、测试计划

### 5.1 单元测试

| 测试模块 | 测试项 | 测试方法 | 验收标准 |
|----------|--------|----------|----------|
| **WebRTCPeer** | PeerConnection 创建 | 调用 createPeerConnection() | 返回 true，无崩溃 |
| **WebRTCPeer** | Offer/Answer 生成 | createOffer() / createAnswer() | SDP 非空，包含 video/audio m-line |
| **MediaBridge** | QImage→I420 转换 | feedVideoFrame(QImage) | I420Buffer 分辨率正确 |
| **MediaBridge** | PCM→AudioFrame 转换 | feedAudioFrame(PCM) | AudioFrame 采样率/通道正确 |
| **Signaling** | SDP 转发 | 模拟 SDP 包 | 服务端正确转发到目标用户 |
| **Signaling** | ICE 转发 | 模拟 ICE 包 | 服务端正确转发到目标用户 |

### 5.2 集成测试

| 测试场景 | 测试步骤 | 预期结果 |
|----------|----------|----------|
| **2人通话（局域网）** | A/B 加入房间，开启音视频 | 双方音视频正常，延迟 < 200ms |
| **2人通话（公网）** | A/B 在不同网络，通过 STUN | ICE 成功穿透，音视频正常 |
| **4人会议** | A/B/C/D 加入同一房间 | 每人收到其他3人的音视频 |
| **屏幕共享** | A 开启屏幕共享 | B/C/D 看到 A 的屏幕，文字清晰 |
| **弱网测试** | 限制带宽 1Mbps，丢包 10% | 视频降质但不断流，音频可懂 |
| **NAT 穿透** | A 在对称 NAT 后 | TURN 中继生效，连接成功 |
| **断线重连** | A 断网后恢复 | 30秒内自动重连，恢复音视频 |
| **信令兼容** | 新旧客户端混用 | 旧客户端走 TCP，新客户端走 WebRTC |

### 5.3 性能测试

| 测试指标 | 测试工具 | 目标值 | 当前值 |
|----------|----------|--------|--------|
| **端到端延迟** | Wireshark + 时间戳 | < 200ms | 400-700ms |
| **视频帧率** | Chrome webrtc-internals | ≥ 25fps | 15-20fps |
| **带宽利用率** | nethogs / iftop | < 3Mbps(摄像头) / 4Mbps(屏幕) | 5-8Mbps |
| **CPU 占用** | 任务管理器 | < 40% (4人会议) | 60-80% |
| **内存占用** | 任务管理器 | < 300MB | 200-400MB |
| **丢包恢复** | Chrome webrtc-internals | 10% 丢包无明显卡顿 | 无恢复机制 |
| **ICE 建连时间** | 日志时间戳 | < 3秒 | N/A（TCP 直连） |

### 5.4 兼容性测试

| 测试维度 | 测试环境 | 验收标准 |
|----------|----------|----------|
| **操作系统** | Windows 10/11、Ubuntu 22.04 | 功能正常 |
| **编译器** | MinGW 11+、GCC 11+ | 编译通过 |
| **网络环境** | 局域网、公网、VPN、4G/5G | ICE 穿透成功 |
| **NAT 类型** | Full Cone、Restricted、Symmetric | STUN 或 TURN 成功 |
| **防火墙** | 仅开放 80/443 端口 | TURN over TLS (443) 成功 |
| **摄像头** | 720p/1080p USB 摄像头 | 采集正常 |
| **浏览器互通** | Chrome 120+ WebRTC | SDP 互通（可选） |

---

## 六、项目时间轴与资源分配

### 6.1 阶段划分

| 阶段 | 名称 | 目标 | 预估工时 | 依赖 |
|------|------|------|----------|------|
| **阶段一** | 网络层替换 | TCP → WebRTC PeerConnection，P2P 通信 | 6-8 周 | 无 |
| **阶段二** | 媒体引擎替换 | FFmpeg → WebRTC 内置编解码 + AEC3 | 4-6 周 | 阶段一 |
| **阶段三** | SFU 部署 | mediasoup SFU，支持 50+ 人 | 4-6 周 | 阶段二 |
| **总计** | — | — | 14-20 周 | — |

### 6.2 详细任务分解

#### 阶段一：网络层替换（6-8 周）

| 周次 | 任务 | 交付物 | 负责人 |
|------|------|--------|--------|
| W1 | WebRTC 预编译库获取、环境搭建 | webrtc/include + webrtc/lib | 开发 |
| W2 | WebRTCPeer 类封装、initGlobal/createPeerConnection | webrtcpeer.h/.cpp | 开发 |
| W3 | 信令协议扩展（packdef.h 新增4个包类型） | packdef.h 修改 | 开发 |
| W3 | 服务端信令转发实现 | clogic.cpp 修改 | 开发 |
| W4 | MediaBridge 媒体桥接层（QImage→I420, PCM→AudioFrame） | mediabridge.h/.cpp | 开发 |
| W5 | ckernel 集成 WebRTC（连接管理、信号绑定） | ckernel.cpp 修改 | 开发 |
| W6 | STUN/TURN 服务器部署（coturn） | turnserver 配置 | 运维 |
| W6 | P2P 连接测试（局域网 + 公网） | 测试报告 | 测试 |
| W7 | ICE 穿透调试、弱网优化 | 优化报告 | 开发 |
| W8 | 阶段一集成测试、回归测试 | 测试报告 | 测试 |

#### 阶段二：媒体引擎替换（4-6 周）

| 周次 | 任务 | 交付物 | 负责人 |
|------|------|--------|--------|
| W9 | WebRTC 视频采集替换 OpenCV | VideoTrackSource 实现 | 开发 |
| W10 | WebRTC 音频采集替换 SDL | AudioDeviceModule 实现 | 开发 |
| W10 | WebRTC AEC3/NS/AGC 集成 | 音频处理管道 | 开发 |
| W11 | WebRTC 视频渲染对接 OpenGL | VideoSinkInterface 实现 | 开发 |
| W12 | Simulcast 多分辨率编码 | 编码配置 | 开发 |
| W13 | 阶段二集成测试、音质/画质评估 | 测试报告 | 测试 |
| W14 | 性能优化、回归测试 | 优化报告 | 开发/测试 |

#### 阶段三：SFU 部署（4-6 周）

| 周次 | 任务 | 交付物 | 负责人 |
|------|------|--------|--------|
| W15 | mediasoup 服务器部署 | SFU 服务 | 运维 |
| W16 | SFU 信令协议设计（Producer/Consumer） | 信令文档 | 开发 |
| W17 | 客户端 SFU 对接（mediasoup-client） | 客户端集成 | 开发 |
| W18 | 多人房间测试（10/30/50人） | 测试报告 | 测试 |
| W19 | 录制功能（可选） | MediaRecorder | 开发 |
| W20 | 阶段三集成测试、全量回归 | 测试报告 | 测试 |

### 6.3 资源分配

| 资源 | 需求 | 说明 |
|------|------|------|
| **开发人员** | 1-2 人 | 需掌握 C++/WebRTC/Qt/FFmpeg |
| **测试人员** | 1 人 | 负责功能/性能/兼容性测试 |
| **运维人员** | 1 人（兼职） | STUN/TURN/SFU 部署 |
| **开发环境** | Windows + Linux | 客户端 Windows，服务端 Linux |
| **服务器** | 1 台公网 VPS | STUN/TURN/SFU 部署（2核4G起） |
| **测试设备** | 4 台 PC + 多种网络环境 | 兼容性测试 |

---

## 七、风险评估与应对措施

### 7.1 技术风险

| 风险项 | 概率 | 影响 | 应对措施 |
|--------|------|------|----------|
| **WebRTC 预编译库与 MinGW 不兼容** | 中 | 高 | 优先测试预编译库兼容性；备选方案：用 MSVC 编译 WebRTC，或自行用 MinGW 编译 |
| **FFmpeg→WebRTC 媒体桥接性能差** | 中 | 中 | 使用 libyuv 加速格式转换；减少内存拷贝；阶段二直接用 WebRTC 采集 |
| **ICE 穿透失败（对称NAT）** | 中 | 高 | 部署 TURN 中继服务器作为兜底；TURN 覆盖率接近 100% |
| **WebRTC 线程模型与 Qt 冲突** | 中 | 中 | WebRTC 使用独立线程，通过信号槽与 Qt 主线程通信，避免直接跨线程访问 |
| **编译产物过大** | 低 | 中 | 裁剪 WebRTC 不需要的模块（如 BoringSSL、libsrtp 独立编译） |

### 7.2 兼容性风险

| 风险项 | 概率 | 影响 | 应对措施 |
|--------|------|------|----------|
| **新旧客户端协议不兼容** | 高 | 高 | 客户端版本协商：新客户端先尝试 WebRTC，失败回退 TCP；服务端同时支持两种协议 |
| **32 位进程内存限制** | 中 | 中 | WebRTC 内存占用较大，32 位进程可能不足；考虑迁移 64 位编译 |
| **防火墙阻断 UDP** | 中 | 高 | TURN over TCP (443端口) 作为兜底；支持 ICE-TCP 候选 |
| **不同 WebRTC 版本 SDP 不兼容** | 低 | 中 | 使用稳定的 WebRTC 分支（M126 LTS）；SDP 兼容性测试 |

### 7.3 性能风险

| 风险项 | 概率 | 影响 | 应对措施 |
|--------|------|------|----------|
| **多人会议 CPU 过高** | 中 | 高 | 阶段三 SFU 减轻客户端编码压力；限制 Simulcast 层数 |
| **弱网下视频卡顿** | 中 | 中 | GCC 动态降码率；SVC 基础层优先；NACK+FEC 丢包恢复 |
| **内存泄漏** | 中 | 高 | WebRTC 使用引用计数（scoped_refptr），定期用 ASan/Valgrind 检测 |
| **TURN 中继带宽成本** | 中 | 中 | 仅在 ICE 穿透失败时使用 TURN；优化 TURN 服务器带宽配额 |

### 7.4 应急回退方案

| 场景 | 回退方案 |
|------|----------|
| **WebRTC 集成失败** | 保留原有 TCP 传输，回退到 HD 升级版本 |
| **阶段一中发现严重问题** | 禁用 WebRTC，代码中通过 `#define USE_WEBRTC 0` 回退 TCP |
| **SFU 部署失败** | 回退到 Mesh P2P（≤4人）或现有 TCP 转发 |
| **TURN 服务器故障** | 局域网内仍可直连（Host 候选）；公网降级为 STUN |

---

## 八、基于 WebRTC 的静音检测功能实现

### 8.1 实现原理

静音检测（Voice Activity Detection，VAD）是视频会议系统中降低带宽占用、提升用户体验的关键技术。本方案基于 WebRTC 内置的 VAD 模块实现静音检测功能，并与 Opus 编码器的 DTX（Discontinuous Transmission）机制协同工作，实现双重静音压缩。

**核心技术栈：**

| 组件 | 技术 | 作用 |
|------|------|------|
| 音频采集 | SDL2 | 跨平台音频设备访问，回调式采集 |
| 静音检测 | WebRTC VAD（`webrtc_vad`） | 基于 GMM 模型的语音/非语音分类 |
| 音频编码 | Opus Codec | 启用 DTX，静音帧压缩至 ~1kbps |
| 状态通知 | Qt Signal/Slot | 跨线程传递静音状态变化事件 |

**工作流程概览：**

```
麦克风 → SDL采集(48kHz/16bit/Mono) → WebRTC VAD检测
                                          ↓
                            ┌─────────────┴─────────────┐
                            ↓                           ↓
                       静音帧                      有声帧
                       清零处理                    原样保留
                            ↓                           ↓
                       Opus编码(DTX压缩)         Opus编码(正常编码)
                            ↓                           ↓
                       ~1kbps 输出                32kbps 输出
                            ↓                           ↓
                            └───────────┬───────────────┘
                                        ↓
                                  Qt信号 → ckernel
                                        ↓
                                  TCP 网络发送
```

**VAD 算法原理：**

WebRTC VAD 采用基于高斯混合模型（GMM）的统计判决方法，主要步骤包括：

1. **分帧加窗**：将 PCM 数据按 10/20/30ms 分帧，施加汉宁窗减少频谱泄漏
2. **子带能量计算**：通过 6 个子带滤波器组计算各频段能量（80-250Hz、250-500Hz、500-1000Hz、1000-2000Hz、2000-3000Hz、3000-4000Hz）
3. **特征提取**：提取子带能量、谱平坦度、谱重心等特征
4. **GMM 判决**：对每个子带用两个高斯分布（语音/噪声）建模，计算似然比
5. **阈值比较**：将似然比与模式相关的阈值比较，输出语音/非语音判决

### 8.2 音频流捕获与处理流程

#### 8.2.1 采集参数配置

项目采用 SDL2 进行音频采集，参数与 Opus 编码器原生优化参数对齐：

```cpp
// SDL 采集参数配置（位于 audioread.cpp::start() 函数）
SDL_AudioSpec desiredSpec;
SDL_AudioSpec obtainedSpec;
SDL_zero(desiredSpec);

// 采集参数：48kHz/16bit/单声道/960采样（20ms）
desiredSpec.freq = 48000;              // 采样率 48kHz（Opus 原生优化速率）
desiredSpec.format = AUDIO_S16LSB;     // 16位有符号小端序
desiredSpec.channels = 1;              // 单声道（语音通话标准配置）
desiredSpec.samples = 960;             // 每次回调 960 采样（20ms @ 48kHz）
desiredSpec.callback = audioCallback;  // 采集回调函数
desiredSpec.userdata = this;           // 传递 AudioRead 实例指针

// 打开 SDL 采集设备（iscapture=1 表示录音）
m_sdlCaptureDev = SDL_OpenAudioDevice(NULL, 1,
                            &desiredSpec, &obtainedSpec, 0);
```

**参数说明：**

| 参数 | 值 | 说明 |
|------|------|------|
| `freq` | 48000 Hz | Opus 编码器原生优化采样率，避免重采样开销 |
| `format` | AUDIO_S16LSB | 16位有符号小端序，匹配 Opus 编码器输入格式 |
| `channels` | 1 | 单声道，语音通话标准配置，降低带宽占用 |
| `samples` | 960 | 每帧 960 采样 = 20ms @ 48kHz，符合 Opus 标准帧长 |

#### 8.2.2 VAD 初始化

在 `AudioRead` 构造函数中初始化 WebRTC VAD 实例：

```cpp
// WebRTC VAD 静音检测初始化（位于 audioread.cpp 构造函数）
// 与 Opus DTX 双重保障：VAD 在编码前清零静音帧，DTX 在编码时压缩静音帧
#ifdef USE_VAD
    if (0 != WebRtcVad_Create(&handle)) {
        qDebug() << "[VAD] vad create fail";
    }
    if (0 != WebRtcVad_Init(handle)) {
        qDebug() << "[VAD] vad init fail";
    }
    // 设置 VAD 模式（0-3，模式越高越严格）
    // 模式 0: 宽松（Quality模式），容易判定为有声
    // 模式 1: 中等偏低（Bitrate模式）
    // 模式 2: 中等偏高（推荐用于VoIP）
    // 模式 3: 严格（Aggressive模式），容易判定为静音
    WebRtcVad_set_mode(handle, 2);  // 中等灵敏度，平衡误判率
#endif
```

#### 8.2.3 音频回调处理流程

SDL 音频线程每 20ms 触发一次回调，完成 VAD 检测和 Opus 编码：

```cpp
// SDL 采集回调函数（位于 audioread.cpp::audioCallback）
// 线程安全：此函数在 SDL 音频线程中调用，通过 Qt 信号跨线程传递数据
void AudioRead::audioCallback(void *userdata, Uint8 *stream, int len)
{
    AudioRead *audio = (AudioRead *)userdata;

    // 校验数据长度：960采样 × 2字节 = 1920字节
    if (len < 1920) return;

    //  WebRTC VAD 静音检测
    // 每 960 采样检测一次，返回 0 表示静音，1 表示有声
    // 静音帧清零，减少 Opus 编码的无效数据
#ifdef USE_VAD
    if (0 == WebRtcVad_Process(audio->handle, 48000, (int16_t*)stream, 960))
    {
        // 静音帧清零（Opus DTX 会进一步压缩静音帧）
        memset(stream, 0, len);
    }
#endif

    //  Opus 编码
    // 输入：960 采样 × 2字节 = 1920字节 PCM（20ms @ 48kHz）
    // 输出：约 60-120 字节 Opus 数据（取决于内容和 DTX 状态）
    unsigned char opusData[4096];
    int nbBytes = opus_encode(audio->m_opusEncoder,
                              (const opus_int16*)stream,
                              960,  // 帧大小：960采样（20ms @ 48kHz）
                              opusData,
                              sizeof(opusData));
    if (nbBytes < 0) {
        qDebug() << "[Opus] 编码错误:" << opus_strerror(nbBytes);
        return;
    }

    //  通过 Qt 信号跨线程传递到 ckernel
    // Qt 信号槽机制自动处理线程安全（QueuedConnection）
    // ckernel::slot_audioFrame 负责序列化协议包并 TCP 发送
    QByteArray sendBuffer((char*)opusData, nbBytes);
    Q_EMIT audio->SIG_audioFrame(sendBuffer);
}
```

#### 8.2.4 Opus 编码器配置

Opus 编码器配置启用 DTX 和 FEC，与 VAD 协同工作：

```cpp
// Opus 编码器初始化（位于 audioread.cpp 构造函数）
int err;
m_opusEncoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
if (err != OPUS_OK || !m_opusEncoder) {
    qDebug() << "[Opus] 编码器创建失败:" << opus_strerror(err);
} else {
    // 设置比特率为 32kbps（宽带级音质，平衡带宽和质量）
    opus_encoder_ctl(m_opusEncoder, OPUS_SET_BITRATE(32000));
    // 启用 DTX（不连续传输），静音时带宽降至 ~1kbps
    opus_encoder_ctl(m_opusEncoder, OPUS_SET_DTX(1));
    // 启用 FEC（前向纠错），提升抗丢包能力
    opus_encoder_ctl(m_opusEncoder, OPUS_SET_INBAND_FEC(1));
    // 设置复杂度为 5（平衡 CPU 占用和音质，范围 0-10）
    opus_encoder_ctl(m_opusEncoder, OPUS_SET_COMPLEXITY(5));
    qDebug() << "[Opus] 编码器创建成功, 48kHz/VOIP/DTX/FEC/32kbps";
}
```

**Opus 编码参数说明：**

| 参数 | 值 | 说明 |
|------|------|------|
| 采样率 | 48000 Hz | Opus 原生优化速率，支持 8/12/16/24/48kHz |
| 声道数 | 1 | 单声道，语音通话标准 |
| 应用模式 | OPUS_APPLICATION_VOIP | 针对语音通话优化，内置 DTX 和 PLC |
| 比特率 | 32000 bps | 宽带级音质，平衡带宽和质量 |
| DTX | 启用 | 静音时带宽降至 ~1kbps |
| FEC | 启用 | 前向纠错，提升抗丢包能力 |
| 复杂度 | 5 | CPU 占用与音质平衡（0-10） |

### 8.3 音量阈值设定与动态调整机制

#### 8.3.1 VAD 灵敏度模式

WebRTC VAD 提供四种灵敏度模式，通过 `WebRtcVad_set_mode()` 函数设置：

```cpp
// VAD 模式设置 API
int WebRtcVad_set_mode(VadHandle handle, int mode);
```

| 模式值 | 名称 | 阈值特点 | 适用场景 | 误判率 |
|--------|------|----------|----------|--------|
| 0 | 宽松（Quality） | 低阈值，易判有声 | 高质量音频、音乐场景 | 误判有声率高 |
| 1 | 中等偏低（Bitrate） | 较低阈值 | 一般 VoIP 场景 | 平衡 |
| 2 | 中等偏高（VoIP） | 中等阈值 | **VoIP 通话（推荐）** | 平衡 |
| 3 | 严格（Aggressive） | 高阈值，易判静音 | 嘈杂环境、低带宽 | 误判静音率高 |

**本项目默认采用模式 2（中等偏高），适用于大多数 VoIP 通话场景。**

#### 8.3.2 动态阈值调整机制

为适应不同环境噪声，本方案设计了基于环境噪声估计的动态阈值调整机制：

```cpp
// ===== 动态 VAD 阈值调整实现 =====
// 在 AudioRead 类中添加以下成员变量和方法

// audioread.h 中添加
class AudioRead : public QObject
{
    // ... 原有代码 ...

private:
    // VAD 动态调整相关成员
    int m_vadMode;                  // 当前 VAD 模式（0-3）
    float m_noiseLevel;             // 环境噪声估计值（RMS 能量）
    float m_noiseLevelEMA;          // 噪声水平的指数移动平均（EMA）
    int m_consecutiveSilentFrames;  // 连续静音帧计数
    int m_consecutiveSpeechFrames;  // 连续有声帧计数
    QTimer *m_vadAdjustTimer;       // VAD 模式动态调整定时器

private slots:
    void slot_adjustVadMode();      // 动态调整 VAD 模式
    float calculateRMS(int16_t *data, int samples);  // 计算 RMS 能量
};

// audioread.cpp 中实现

// 构造函数中初始化
AudioRead::AudioRead(QObject *parent) : QObject(parent)
{
    // ... 原有初始化代码 ...

    m_vadMode = 2;                  // 默认中等灵敏度
    m_noiseLevel = 0.0f;
    m_noiseLevelEMA = 0.0f;         // EMA 初始化为 0
    m_consecutiveSilentFrames = 0;
    m_consecutiveSpeechFrames = 0;

    // 每 5 秒评估一次环境噪声，动态调整 VAD 模式
    m_vadAdjustTimer = new QTimer(this);
    m_vadAdjustTimer->setInterval(5000);  // 5秒间隔
    connect(m_vadAdjustTimer, &QTimer::timeout,
            this, &AudioRead::slot_adjustVadMode);
    m_vadAdjustTimer->start();
}

// 计算 PCM 数据的 RMS 能量值
float AudioRead::calculateRMS(int16_t *data, int samples)
{
    double sum = 0.0;
    for (int i = 0; i < samples; i++) {
        sum += (double)data[i] * data[i];
    }
    double rms = sqrt(sum / samples);
    // 归一化到 0.0-1.0（int16_t 最大值 32767）
    return (float)(rms / 32767.0);
}

// 动态调整 VAD 模式
void AudioRead::slot_adjustVadMode()
{
    // EMA 平滑系数：0.95 偏向历史值，避免短期波动影响
    const float EMA_ALPHA = 0.95f;

    // 更新噪声水平 EMA 估计
    // 如果当前判定为静音，则将当前 RMS 视为噪声水平
    m_noiseLevelEMA = EMA_ALPHA * m_noiseLevelEMA +
                      (1.0f - EMA_ALPHA) * m_noiseLevel;

    // 根据噪声水平动态调整 VAD 模式
    int newMode = m_vadMode;

    if (m_noiseLevelEMA < 0.02f) {
        // 噪声水平极低（安静环境），使用宽松模式，避免漏检轻声说话
        newMode = 1;
    } else if (m_noiseLevelEMA < 0.10f) {
        // 噪声水平中等（办公室环境），使用推荐模式
        newMode = 2;
    } else {
        // 噪声水平较高（嘈杂环境），使用严格模式，避免误判有声
        newMode = 3;
    }

    // 模式变化时才更新，避免频繁切换
    if (newMode != m_vadMode) {
        m_vadMode = newMode;
#ifdef USE_VAD
        WebRtcVad_set_mode(handle, m_vadMode);
#endif
        qDebug() << "[VAD] 模式调整为" << m_vadMode
                 << "噪声EMA:" << m_noiseLevelEMA;
    }
}
```

#### 8.3.3 阈值调整策略表

| 环境 | 噪声 EMA 范围 | VAD 模式 | 说明 |
|------|---------------|----------|------|
| 安静（录音棚/夜间） | < 0.02 | 1（宽松） | 避免漏检轻声说话 |
| 一般（办公室/居家） | 0.02 - 0.10 | 2（中等） | 平衡检测准确性 |
| 嘈杂（公共场所/车载） | > 0.10 | 3（严格） | 避免误判环境噪声为语音 |

### 8.4 静音状态判定逻辑与事件触发机制

#### 8.4.1 状态判定逻辑

静音状态判定基于 `WebRtcVad_Process()` 返回值，结合状态平滑算法避免频繁切换：

```cpp
// WebRtcVad_Process 返回值
// 返回 0：静音（无语音活动）
// 返回 1：有声（有语音活动）
// 返回 -1：错误（参数无效等）
int WebRtcVad_Process(VadHandle handle,
                      int fs,           // 采样率：8000/16000/32000/48000
                      const int16_t* audio_frame,
                      size_t frame_length);  // 帧长度：必须是 80/160/240/480/960 等
```

#### 8.4.2 状态平滑算法

为避免静音状态频繁切换（如说话停顿瞬间），采用状态平滑算法：

```cpp
// ===== 静音状态平滑与事件触发实现 =====

// audioread.h 中添加信号和成员
class AudioRead : public QObject
{
    Q_OBJECT
    // ... 原有代码 ...

signals:
    // 静音状态变化信号
    // isMute=true: 进入静音状态
    // isMute=false: 退出静音状态（开始说话）
    void SIG_muteStateChanged(bool isMute);

private:
    bool m_currentMuteState;        // 当前静音状态
    bool m_pendingMuteState;        // 待确认的静音状态
    int m_muteConfirmFrames;        // 状态确认帧计数
    static const int MUTE_CONFIRM_THRESHOLD = 3;   // 连续3帧确认状态切换
};

// audioread.cpp 中实现

// 构造函数中初始化
AudioRead::AudioRead(QObject *parent) : QObject(parent)
{
    // ... 原有初始化代码 ...

    m_currentMuteState = false;     // 初始状态：有声
    m_pendingMuteState = false;
    m_muteConfirmFrames = 0;
}

// 修改后的 audioCallback（增加状态平滑和事件触发）
void AudioRead::audioCallback(void *userdata, Uint8 *stream, int len)
{
    AudioRead *audio = (AudioRead *)userdata;

    if (len < 1920) return;

    bool isMute = false;

#ifdef USE_VAD
    // VAD 检测：返回 0 表示静音
    int vadResult = WebRtcVad_Process(audio->handle, 48000,
                                       (int16_t*)stream, 960);
    isMute = (vadResult == 0);

    // 计算当前帧 RMS 能量（用于动态阈值调整）
    audio->m_noiseLevel = audio->calculateRMS((int16_t*)stream, 960);

    // 状态平滑处理：连续 MUTE_CONFIRM_THRESHOLD 帧确认才切换状态
    if (isMute != audio->m_pendingMuteState) {
        // 待确认状态变化，重置计数器
        audio->m_pendingMuteState = isMute;
        audio->m_muteConfirmFrames = 1;
    } else {
        // 待确认状态延续，增加计数
        audio->m_muteConfirmFrames++;
        // 达到确认阈值且状态确实变化，触发事件
        if (audio->m_muteConfirmFrames >= MUTE_CONFIRM_THRESHOLD &&
            audio->m_pendingMuteState != audio->m_currentMuteState) {
            audio->m_currentMuteState = audio->m_pendingMuteState;
            Q_EMIT audio->SIG_muteStateChanged(audio->m_currentMuteState);
            qDebug() << "[VAD] 静音状态切换:" << audio->m_currentMuteState;
        }
    }

    // 静音帧清零（Opus DTX 会进一步压缩静音帧）
    if (isMute) {
        memset(stream, 0, len);
    }
#endif

    // Opus 编码（原逻辑不变）
    unsigned char opusData[4096];
    int nbBytes = opus_encode(audio->m_opusEncoder,
                              (const opus_int16*)stream, 960,
                              opusData, sizeof(opusData));
    if (nbBytes < 0) return;

    QByteArray sendBuffer((char*)opusData, nbBytes);
    Q_EMIT audio->SIG_audioFrame(sendBuffer);
}
```

#### 8.4.3 事件触发机制与 UI 联动

在 `ckernel` 中连接静音状态信号，实现 UI 联动：

```cpp
// ckernel.cpp 构造函数中添加信号连接
m_pAudioRead = new AudioRead;
connect(m_pAudioRead, SIGNAL(SIG_audioFrame(QByteArray)),
        this, SLOT(slot_audioFrame(QByteArray)));
// 新增：连接静音状态变化信号
connect(m_pAudioRead, SIGNAL(SIG_muteStateChanged(bool)),
        this, SLOT(slot_muteStateChanged(bool)));

// ckernel.h 中添加槽函数声明
private slots:
    void slot_muteStateChanged(bool isMute);

// ckernel.cpp 中实现槽函数
void ckernel::slot_muteStateChanged(bool isMute)
{
    qDebug() << "[Audio] 静音状态:" << (isMute ? "静音" : "有声");

    // 更新 UI 显示（麦克风图标状态）
    if (m_pRoomdialog) {
        // 在 RoomDialog 中更新麦克风图标
        // isMute=true: 显示静音图标（灰色麦克风）
        // isMute=false: 显示正常图标（亮色麦克风）
        m_pRoomdialog->updateMuteStatus(isMute);
    }

    // 可选：记录静音时段用于会议纪要
    if (isMute) {
        qDebug() << "[Audio] 用户进入静音状态 @"
                 << QDateTime::currentDateTime().toString("hh:mm:ss");
    } else {
        qDebug() << "[Audio] 用户退出静音状态 @"
                 << QDateTime::currentDateTime().toString("hh:mm:ss");
    }
}
```

#### 8.4.4 状态机时序图

```
时序图：静音状态切换流程

SDL音频线程          AudioRead           ckernel          RoomDialog
    │                   │                   │                 │
    │  PCM数据(20ms)    │                   │                 │
    │──────────────────>│                   │                 │
    │                   │                   │                 │
    │                   │ WebRtcVad_Process │                 │
    │                   │ (返回0=静音)      │                 │
    │                   │                   │                 │
    │                   │ 状态平滑(3帧确认) │                 │
    │                   │                   │                 │
    │                   │ 连续3帧静音       │                 │
    │                   │──SIG_muteStateChanged(true)────────>│
    │                   │                   │ slot_muteStateChanged
    │                   │                   │──updateMuteStatus──>│
    │                   │                   │                 │ 更新图标
    │                   │                   │                 │ (灰色麦克风)
    │                   │                   │                 │
    │  PCM数据(20ms)    │                   │                 │
    │──────────────────>│                   │                 │
    │                   │ WebRtcVad_Process │                 │
    │                   │ (返回1=有声)      │                 │
    │                   │                   │                 │
    │                   │ 连续3帧有声       │                 │
    │                   │──SIG_muteStateChanged(false)────────>│
    │                   │                   │ slot_muteStateChanged
    │                   │                   │──updateMuteStatus──>│
    │                   │                   │                 │ 更新图标
    │                   │                   │                 │ (亮色麦克风)
```

### 8.5 浏览器兼容性处理方案

> **说明：** 本项目为 C++ 桌面应用，使用 WebRTC Native API。以下兼容性方案适用于未来可能扩展的 Web 端（如基于 WebView 的客户端），以及不同平台（Windows/macOS/Linux）的兼容性处理。

#### 8.5.1 平台兼容性处理

```cpp
// 跨平台 VAD 初始化兼容性处理
#ifdef USE_VAD
    // Windows 平台：直接使用 WebRTC VAD
    #ifdef Q_OS_WIN
        if (0 != WebRtcVad_Create(&handle)) {
            qDebug() << "[VAD] Windows: vad create fail";
        }
    #endif

    // macOS 平台：需注意 CoreAudio 采样率转换
    #ifdef Q_OS_MAC
        if (0 != WebRtcVad_Create(&handle)) {
            qDebug() << "[VAD] macOS: vad create fail";
        }
        // macOS CoreAudio 默认 44100Hz，需重采样到 48000Hz
        // 或使用 SDL 的 obtainedSpec.freq 动态调整 VAD 采样率参数
    #endif

    // Linux 平台：需注意 ALSA/PulseAudio 差异
    #ifdef Q_OS_LINUX
        if (0 != WebRtcVad_Create(&handle)) {
            qDebug() << "[VAD] Linux: vad create fail";
        }
    #endif

    // 通用初始化
    if (0 != WebRtcVad_Init(handle)) {
        qDebug() << "[VAD] vad init fail";
    }
    WebRtcVad_set_mode(handle, 2);
#endif
```

#### 8.5.2 降级处理方案

当 VAD 不可用时（如编译未启用 `USE_VAD`），采用基于 RMS 能量的简单静音检测作为降级方案：

```cpp
// 降级方案：基于 RMS 能量的静音检测（不依赖 WebRTC VAD）
// 当 USE_VAD 未定义或 WebRtcVad_Create 失败时启用
float calculateRMS(int16_t *data, int samples)
{
    double sum = 0.0;
    for (int i = 0; i < samples; i++) {
        sum += (double)data[i] * data[i];
    }
    return (float)(sqrt(sum / samples) / 32767.0);
}

// 在 audioCallback 中使用降级检测
#ifndef USE_VAD
    // 降级方案：RMS 能量阈值检测
    float rms = calculateRMS((int16_t*)stream, 960);
    // RMS 阈值 0.01（约 -40dBFS），低于此值判定为静音
    if (rms < 0.01f) {
        memset(stream, 0, len);  // 静音帧清零
    }
#endif
```

**降级方案对比：**

| 检测方式 | 准确率 | CPU 占用 | 适用场景 |
|----------|--------|----------|----------|
| WebRTC VAD（GMM 模型） | 高（>95%） | 中等 | 正常启用 |
| RMS 能量阈值 | 中等（~85%） | 极低 | 降级/资源受限设备 |
| 过零率 + 能量双门限 | 中等（~88%） | 低 | 备用降级方案 |

#### 8.5.3 Web 端兼容性（未来扩展）

若未来扩展 Web 端客户端，使用 Web Audio API 实现 VAD：

```javascript
// Web 端 VAD 实现（基于 Web Audio API）
async function setupWebVAD(stream) {
    const audioContext = new AudioContext();
    const source = audioContext.createMediaStreamSource(stream);

    // 创建分析节点
    const analyser = audioContext.createAnalyser();
    analyser.fftSize = 2048;
    source.connect(analyser);

    const buffer = new Uint8Array(analyser.frequencyBinCount);
    const RMS_THRESHOLD = 0.01;  // RMS 阈值

    function detectMute() {
        analyser.getByteTimeDomainData(buffer);

        // 计算 RMS
        let sum = 0;
        for (let i = 0; i < buffer.length; i++) {
            const val = (buffer[i] - 128) / 128;
            sum += val * val;
        }
        const rms = Math.sqrt(sum / buffer.length);

        const isMute = rms < RMS_THRESHOLD;
        // 触发静音状态变化事件
        onMuteStateChanged(isMute);

        requestAnimationFrame(detectMute);
    }

    detectMute();
}
```

### 8.6 性能优化建议

#### 8.6.1 计算量优化

| 优化项 | 方法 | 预期收益 |
|--------|------|----------|
| 降低 VAD 帧率 | 每 2 帧（40ms）检测一次而非每帧 | CPU 占用降低 50% |
| SIMD 指令优化 | 启用 WebRTC 的 SSE/AVX 优化编译选项 | RMS 计算加速 2-4 倍 |
| 避免重复计算 | 缓存上一帧 VAD 结果，相同输入跳过 | 边际收益 |
| 帧长度优化 | 使用 30ms 帧（1440 采样）替代 20ms | 减少回调次数 33% |

```cpp
// 优化示例：每 2 帧检测一次 VAD
static int frameCounter = 0;
frameCounter++;

#ifdef USE_VAD
    // 每 2 帧（40ms）检测一次 VAD，降低 CPU 占用
    if (frameCounter % 2 == 0) {
        int vadResult = WebRtcVad_Process(audio->handle, 48000,
                                           (int16_t*)stream, 960);
        // 缓存检测结果供下一帧使用
        audio->m_lastVadResult = (vadResult == 0);
    }
    bool isMute = audio->m_lastVadResult;
    if (isMute) {
        memset(stream, 0, len);
    }
#endif
```

#### 8.6.2 线程模型优化

```cpp
// 优化建议：VAD 检测放入独立线程，避免阻塞音频回调
// 注意：当前实现中 VAD 计算量较小（<0.1ms/帧），暂不需要独立线程
// 若未来扩展为复杂 VAD（如基于深度学习），建议采用以下线程模型：

// audioread.h
class AudioRead : public QObject
{
    // ... 原有代码 ...

private:
    QThread *m_vadThread;           // VAD 检测线程
    QQueue<int16_t*> m_pcmQueue;    // PCM 数据队列
    QMutex m_queueMutex;            // 队列互斥锁
};

// audioCallback 中仅入队，不执行 VAD
void AudioRead::audioCallback(void *userdata, Uint8 *stream, int len)
{
    AudioRead *audio = (AudioRead *)userdata;

    // 将 PCM 数据复制并入队（避免阻塞音频线程）
    int16_t *pcmCopy = (int16_t*)malloc(len);
    memcpy(pcmCopy, stream, len);

    audio->m_queueMutex.lock();
    audio->m_pcmQueue.enqueue(pcmCopy);
    audio->m_queueMutex.unlock();

    // VAD 和编码在独立线程中处理
    // ... 编码逻辑 ...
}
```

#### 8.6.3 内存与带宽优化

| 优化项 | 方法 | 预期收益 |
|--------|------|----------|
| 静音帧跳过编码 | VAD 判定静音后直接发送静音标志 | 带宽降低 ~90%（静音期间） |
| DTX 压缩 | Opus 编码器 DTX 自动压缩静音帧 | 带宽从 32kbps 降至 ~1kbps |
| 内存复用 | 复用 opusData 缓冲区，避免反复分配 | 减少 GC 压力 |
| 批量发送 | 连续静音帧合并为单个数据包 | 减少网络包数量 |

```cpp
// 优化示例：静音帧跳过编码，直接发送静音标志
#ifdef USE_VAD
    if (isMute && audio->m_consecutiveSilentFrames > 5) {
        // 连续 5 帧静音后，仅发送静音标志包（2字节）
        // 而非完整的 Opus 编码包，进一步节省带宽
        uint16_t silenceFlag = 0xFFFF;  // 静音标志
        QByteArray silencePacket((char*)&silenceFlag, 2);
        Q_EMIT audio->SIG_audioFrame(silencePacket);
        return;
    }
#endif
```

#### 8.6.4 性能指标监控

```cpp
// 性能监控：记录 VAD 检测耗时和静音比例
void AudioRead::logVadPerformance()
{
    static qint64 totalFrames = 0;
    static qint64 silentFrames = 0;
    static QElapsedTimer timer;

    if (!timer.isValid()) timer.start();

    totalFrames++;
    if (m_currentMuteState) silentFrames++;

    // 每 1000 帧（约 20 秒）输出一次统计
    if (totalFrames % 1000 == 0) {
        qreal silentRatio = (qreal)silentFrames / totalFrames * 100;
        qDebug() << "[VAD] 性能统计:"
                 << "总帧数:" << totalFrames
                 << "静音比例:" << QString::number(silentRatio, 'f', 1) << "%"
                 << "运行时间:" << timer.elapsed() / 1000 << "s";
    }
}
```

### 8.7 实施步骤与验证清单

#### 8.7.1 实施步骤

| 步骤 | 任务 | 文件 | 验证方法 |
|------|------|------|----------|
| 1 | 确认 WebRTC VAD 库已链接 | VCC视频会议.pro | 编译通过，无链接错误 |
| 2 | 在 audioread.h 添加信号和成员变量 | audioread.h | 代码审查 |
| 3 | 在构造函数中初始化 VAD 和动态调整定时器 | audioread.cpp | 日志输出 "[VAD] vad init" |
| 4 | 实现 calculateRMS() 方法 | audioread.cpp | 单元测试 RMS 计算准确 |
| 5 | 实现 slot_adjustVadMode() 动态调整 | audioread.cpp | 日志输出模式切换 |
| 6 | 修改 audioCallback 增加状态平滑和事件触发 | audioread.cpp | 日志输出状态切换 |
| 7 | 在 ckernel 中连接 SIG_muteStateChanged 信号 | ckernel.cpp | 信号触发时日志输出 |
| 8 | 在 RoomDialog 中实现 updateMuteStatus() | roomdialog.cpp | UI 图标正确切换 |
| 9 | 集成测试：实际通话场景验证 | - | 静音/有声切换正确 |
| 10 | 性能测试：CPU 占用和带宽监控 | - | CPU 占用 <5%，带宽降低明显 |

#### 8.7.2 验证清单

- [ ] VAD 初始化成功，日志输出 "[VAD] vad init"
- [ ] 安静环境下说话，静音状态正确切换（有声→静音→有声）
- [ ] 嘈杂环境下，VAD 模式自动调整为 3（严格）
- [ ] 静音时网络带宽显著降低（从 32kbps 降至 ~1kbps）
- [ ] UI 麦克风图标正确反映静音状态
- [ ] CPU 占用不超过 5%（单核）
- [ ] 连续运行 1 小时无内存泄漏
- [ ] 静音状态切换延迟 < 60ms（3帧 × 20ms）

---

## 附录

### A. WebRTC 关键 API 速查

| API | 用途 | 所在头文件 |
|-----|------|-----------|
| `CreatePeerConnectionFactory()` | 创建工厂 | api/peer_connection_interface.h |
| `CreatePeerConnection()` | 创建连接 | api/peer_connection_interface.h |
| `CreateOffer()` / `CreateAnswer()` | 生成 SDP | api/peer_connection_interface.h |
| `SetLocalDescription()` / `SetRemoteDescription()` | 设置 SDP | api/peer_connection_interface.h |
| `AddIceCandidate()` | 添加 ICE 候选 | api/peer_connection_interface.h |
| `CreateVideoTrack()` / `CreateAudioTrack()` | 创建媒体轨 | api/peer_connection_interface.h |
| `CreateDataChannel()` | 创建数据通道 | api/peer_connection_interface.h |
| `I420Buffer::Create()` | 创建 I420 视频 buffer | api/video/i420_buffer.h |
| `libyuv::ARGBToI420()` | RGB→I420 转换 | libyuv.h |

### B. 调试工具

| 工具 | 用途 |
|------|------|
| `chrome://webrtc-internals` | WebRTC 内部状态（仅浏览器） |
| Wireshark + RTP 分析器 | 抓包分析 RTP/SRTP 流 |
| `turnutils_uclient` | TURN 服务器连通性测试 |
| WebRTC 日志（`RTC_LOG`) | WebRTC 内部日志 |
| `getStats()` API | 运行时统计信息 |

### C. 参考文档

- [WebRTC Native API Guide](https://webrtc.googlesource.com/src/+/refs/heads/master/docs/native-code/)
- [mediasoup 官方文档](https://mediasoup.org/documentation/)
- [coturn 配置指南](https://github.com/coturn/coturn/wiki)
- [WebRTC SDP 规范](https://datatracker.ietf.org/doc/html/rfc8829)

---

> **文档状态：** 第八章静音检测功能已实施落地
> **第八章实施情况：** 基于 WebRTC 的静音检测功能已在 ckernel/AudioRead/RoomDialog 中完整实现——包括 VAD 模式设置（构造函数 `WebRtcVad_set_mode(handle, 2)` 设为 VoIP 推荐模式 2）、动态阈值调整（QTimer 5 秒触发 `slot_adjustVadMode`，噪声 EMA α=0.95 三档自适应 1/2/3）、状态平滑（连续 3 帧确认≈60ms 防抖）、UI 联动（`SIG_muteStateChanged` → ckernel `slot_muteStateChanged` → RoomDialog `updateMuteStatus`，cb_audio 文本反馈）；编译验证通过（Qt 5.12.11 MinGW 32-bit）。
> **预估总工时：** 14-20 周（3.5-5 个月）（WebRTC 整体集成，不含已落地的第八章）
> **建议启动顺序：** 阶段一（网络层替换）→ 阶段二（媒体引擎）→ 阶段三（SFU）
> **与 P0 计划的关系：** P0（安全与基础改进）可在阶段一期间并行执行，不冲突
> **下一步：** 审查本方案 → 确认技术选型 → 搭建 WebRTC 开发环境 → 启动阶段一
>
> ---
> **【历史记录】** 以下为方案制定阶段的原始状态说明，保留备查：
>
> 原状态（2026-07-31）：方案制定完成（含第八章静音检测功能实现），待审查
> 原静音检测功能说明：第八章已包含完整实施方案，可独立于 WebRTC 整体集成先行实施（VAD 模块已集成）
> 原下一步：审查本方案 → 确认技术选型 → 搭建 WebRTC 开发环境 → 启动阶段一
