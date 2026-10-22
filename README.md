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
├── deploy/                   部署资产（Docker / 裸机 / mTLS / CI 门禁）
│   ├── deploy.sh rollback.sh smoke.sh security_smoke.sh
│   ├── Dockerfile docker-compose*.yml
│   ├── bare/tls/             私有 CA、证书签发/吊销、CRL 定时续期
│   ├── tls/ stunnel/         证书生成与 mTLS 隧道
│   └── monitoring/           Prometheus 告警规则
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

本仓库**已包含**部分第三方源码（Opus / Speex / WebRTC AEC/NS/AGC/VAD，
均为 BSD-3-Clause，合计约 2.5 万行，清单见 [NOTICE](NOTICE) 第 2.1 节）；
但以下**体积较大的依赖不随仓库分发**（已由 `.gitignore` 排除），
请按 NOTICE 中的表格自行下载并放到指定位置：

```
vcc-client/
├── ffmpeg-4.2.2/          # 需含 include/ lib/ bin/
├── SDL2-2.0.10/          # 需含 include/ lib/x86/SDL2.lib
└── thirdparty/            # OpenCV 等运行时 DLL（可选）
```

OpenCV 不在上述目录里硬编码，而是通过 `OPENCV_DIR` 变量指定
（详见 `vcc-client/VideoApi/videoapi.pri` 顶部注释），例如：

```bash
export OPENCV_DIR=D:/path/to/opencv-release/opencv-release
qmake vcc-client.pro
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

## 部署

`deploy/` 提供完整的容器化部署资产与 TLS 双向认证方案。

```
deploy/
├── deploy.sh                 一键部署（构建镜像 → 起栈 → 迁移 → 冒烟）
├── deploy-bare.sh            裸机部署（无容器）
├── rollback.sh               按 tag 回滚
├── smoke.sh                  功能冒烟
├── security_smoke.sh         安全项冒烟（mTLS / SQL 注入 / 弱口令）
├── ci-local.sh               本地门禁：编译 + 冒烟 + 注入回归
├── migrate.sh                数据库版本化迁移
├── prepare.sh                打包前的 staging 准备
├── pack_client.bat           客户端打包（Windows）
├── Dockerfile
├── docker-compose.yml        主栈：MySQL + 服务端
├── docker-compose.tls.yml    TLS 变体
├── docker-compose.smoke.yml  冒烟隔离栈
├── schema.sql                建库建表
├── migrations/               版本化迁移脚本
├── playerhallserver.service   systemd 单元
├── config.ini.example        客户端配置样例
├── .env.example              环境变量样例（复制为 .env 后填写）
├── tls/                      证书生成（gen-certs.sh）
├── stunnel/                  mTLS 隧道配置
├── bare/tls/                 裸机 mTLS：私有 CA、签发/吊销、CRL 定时续期
└── monitoring/               Prometheus 告警规则
```

### 快速开始

```bash
cd deploy

# 1. 填配置：复制样例并修改数据库口令等敏感项
cp .env.example .env

# 2. 生成 TLS 证书（若需 mTLS 双向认证）
./tls/gen-certs.sh

# 3. 一键部署
./deploy.sh

# 4. 验证
./smoke.sh && ./security_smoke.sh
```

> **凭据处理**：`deploy/.env`、`deploy/certs/`、`*.key` / `*.crt` / `*.pem`
> 均已在 `.gitignore` 中排除，**不会入库**。证书可用 `tls/gen-certs.sh`
> 或 `bare/tls/ca-init.sh` 重新生成。
> 数据库口令经环境变量注入服务端进程，不写入源码。

### 客户端打包

```batch
deploy\pack_client.bat <服务端IP>
```

产物落在 `deploy/dist/vcc-client/`，含 Qt 运行库（`windeployqt` 收集）、
FFmpeg/SDL2 DLL、`config.ini` 与一键启动脚本。

> 该脚本假设客户端已在
> `build-vcc-client-Desktop_Qt_5_12_11_MinGW_32_bit-release/` 编译完成。
> 实际构建目录以 Qt Creator 的 shadow build 路径为准，可修改脚本顶部的
> `BUILD_DIR` 变量。

## 配置

客户端通过同目录下的 `config.ini` 指定服务端地址：

```ini
[Net]
ip=127.0.0.1
```

> 只有 `ip` 一项生效。服务端端口目前固定为 `8000`，定义在
> `vcc-client/netapi/net/packdef.h` 的 `_DEF_PORT`，
> 改动需改代码后重新编译（代码未读取 `port` 配置项）。
> 未提供 `config.ini` 时回退到 `_DEF_SERVER_IP`，即 `127.0.0.1`。

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

- **SQL 注入**：全部外部输入经 `CMysql::EscapeMysql()` 转义后再拼接 SQL。
  使用官方 `mysql_real_escape_string` 并按连接字符集处理多字节，
  避免手写替换在 GBK/Big5 下被绕过；转义长度按字段 `sizeof` 限定，
  防止定长数组未写 `\0` 时 `strlen` 越界读。
  *（说明：用的是转义而非预处理语句。改用预处理需重构全部查询签名，
  属较大改动，当前转义方案在本项目的表结构下是有效的。）*
- **口令存储**：数据库中存 bcrypt 散列，不存明文。
  *（需要说明的取舍：客户端提交前仍会先做一次 `MD5(口令 + 固定盐)`，
  盐值编译在客户端二进制内、可被逆向获取，因此该层 MD5 能挡住撞库，
  但挡不住针对弱口令的字典攻击。彻底的做法是让客户端直接提交原始口令，
  全程只由服务端做bcrypt。本项目因协议既定未做此改动。）*
- **越权访问**：邮箱补全接口增加归属校验
- **内存安全**：修复登录包 `memset` 越界写；双端补齐包长合法性校验
  （≤0 或 >1MB 直接丢弃），服务端此前缺失该校验，可被未认证客户端
  用异常包长耗尽内存；数据库字段为 NULL 时的空指针构造已加固
- **凭据外置**：数据库口令由环境变量注入，不写入源码；TLS 私钥文件权限 600
- **TLS**：仅启用 TLSv1.2 / TLSv1.3 与现代 GCM 套件，客户端证书双向校验
  （`verify = 3`），无 `VERIFY_NONE`

## 项目状态

核心功能已实现并可运行。当前为**个人学习与展示用途**，
未做大规模并发压测，不建议直接用于生产环境。

## 许可证

本项目采用 [MIT](LICENSE) 许可证。

第三方组件的许可证说明见 [NOTICE](NOTICE)。
**本项目与「企业微信」及腾讯公司无任何关联。**

## 免责声明

详见 [NOTICE](NOTICE) 第五节。
