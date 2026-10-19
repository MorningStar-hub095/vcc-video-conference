# VCC 视频会议

一个用 **Qt + C++** 从零实现的桌面视频会议系统，包含自定义二进制协议、
TCP/UDP 双通道传输、音频采集编码、视频编解码与屏幕共享。

> 早期版本代号 `WeChat`，**与腾讯公司无关**，未使用其任何代码或素材，现已完成更名。

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

---

## 功能一览

| 模块 | 能力 |
|---|---|
| 用户 | 账号注册、登录、注销、邮箱补全 |
| 房间 | 创建房间、加入房间、离开房间、成员列表实时同步 |
| 音视频 | 麦克风采集与播放、摄像头采集与渲染、桌面屏幕共享 |
| 音频处理 | Opus 编码（48kHz / VOIP / 32kbps）、DTX 静音断流、WebRTC VAD 降噪 |
| 视频处理 | H.264 硬编码优先、OpenCV 人脸检测与帧质量处理 |
| 网络传输 | TCP 控制通道 + UDP 音视频通道、PLI 关键帧请求与重传 |

## 技术架构

```
┌──────────────────────────────────────────────────────────┐
│  客户端 vcc-client（Qt 5.12 / MinGW 32-bit）              │
│                                                          │
│  UI 层    vccdialog · logindialog · roomdialog · usershow  │
│      ↓                                                   │
│  内核层   ckernel（信号槽调度 / 协议编解码 / 房间状态）     │
│      ↓                                                   │
│  ────────── 网络层 netapi（自研 TCP/UDP 协议栈）────────── │
│      ↓                          ↓                        │
│  音频 AudioApi              视频 VideoApi / VideoRender    │
│  SDL2 采集 → VAD → Opus    摄像头/屏幕 → OpenCV → H.264   │
└──────────────┬───────────────────────────────┬───────────┘
               │ TCP: 控制与信令（自研二进制协议）  │
               │ UDP: 音视频帧（裸流）             │
┌──────────────▼───────────────────────────────▼───────────┐
│  服务端 PlayerHallServer（C++ / MySQL）                   │
│  账号鉴权 → 房间管理 → 媒体转发中继                        │
└──────────────────────────────────────────────────────────┘
```

### 自定义二进制协议

帧格式：**[int32 小端 包体长度][包体]**

| 包类型 | 方向 | 说明 |
|---|---|---|
| `PACK_REGISTER_RQ/RS` | C→S / S→C | 账号注册 |
| `PACK_LOGIN_RQ/RS` | C→S / S→C | 登录鉴权 |
| `PACK_CREATEROOM_RQ/RS` | C→S / S→C | 创建房间 |
| `PACK_JOINROOM_RQ/RS` | C→S / S→C | 加入房间 |
| `PACK_LEAVEROOM_RQ/RS` | C→S / S→C | 离开房间 |
| `PACK_ROOM_MEMBER` | S→C | 成员列表同步 |
| `PACK_AUDIO_REGISTER` | C→S | 音频 fd 注册 |
| `PACK_VIDEO_REGISTER` | C→S | 视频 fd 注册 |
| `PACK_AUDIO_FRAME` | C→S | 音频帧转发 |
| `PACK_VIDEO_FRAME` | C→S | 视频帧转发 |
| `PACK_VIDEO_SPSPPS` | C→S | H.264 参数集 |
| `PACK_PLI_REQUEST` | C→S | 请求关键帧 |
| `PACK_UPDATE_EMAIL_RQ/RS` | C→S / S→C | 邮箱补全 |

## 代码规模

| 部分 | 语言 | 行数 |
|---|---|---|
| 客户端自研代码 | C++ / Qt | ~8,750 |
| 服务端 | C++ | ~2,800 |
| 随仓库分发的第三方源码（Opus / Speex / WebRTC） | C | ~25,600 |

> 第三方源码（BSD-3-Clause）的清单与许可说明见 [NOTICE](NOTICE) 第 2.1 节。

## 目录结构

```
vcc-video-conference/
├── vcc-client/              客户端
│   ├── ckernel.{h,cpp}      内核：信号槽调度、协议编解码、房间状态
│   ├── vccdialog.{h,cpp,ui} 主界面（创建/加入房间）
│   ├── logindialog.{h,cpp,ui} 登录/注册界面
│   ├── roomdialog.{h,cpp,ui}   房间内界面（成员列表、控制按钮）
│   ├── usershow.{h,cpp,ui}     视频画面显示控件
│   ├── main.cpp              入口
│   ├── netapi/               自研网络协议栈
│   │   ├── net/              TcpClient / TcpServer / UdpNet
│   │   ├── mediator/         TCP/UDP 中介器抽象
│   │   └── net/packdef.h     协议包定义（唯一事实来源）
│   ├── AudioApi/             音频采集、编码、播放
│   ├── VideoApi/             视频采集、屏幕共享、人脸检测
│   ├── VideoRender/          H.264 编解码与 OpenGL 渲染
│   ├── md5/                  口令散列
│   └── opengl/               OpenGL 渲染（历史保留）
│
├── server/                   服务端
│   ├── PlayerHallServer.pro
│   ├── src/TCPKernel.cpp     网络内核
│   ├── src/clogic.cpp        业务逻辑（6 类请求处理器）
│   ├── src/Mysql.cpp         数据库封装
│   ├── src/bcrypt_wrapper.cpp 口令 bcrypt 散列
│   └── include/packdef.h     协议定义（与客户端严格对齐）
│
└── tools/
    └── make_placeholder_assets.py  生成 UI 占位图
```

### 关于界面素材

原项目带有 193 张表情与 37 张 UI 图，**版权归属不明，已全部从本仓库移除**。

其中 4 张被 `logindialog.ui` / `vccdialog.ui` 通过 `resource.qrc` 实际引用
（登录页背景、三个按钮图标）。为保证克隆后**编译通过且界面不空白**，
这 4 张替换为同名同尺寸的自绘中性占位图（合计约 5 KB）：

| 文件 | 尺寸 | 用途 |
|---|---|---|
| `vcc-client/images/register.png` | 478×854 | 登录页品牌背景（显示区 220×400） |
| `vcc-client/images/sett.png` | 975×975 | 设置按钮图标 |
| `vcc-client/images/++.png` | 2048×2048 | 「+」号按钮图标（引用 3 处） |
| `vcc-client/tx/4.png` | 79×77 | 摄像头按钮图标 |

想换成自己的素材，重跑脚本生成新的即可：

```bash
pip install Pillow
python tools/make_placeholder_assets.py
```

## 构建

### 环境要求

| 项目 | 要求 |
|---|---|
| Qt | 5.12.11（MinGW 32-bit Kit） |
| 编译器 | MinGW 7.3.0 32-bit |
| OpenCV | 4.2.0 |
| 数据库 | MySQL 5.6+ |

### 第三方依赖需自行获取

本仓库**不含**第三方源码（避免许可证冲突与仓库膨胀），请按 `NOTICE`
中的表格自行下载并放到以下位置：

```
vcc-client/
├── ffmpeg-4.2.2/          # 需含 include/ lib/ bin/
├── SDL2-2.0.10/          # 需含 include/ lib/x86/SDL2.lib
└── thirdparty/            # OpenCV 等运行时 DLL（可选）
```

### 编译客户端

```bash
# 生成 Makefile
mkdir -p build && cd build
qmake ../vcc-client/vcc-client.pro -spec win32-g++ "CONFIG+=release"

# 编译
mingw32-make -f Makefile.Release -j4

# 产物
release/vcc-client.exe
```

> **运行时依赖**：产物目录需要具备 Qt 运行时 DLL、FFmpeg 的 `*.dll`、
> SDL2 的 `SDL2.dll` 以及 OpenCV 的 `*.dll`。
> 可用 Qt 自带的 `windeployqt.exe` 收集 Qt 部分：
> ```bash
> windeployqt release/vcc-client.exe
> ```

### 编译服务端

```bash
cd server
qmake PlayerHallServer.pro
make

# 数据库连接信息通过环境变量注入，不写入源码：
#   DB_IP / DB_USER / DB_PWD / DB_NAME
```

## 配置

客户端通过同目录下的 `config.ini` 指定服务端地址：

```ini
[Net]
ip=127.0.0.1
port=8000
```

服务端数据库账号口令**不写入源码**，通过环境变量注入：

| 变量 | 说明 | 默认值 |
|---|---|---|
| `DB_IP` | 数据库地址 | `localhost` |
| `DB_USER` | 数据库用户 | `vcc`（**须覆盖**） |
| `DB_PWD` | 数据库口令 | 空（**须注入**） |
| `DB_NAME` | 数据库名 | `vcc`（**须覆盖**） |

## 安全说明

本项目为学习作品，已在开发过程中处理过以下安全问题，
相关防护代码保留在仓库中供学习参考：

- **SQL 注入**：全部查询改为参数化/预处理语句
- **口令存储**：数据库中仅存 bcrypt 散列，不存明文
- **越权访问**：邮箱补全接口增加归属校验
- **内存安全**：修复登录包 `memset` 越界写
- **凭据外置**：数据库口令与会话密钥均由环境变量/文件注入，权限 600

## 项目状态

核心功能已实现并可运行。当前为**个人学习与展示用途**，
未做大规模并发压测，不建议直接用于生产环境。

## 许可证

本项目采用 [MIT](LICENSE) 许可证。

第三方组件的许可证说明见 [NOTICE](NOTICE)。
**本项目与「企业微信」及腾讯公司无任何关联。**

## 免责声明

详见 [NOTICE](NOTICE) 第五节。
