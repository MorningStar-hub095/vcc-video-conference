# VCC视频会议 音视频会议系统 - 项目开发总结

> **项目类型：** 桌面端多人音视频会议系统（C/S架构）
> **技术栈：** C++ / Qt 5.12 / FFmpeg 4.2 / SDL2 / Opus / OpenCV / OpenGL / MySQL
> **开发环境：** Windows + Qt Creator + MinGW 32-bit | 服务端 Linux + epoll
> **总结时间：** 2026-08-01（v1.1 增补静音检测功能）

---

## 一、项目概述

### 1.1 项目目标
开发一个支持多人实时音视频通话的桌面会议系统，功能包括：用户注册登录、房间管理（创建/加入/退出）、摄像头视频通话、麦克风语音通话、桌面共享、人脸检测标记、音视频同步等。

### 1.2 系统架构

**架构设计原则：** 六层分层 + 中介者模式 + 接口抽象。UI层与网络层完全解耦，通过ckernel中介者统一调度。网络层通过INet/INetMediator接口抽象，支持TCP/UDP无缝切换。

#### 客户端架构（Windows/Qt 5.12）
```
UI层（Qt Widgets 用户界面）
├── LoginDialog      登录注册界面（手机号+MD5加盐密码）
├── VCC视频会议Dialog     主界面（创建/加入房间、设置）
├── RoomDialog       会议房间界面（成员列表、音视频控制、退出）
└── UserShow         用户视频显示控件（封装YuvGLWidget/MyOpenGLWidget）

核心调度层（业务中枢 - Meyer单例）
└── ckernel          协议映射表 m_netPackMap[100] | 三路TCP连接 | 模块生命周期管理
                     信号槽连接：20+条跨模块信号槽连接

音频层（SDL2 + Opus 48kHz）
├── AudioRead        SDL回调采集(960samples/20ms) + WebRTC VAD(动态阈值+状态平滑) + Opus编码(VOIP+DTX+FEC) + SIG_muteStateChanged UI联动
└── AudioWrite       mutex保护的deque队列 + Opus解码 + SDL回调解码播放

视频采集层（OpenCV + Qt Screen）
├── VideoRead        多后端回退采集(MSMF→DirectShow→默认) 320x240@30fps
├── ScreenRead       QScreen::grabWindow桌面截图 1024x768
└── MyFaceDetect     OpenCV CascadeClassifier人脸检测（可选）

视频编解码层（FFmpeg 4.x H.264）
├── H264Encoder      硬件回退链(NVENC→QSV→libx264) ultrafast+zerolatency 200kbps
└── H264Decoder      每成员独立实例 延迟初始化 引用计数零拷贝输出

渲染层（OpenGL 4.x + GLSL）
├── YuvGLWidget      三纹理YUV420P上传 + Fragment Shader BT.601色彩转换
└── MyOpenGLWidget   QImage纹理上传（JPEG/桌面共享回退路径）

同步层（音频主时钟方案）
└── AVSyncManager    单例 每用户独立SyncContext 同步阈值10ms 最大等待100ms

网络传输层（Winsock2 TCP 三路分离）
├── INet             纯虚接口（InitNet/SendData/UnInitNet）
├── TcpClient        TCP客户端（40KB接收缓冲区 4字节包头防粘包）
├── TcpServer        TCP服务端（epoll/select多路复用）
└── INetMediator     中介者抽象基类（Qt信号SIG_ReadyData跨线程传递）

工具层
├── MD5              加盐哈希（盐值vC0nF2024$SecR1t^S4ltK3y!）
├── ThreadWorker     线程工作基类（moveToThread + 信号驱动）
└── SendVideoWorker  视频帧跨线程发送（300ms丢帧策略）
```

#### 服务端架构（Linux + epoll）
```
PlayerHallServer/
├── block_epoll_net  epoll多路复用（LT模式 + EPOLLONESHOT）线程池处理
├── clogic           协议分发（函数指针映射表，与客户端对称设计）
├── 房间管理         roomId → memberList映射（std::map + mutex保护）
├── TCPKernel        网络内核（连接管理、数据收发）
└── MySQL            VCC视频会议.t_user表（用户注册信息持久化）
```

#### 线程模型
```
线程1: Qt主线程(UI)        → 事件循环、信号槽分发、协议处理、OpenGL渲染
线程2: SDL音频采集线程      → audioCallback每20ms触发、VAD检测、Opus编码
线程3: SDL音频播放线程      → audioCallback每20ms触发、Opus解码、混音播放
线程4: 网络连接子线程       → std::thread.detach()执行阻塞式TCP connect
线程5: SendVideoWorker线程  → 视频帧TCP发送、4KB分片循环发送
线程6+: 接收线程(TcpClient) → 每条TCP连接一个接收线程（共3个）
```

### 1.3 开发流程总览
```
需求分析 → 架构设计 → 模块开发 → 集成测试 → 部署调试 → 优化迭代
   ↓          ↓          ↓          ↓          ↓          ↓
 确定功能    分层设计    逐模块编码  联调测试    网络配置    音视频同步
 技术选型    接口定义    协议实现    压力测试    端口调试    H264升级
```

---

## 二、开发阶段详细记录

### 阶段一：需求分析与技术选型

#### 2.1.1 功能需求梳理

| 功能模块 | 具体需求 | 优先级 |
|----------|----------|--------|
| 用户系统 | 注册（手机号+密码+用户名）、登录（MD5加盐） | P0 |
| 房间管理 | 创建房间、加入房间（房间号）、退出房间、成员列表同步 | P0 |
| 音频通话 | 麦克风采集、语音编码传输、多人混音播放 | P0 |
| 静音检测(VAD) | 基于 WebRTC VAD 静音检测：动态阈值自适应 + 状态平滑防抖 + 静音状态 UI 联动（房间界面 cb_audio 反馈） | P1 |
| 视频通话 | 摄像头采集、视频编码传输、多路视频显示 | P0 |
| 桌面共享 | 屏幕截图、编码传输、远程显示 | P1 |
| 人脸检测 | 摄像头画面中标记人脸位置 | P2 |
| 音视频同步 | 音频主时钟方案，视频跟随音频节奏 | P1 |
| H264编码 | 替代JPEG，支持硬件加速编码 | P1 |

#### 2.1.2 技术选型决策

| 技术维度 | 选型方案 | 选型理由 | 备选方案 |
|----------|----------|----------|----------|
| **UI框架** | Qt 5.12 | 跨平台、信号槽机制、丰富的GUI组件 | MFC、wxWidgets |
| **视频采集** | OpenCV VideoCapture | 简单易用、多后端支持 | DirectShow、V4L2 |
| **视频编码** | FFmpeg H.264 | 硬件加速支持、压缩率高、行业标准 | OpenH264、x264独立库 |
| **音频采集** | SDL2（后期升级） | 回调驱动、跨平台、与播放统一 | QAudioInput（初始方案） |
| **音频编码** | Opus（后期升级） | 低延迟、高音质、内置DTX/FEC | Speex（初始方案） |
| **音频播放** | SDL2 | 回调驱动、低延迟、跨平台 | QAudioOutput |
| **视频渲染** | OpenGL + GLSL | GPU渲染、YUV直接上屏、性能最优 | QPainter软渲染 |
| **网络通信** | TCP（Winsock2） | 可靠传输、简单实现 | UDP+RTP |
| **数据库** | MySQL | 成熟稳定、社区支持好 | SQLite、PostgreSQL |
| **服务端** | epoll（Linux） | 高并发IO多路复用 | select、poll |

---

### 阶段二：架构设计

#### 2.2.1 分层架构设计

采用 **六层架构** + **中介者模式**，各层职责明确：

```
UI层 ←→ 核心层(ckernel) ←→ 业务层(音频/视频/编解码/同步)
                ↓
         网络层(INetMediator)
                ↓
         传输层(TcpClient/Server)
```

**设计模式应用：**

| 模式 | 应用位置 | 解决的问题 |
|------|----------|------------|
| 接口模式 | INet / INetMediator | 网络层可替换（TCP/UDP） |
| 中介者模式 | ckernel | 解耦UI层与业务层，统一消息分发 |
| 单例模式 | AVSyncManager | 全局唯一同步管理器 |
| 观察者模式 | Qt信号槽 | 跨线程、跨模块事件通知 |
| 策略模式 | m_netPackMap函数指针表 | O(1)协议分发 |
| 生产者-消费者 | SDL音频回调+deque队列 | 线程安全音频数据传递 |
| 模板方法 | ThreadWorker → SendVideoWorker | 线程工作基类复用 |

#### 2.2.2 协议设计

**设计原则：** 二进制协议、统一包头、O(1)分发、三路TCP分离。

**协议常量定义：**
```cpp
#define _DEF_BUFFER      4096    // 网络缓冲区大小
#define _DEF_CONTENT_SIZE 1024   // 内容区大小
#define _MAX_SIZE        40      // 登录标识字段最大长度
#define MAX_SIZE         60      // 通用字段最大长度
#define _DEF_PORT        8000    // 服务端默认端口
#define _DEF_SERVERIP    "192.0.2.1"  // 默认服务端IP
#define DEF_PACK_BASE    10000   // 协议类型编号基址
#define DEF_PACK_COUNT   100     // 协议映射表容量
#define H264_FRAGMENT_SIZE      4096   // H.264分片大小
#define H264_REASSEMBLE_TIMEOUT 100    // 分片重组超时(ms)
```

**统一包头格式：**
```
[4字节包长(int)][PackType type(int)][int userId] + 业务字段...
```

**协议类型编号（共20种）：**

| 协议名 | 类型编号 | 方向 | 说明 |
|--------|----------|------|------|
| REGISTER_RQ | 10000 | C→S | 注册请求（tel+password+name+email，字段60字节） |
| REGISTER_RS | 10001 | S→C | 注册响应（result: 0=手机号已存在/1=成功/2=昵称已存在） |
| LOGIN_RQ | 10002 | C→S | 登录请求（tel字段复用为通用标识，支持手机/用户名/邮箱） |
| LOGIN_RS | 10003 | S→C | 登录响应（result: 0=用户不存在/1=密码错误/2=成功/3=成功但无邮箱） |
| CREATEROOM_RQ | 10004 | C→S | 创建房间请求 |
| CREATEROOM_RS | 10005 | S→C | 创建房间响应（roomId + 成员列表） |
| JOINROOM_RQ | 10006 | C→S | 加入房间请求（roomId正则校验：`^[0-9]{1,8}$`） |
| JOINROOM_RS | 10007 | S→C | 加入房间响应 |
| ROOMMEMBER | 10008 | S→C | 房间成员通知（userId+userName，UTF-8编码） |
| AUDIOFRAME | 10009 | C→S→C | 音频帧转发（Opus编码数据，60-120字节/帧） |
| VIDEOFRAME | 10010 | C→S→C | 视频帧-JPEG转发（300-800KB/帧） |
| LEAVEROOM_RQ | 10011 | C→S | 退出房间请求 |
| LEAVEROOM_RS | 10012 | S→C | 退出房间通知（广播给其他成员） |
| AUDIO_REGISTER | 10013 | C→S | 音频通道注册（连接2握手） |
| VIDEO_REGISTER | 10014 | C→S | 视频通道注册（连接3握手） |
| UPDATE_EMAIL_RQ | 10015 | C→S | 补全邮箱请求 |
| UPDATE_EMAIL_RS | 10016 | S→C | 补全邮箱响应 |
| VIDEOFRAME_H264 | 10017 | C→S→C | H.264视频帧转发（4KB分片，含帧序号+分片索引） |
| PLI_REQUEST | 10018 | C→S→C | PLI关键帧请求（新成员加入时触发） |
| SPS_PPS | 10019 | C→S→C | H.264 SPS/PPS参数集（可选，编码器inline模式可不使用） |

**三路TCP分离设计：**
- **连接1（信令）：** 登录/注册/房间管理/邮箱补全，小包（50-200字节），低频
- **连接2（音频）：** 音频帧转发，中包（100-200字节），高频（50帧/秒）
- **连接3（视频）：** 视频帧转发，大包（4KB/分片），中频（30帧/秒）

**设计理由：** 音视频数据量大且持续，与信令混用会导致信令延迟。三路分离确保控制指令即时响应。每条连接独立接收线程，互不阻塞。

---

### 阶段三：模块开发

#### 2.3.1 网络模块开发

**关键实现：**

```cpp
// TcpClient.h - TCP客户端核心成员
class TcpClient : public INet
{
    SOCKET m_sock;           // 主连接socket（INVALID_SOCKET表示未连接）
    HANDLE m_hThread;        // 接收线程句柄
    char m_szRecvBuf[40960]; // 接收缓冲区（40KB，防止粘包溢出）
    int m_nOffset;           // 缓冲区已接收字节数（循环接收状态）
    bool m_isConnected;      // 连接状态标志
    bool m_isStop;           // 停止接收线程标志
};
```

**遇到的问题与解决方案：**

**问题1：TCP粘包**
- **现象：** 多个协议包合并为一个TCP段到达接收端，导致解析错误
- **原因：** TCP是字节流协议，无消息边界
- **解决方案：** 4字节长度包头 + 循环接收直到收满完整包
```cpp
// 接收时先读4字节包长，再根据包长读取完整数据
int nRecvNum = recv(m_sock, m_szRecvBuf + m_nOffset, sizeof(int) - m_nOffset, 0);
if (m_nOffset >= sizeof(int)) {
    int nPackSize = *(int*)m_szRecvBuf;
    // 继续接收直到收满 nPackSize 字节
}
```

**问题2：视频帧超过1MB安全阈值**
- **现象：** JPEG视频帧可能超过1MB，导致接收缓冲区溢出
- **解决方案：** 发送前检查包大小，超过1MB则丢弃该帧
```cpp
if (buflen + sizeof(STRU_VIDEO_FRAME) > 1 * 1024 * 1024) {
    printf("[VideoFrame] 包太大 %d, 跳过该帧\n", buflen);
    return;
}
```

**问题3：UDP sendto阻塞导致音频卡顿**
- **现象：** UDP发送时sendto偶尔阻塞，音频数据积压
- **解决方案：** 改用TCP替代UDP，TCP的流控机制天然处理背压

#### 2.3.2 音频模块开发

**初始方案（Speex + QAudioInput）：**
```
QAudioInput → QTimer 20ms轮询 → WebRTC VAD → Speex编码(8kHz) → TCP发送
TCP接收 → Speex解码 → SDL播放
```

**升级方案（Opus + SDL采集）：**
```
SDL采集回调(48kHz/960samples/20ms) → WebRTC VAD(480样本最佳帧长) → Opus编码(VOIP+DTX+FEC/32kbps) → SIG_audioFrame信号 → ckernel组包 → TCP发送
TCP接收 → slot_playAudio → mutex保护入队deque → SDL音频线程回调出队 → Opus解码(960samples) → SDL_MixAudioFormat混音播放
```

**遇到的问题与解决方案：**

**问题4：SDL初始化与Qt事件循环冲突**
- **现象：** `SDL_Init(SDL_INIT_AUDIO)` 成功，但后续Qt信号槽机制异常
- **原因：** SDL和Qt都试图接管主线程事件循环
- **解决方案：** SDL仅初始化音频子系统（`SDL_INIT_AUDIO`），不初始化视频；Qt负责UI事件循环

**问题5：Speex解码后声音断续**
- **现象：** 接收端播放声音一卡一卡
- **原因：** QTimer轮询精度不足（20ms ±5ms抖动），采集帧大小不一致
- **解决方案：** 升级为SDL回调驱动，系统级音频线程保证20ms ±0.5ms精度

**问题6：多人同时说话时音频数据竞争**
- **现象：** 多个发送者的音频帧同时到达，解码播放出现乱音
- **原因：** deque队列无保护，多线程并发写入
- **解决方案：** std::mutex保护deque队列，SDL回调中加锁取出数据
```cpp
std::deque<QByteArray> m_audioQueue;
std::mutex m_mutex;

// 生产者（接收线程）
void AudioWrite::slot_playAudio(QByteArray bt) {
    std::lock_guard<std::mutex> lck(m_mutex);
    m_audioQueue.emplace_back(bt);
}

// 消费者（SDL回调线程）
void AudioWrite::audioCallback(void *userdata, Uint8 *stream, int len) {
    std::lock_guard<std::mutex> lck(audio->m_mutex);
    if (!audio->m_audioQueue.empty()) {
        // 取出并解码播放
    }
}
```

**问题7：WebRTC VAD与48kHz采样率兼容性**
- **现象：** WebRtcVad_Process在48kHz下返回异常结果
- **原因：** VAD默认针对16kHz优化，48kHz需要调整参数
- **解决方案：** 使用 `WebRtcVad_ValidRateAndFrameLength(48000, 480)` 验证，480样本(10ms)为最佳帧长

**已实现功能：基于 WebRTC VAD 的静音检测**

在基础 VAD 检测之上，AudioRead 进一步落地了完整的静音检测(VAD)能力，由四个要素组成：

1. **模式设置：** 构造函数在 VAD 初始化后显式调用 `WebRtcVad_set_mode(handle, 2)`，设为 VoIP 推荐模式 2。
2. **动态阈值：** QTimer 每 5 秒触发 `slot_adjustVadMode()`，用噪声 RMS 的 EMA（α=0.95）估计环境噪声，按三档自适应切换 VAD 模式——EMA<0.02→模式1（宽松）、<0.10→模式2（中等）、≥0.10→模式3（严格），仅模式变化时调用 `WebRtcVad_set_mode`。
3. **状态平滑：** audioCallback 中对 VAD 结果做连续 3 帧确认（≈60ms）防抖，`m_consecutiveSilentFrames`/`m_consecutiveSpeechFrames` 达到 `MUTE_CONFIRM_THRESHOLD=3` 才切换状态，避免单帧抖动误触发。
4. **UI 联动：** 状态切换时 `Q_EMIT SIG_muteStateChanged(bool)`，ckernel `slot_muteStateChanged` 接收后调用 `RoomDialog::updateMuteStatus`，静音时 cb_audio 文本变"音频(无声)"并置灰，有声时恢复"音频"。

线程模型：audioCallback 在 SDL 线程计算 RMS（`calculateRMS`）、写 `m_noiseLevel`、发信号（跨线程 QueuedConnection 到主线程）；QTimer 主线程触发 `slot_adjustVadMode` 读 `m_noiseLevel`（单 float 良性跨线程）；UI 更新在主线程。静音帧仍 memset 清零 PCM，配合 Opus DTX 双重压缩，静音时带宽降至 ~1kbps。

#### 2.3.3 视频模块开发

**摄像头采集（OpenCV）：**
```cpp
// 多后端回退策略
if (!m_cap.open(0, cv::CAP_MSMF)) {        // Windows首选MSMF
    if (!m_cap.open(0, cv::CAP_DSHOW)) {    // 回退DirectShow
        if (!m_cap.open(0)) {               // 默认后端
            return;
        }
    }
}
m_cap.set(cv::CAP_PROP_FRAME_WIDTH, 320);
m_cap.set(cv::CAP_PROP_FRAME_HEIGHT, 240);
m_cap.set(cv::CAP_PROP_FPS, 30);
```

**遇到的问题与解决方案：**

**问题8：摄像头打开失败（无MSMF后端）**
- **现象：** `m_cap.open(0)` 失败，返回false
- **原因：** Windows下Qt Creator编译的OpenCV默认不带MSMF后端
- **解决方案：** 三重回退：MSMF → DirectShow → 默认后端

**问题9：视频帧发送阻塞UI线程**
- **现象：** 摄像头采集回调中直接调用TCP发送，UI卡顿
- **原因：** TCP send可能阻塞（发送缓冲区满）
- **解决方案：** 视频编码和发送分离到独立子线程（SendVideoWorker）
```cpp
// 主线程：采集+编码
void ckernel::slot_sendVideoFrame(QImage img) {
    QByteArray ba;
    if (m_h264Encoder) {
        m_h264Encoder->encode(img, ba);  // H264编码
    }
    Q_EMIT m_pSendVideoWorker->SIG_SendVideo(ba.data(), ba.size());  // 发送到子线程
}

// 子线程：网络发送（300ms丢帧策略）
void ckernel::slot_SendVideo(char* buf, int len) {
    m_pClient->SendData(0, buf, len);  // TCP发送
}
```

**问题10：视频帧队列堆积**
- **现象：** 网络慢时视频帧在队列中堆积，内存持续增长
- **解决方案：** 子线程中检查队列长度，超过300ms的旧帧直接丢弃

#### 2.3.4 H264编解码模块开发

**编码器硬件回退链（4级回退）：**
```cpp
// 依次尝试：NVENC(NVIDIA GPU) → QSV(Intel核显) → libx264(纯CPU) → AV_CODEC_ID_H264(系统默认)
const char* encoders[] = {"h264_nvenc", "h264_qsv", "libx264"};
for (auto name : encoders) {
    codec = avcodec_find_encoder_by_name(name);
    if (codec) { m_encoderName = name; break; }
}
if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);

// 编码参数配置
codecCtx->width = 320;                    // 视频宽度
codecCtx->height = 240;                   // 视频高度
codecCtx->time_base = {1, 30};            // 30fps
codecCtx->framerate = {30, 1};
codecCtx->bit_rate = 200000;              // 200kbps码率
codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;  // YUV420P色彩空间
codecCtx->gop_size = 60;                  // 每60帧一个I帧(2秒)
codecCtx->max_b_frames = 0;               // 禁用B帧（降低延迟）
codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;  // 低延迟标志

// preset和tune优化
av_opt_set(codecCtx->priv_data, "preset", "ultrafast", 0);  // 最快编码速度
av_opt_set(codecCtx->priv_data, "tune", "zerolatency", 0);  // 零延迟调优
```

**遇到的问题与解决方案：**

**问题11：NVENC编码器找不到**
- **现象：** `avcodec_find_encoder_by_name("h264_nvenc")` 返回NULL
- **原因：** 系统没有NVIDIA显卡或驱动版本不支持
- **解决方案：** 回退链自动尝试QSV→libx264→AV_CODEC_ID_H264

**问题12：编码后画面绿色/花屏**
- **现象：** 解码后画面有绿色条纹
- **原因：** RGB24→YUV420P转换时stride对齐问题，Qt的QImage每行字节数可能不是4的倍数
- **解决方案：** 使用 `av_image_alloc` 分配对齐缓冲区，sws_scale自动处理stride
```cpp
sws_scale(m_swsCtx,
    srcSlice, srcStride, 0, m_height,  // 输入：RGB24
    m_frame->data, m_frame->linesize); // 输出：YUV420P（自动4字节对齐）
```

**问题13：关键帧请求（PLI）机制**
- **问题：** 新成员加入房间时，收到的视频帧是P帧（预测帧），无法独立解码，画面花屏
- **解决方案：** 新成员加入时发送PLI请求，服务端转发给其他成员，发送方收到后强制编码一个I帧
```cpp
// H264Encoder收到PLI后
void H264Encoder::requestKeyFrame() {
    m_codecCtx->gop_size = 0;  // 强制下一帧为I帧
}
// 编码完成后恢复
m_codecCtx->gop_size = 60;     // 恢复正常GOP
```

**问题14：AVFrame引用计数导致的use-after-free**
- **现象：** 偶发崩溃，访问已释放的AVFrame
- **原因：** 代码中对同一AVFrame调用了两次 `av_frame_free`
- **解决方案：** 严格遵循引用计数规则：clone后必须unref原始帧，传递到渲染层后由渲染层负责free
```cpp
// 正确的零拷贝传递链
AVFrame* sharedFrame = av_frame_clone(frame);  // refcount=2
av_frame_unref(frame);                          // refcount=1
emit signal_frameDecoded(sharedFrame);
// 渲染层收到后：使用完毕 → av_frame_free(sharedFrame)
```

**问题15：H264视频帧超过1MB**
- **现象：** H264编码后的I帧可能超过1MB
- **解决方案：** 4KB分片传输，每个分片带帧序号和分片索引，接收端重组
```cpp
#define H264_FRAGMENT_SIZE 4096
// 发送端：分片
int totalFragments = (h264Data.size() + H264_FRAGMENT_SIZE - 1) / H264_FRAGMENT_SIZE;
for (int i = 0; i < totalFragments; i++) {
    // 构造分片包：[header + fragmentIndex + totalFragments + data]
}
// 接收端：重组
std::map<int, std::vector<QByteArray>> m_fragments;
```

#### 2.3.5 视频渲染模块开发

**OpenGL现代管线（YuvGLWidget）：**

采用VAO/VBO + Shader的现代OpenGL管线（兼容Core Profile），YUV420P三纹理上传 + Fragment Shader BT.601色彩转换：

```glsl
// GLSL Fragment Shader - YUV420P → RGB（BT.601标准转换矩阵）
varying vec2 v_texCoord;
uniform sampler2D tex_y;  // Y分量纹理（GL_RED格式，亮度）
uniform sampler2D tex_u;  // U分量纹理（GL_RED格式，色度Cb）
uniform sampler2D tex_v;  // V分量纹理（GL_RED格式，色度Cr）

void main() {
    float y = texture2D(tex_y, v_texCoord).r;        // 采样Y分量
    float u = texture2D(tex_u, v_texCoord).r - 0.5;  // 采样U分量（偏移到[-0.5, 0.5]）
    float v = texture2D(tex_v, v_texCoord).r - 0.5;  // 采样V分量（偏移到[-0.5, 0.5]）

    // BT.601标准 YUV → RGB 转换矩阵
    float r = y + 1.402 * v;                          // R通道
    float g = y - 0.344136 * u - 0.714136 * v;        // G通道
    float b = y + 1.772 * u;                          // B通道
    gl_FragColor = vec4(clamp(r, 0.0, 1.0),
                       clamp(g, 0.0, 1.0),
                       clamp(b, 0.0, 1.0), 1.0);
}
```

**纹理上传策略：**
- 首帧：`glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, data)` 分配GPU内存
- 后续帧：`glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, data)` 增量更新（避免重复分配）
- 纹理尺寸变化时重新分配（`m_texWidth != frame->width || m_texHeight != frame->height`）

**性能数据：**
- 320x240@30fps 单路：CPU占用 <2%（仅纹理上传开销）
- 4路同时渲染：CPU占用 <5%（GPU并行处理色彩转换）
- 相比QPainter软渲染：CPU占用降低约85%

**遇到的问题与解决方案：**

**问题16：首次渲染闪烁/黑屏**
- **现象：** 第一帧渲染时出现短暂黑屏或闪烁
- **原因：** 首次调用 `paintGL` 时纹理尚未上传完成
- **解决方案：** 首帧使用 `glTexImage2D` 分配纹理内存，后续使用 `glTexSubImage2D` 增量更新

**问题17：桌面共享帧与摄像头帧使用不同渲染路径**
- **问题：** 桌面共享输出QImage，摄像头输出AVFrame，需要两套渲染
- **解决方案：** UserShow中维护两套Widget（YuvGLWidget + MyOpenGLWidget），根据数据类型选择渲染路径
```cpp
void UserShow::slot_setFrame(AVFrame* frame) {
    if (m_yuvWidget) m_yuvWidget->slot_setFrame(frame);  // H264解码帧
}
void UserShow::slot_setImage(QImage img) {
    if (m_glWidget) m_glWidget->slot_setImage(img);       // JPEG/桌面共享
}
```

#### 2.3.6 音视频同步模块开发

**音频主时钟方案（参考ffplay）：**

每用户独立SyncContext，音频时钟为主基准，视频时钟为从基准。同步阈值10ms，最大等待100ms：

```cpp
// AVSyncManager 核心参数
#define SYNC_THRESHOLD_US  10000    // 同步阈值：10ms（超过此值才等待）
#define MAX_WAIT_MS        100      // 最大等待：100ms（避免卡死）
#define CLOCK_TIMEOUT_S    5.0      // 时钟超时：5秒（超时标记失效）

// 核心同步逻辑
int AVSyncManager::computeSyncDelay(SyncContext* ctx, double video_pts, double frameDelay) {
    double currentVideoPts = getCurrentTimeUs();  // 微秒级系统时间
    double diff = currentVideoPts - ctx->audioClock;  // 音视频时间差

    if (diff > SYNC_THRESHOLD_US) {
        // 视频超前于音频，需要等待
        int waitMs = qMin((int)(diff / 1000), MAX_WAIT_MS);
        return waitMs;  // 返回等待毫秒数
    }
    return 0;  // 同步正常或视频落后，立即显示
}

// SyncContext 结构（每用户独立）
struct SyncContext {
    double audioClock;          // 音频时钟（微秒，由audioCallback更新）
    double videoClock;          // 视频时钟（微秒，由解码PTS更新）
    qint64 lastAudioUpdateTime; // 音频最后更新时间（超时检测）
    qint64 lastVideoUpdateTime; // 视频最后更新时间
    bool isPaused;              // 暂停标志
    atomic<bool> audioClockValid; // 音频时钟有效标志（新用户保护）
};
```

**遇到的问题与解决方案：**

**问题18：新用户加入房间时同步状态异常**
- **现象：** 新成员加入后，视频帧等待时间过长
- **原因：** 新用户的audioClock未初始化，computeSyncDelay返回异常值
- **解决方案：** `audioClockValid` 原子标志位，未就绪时直接显示视频帧不等待

**问题19：长时间通话后音视频逐渐失步**
- **现象：** 通话30分钟后，音视频偏差逐渐增大
- **原因：** 音频和视频的系统时钟存在微小漂移
- **解决方案：** SyncContext 5秒超时机制，超过5秒未更新的时钟标记为失效

---

### 阶段四：集成测试

#### 2.4.1 测试环境搭建
- 服务端：Linux虚拟机（VMware），安装MySQL，编译PlayerHallServer
- 客户端：Windows本机，Qt Creator编译VCC视频会议
- 网络：虚拟机桥接模式，客户端config.ini配置服务端IP

#### 2.4.2 测试用例与结果

| 测试项 | 测试方法 | 遇到的问题 | 解决方案 |
|--------|----------|------------|----------|
| 登录注册 | 两台客户端同时注册 | 注册后需切换到登录标签页 | UI设计已考虑，非Bug |
| 房间创建/加入 | 创建房间后用房间号加入 | 服务端未开启时报"网络未连接" | 检查config.ini IP配置 |
| 音频通话 | 两台客户端互发音频 | 声音断续（Speex方案） | 升级为SDL+Opus |
| 视频通话 | 两台客户端互发视频 | JPEG模式带宽高、延迟大 | 升级为H264编码 |
| H264视频 | 开启H264模式 | 新加入者画面花屏 | PLI关键帧请求机制 |
| 桌面共享 | 开启桌面共享 | 帧率低（约15fps） | 降低分辨率到1024x768 |
| 音视频同步 | 多人通话 | 逐渐失步 | 音频主时钟+5秒超时 |
| 长时间通话 | 持续30分钟 | 无内存泄漏 | 引用计数正确释放 |

#### 2.4.3 网络问题排查

**问题20：客户端连接服务端失败**
```
日志输出：[DEBUG] 网络未连接
```
- **排查步骤：**
  1. `ping 192.0.2.130` → 网络可达
  2. `netstat -tlnp | grep 8000` → 服务端未监听
  3. 检查MySQL → 服务未启动
  4. `systemctl start mysql` → 启动MySQL
  5. 重新启动PlayerHallServer → 成功监听8000端口
  6. `Test-NetConnection -ComputerName 192.0.2.130 -Port 8000` → TcpTestSucceeded: True

**问题21：config.ini IP地址错误**
- **现象：** 连接旧IP 192.0.2.129 失败
- **原因：** config.ini 优先级高于代码默认值，旧文件未更新
- **解决方案：** 修改 `build-VCC视频会议-...\release\config.ini` 和 `full_copy_test\config.ini` 中的IP

---

### 阶段五：优化迭代

#### 2.5.1 性能优化记录

| 优化项 | 优化前 | 优化后 | 提升 |
|--------|--------|--------|------|
| 视频编码 | JPEG软编 | H264硬件编码(NVENC/QSV) | 带宽降低60%，编码延迟降低80% |
| 音频编码 | Speex 8kHz | Opus 48kHz VOIP | 音质从电话级提升到宽带级 |
| 音频采集 | QTimer轮询 | SDL回调驱动 | 延迟精度从±5ms提升到±0.5ms |
| 视频渲染 | QPainter软渲染 | OpenGL GPU渲染 | CPU占用降低50% |
| 视频传输 | 主线程发送 | 子线程发送+丢帧策略 | UI不再卡顿 |
| 协议分发 | if-else链 | 函数指针映射表 | O(1)查找，消息处理延迟降低 |
| 解码帧传递 | memcpy拷贝 | av_frame_clone零拷贝 | 引用计数机制 | 内存带宽降低，延迟降低 |
| 静音带宽优化 | 静音帧全量编码传输 | WebRTC VAD 静音检测 + memset 清零 + Opus DTX 双重压缩 | 动态阈值自适应 + 状态平滑防抖 | 静音时音频带宽从 32kbps 降至 ~1kbps |

#### 2.5.2 内存管理优化

**问题22：H264Decoder中AVFrame内存泄漏**
- **现象：** 长时间通话后内存持续增长
- **原因：** `avcodec_receive_frame` 成功后未调用 `av_packet_unref`
- **解决方案：** 在 `decode()` 函数中添加 `av_packet_unref(pkt)` 释放包引用

---

## 三、关键技术难点与解决方案汇总

| 序号 | 技术难点 | 解决方案 | 涉及文件 |
|------|----------|----------|----------|
| 1 | TCP粘包 | 4字节包头 + 循环接收 | TcpClient.cpp |
| 2 | UDP阻塞导致卡顿 | 改用TCP三路分离 | ckernel.cpp |
| 3 | 音频采集精度不足 | QTimer → SDL回调 | audioread.cpp |
| 4 | 音频编码升级 | Speex → Opus(VOIP+DTX+FEC) | audioread.cpp/audiowrite.cpp |
| 5 | 多人音频竞争 | mutex保护deque队列 | audiowrite.cpp |
| 6 | 视频帧队列堆积 | 300ms丢帧策略 | ckernel.cpp |
| 7 | 视频编码硬件回退 | NVENC→QSV→libx264自动切换 | h264encoder.cpp |
| 8 | RGB→YUV对齐问题 | av_image_alloc自动对齐 | h264encoder.cpp |
| 9 | H264 I帧超1MB | 4KB分片传输+重组 | ckernel.cpp/h264decoder.cpp |
| 10 | 新成员画面花屏 | PLI关键帧请求机制 | ckernel.cpp/h264encoder.cpp |
| 11 | AVFrame use-after-free | 引用计数零拷贝传递链 | h264decoder.cpp/usershow.cpp/yuvglwidget.cpp |
| 12 | 音视频逐渐失步 | 音频主时钟+5秒超时失效 | AVSyncManager.cpp |
| 13 | 新用户同步异常 | audioClockValid标志位 | AVSyncManager.cpp |
| 14 | OpenGL首帧闪烁 | 首帧glTexImage2D + 后续glTexSubImage2D | yuvglwidget.cpp |
| 15 | 摄像头打开失败 | 三重回退(MSMF→DirectShow→默认) | videoread.cpp |
| 16 | 密码明文传输 | MD5加盐哈希 | md5.cpp/ckernel.cpp |
| 17 | 网络阻塞UI | 异步线程初始化 + 子线程发送 | ckernel.cpp |
| 18 | config.ini IP覆盖默认值 | initConfig()读取后覆盖 | ckernel.cpp |
| 19 | 登录回复包类型错误 | m_nType修正为DEF_PACK_LOGIN_RS | packdef.h |
| 20 | 服务端MySQL未启动 | 检查systemctl + 数据库初始化 | 排查文档 |
| 21 | 静音检测误触发/带宽浪费 | WebRTC VAD 动态阈值自适应 + 状态平滑(连续3帧确认) + SIG_muteStateChanged UI联动 | audioread.cpp/ckernel.cpp/roomdialog.cpp |

---

## 四、项目开发最佳实践

### 4.1 架构设计最佳实践

1. **分层解耦：** 严格遵循六层架构，UI层不直接调用网络层，通过ckernel中介者转发
2. **接口抽象：** 网络层通过INet/INetMediator接口抽象，支持TCP/UDP无缝切换
3. **三路TCP分离：** 信令、音频、视频使用独立TCP连接，避免数据阻塞
4. **协议映射表：** 函数指针数组实现O(1)协议分发，比if-else链高效且易扩展

### 4.2 音视频开发最佳实践

1. **SDL统一音频：** 采集和播放都使用SDL，架构统一，维护成本低
2. **Opus优先：** Opus在所有维度优于Speex，新项目应直接选择Opus
3. **硬件编码回退：** 始终准备软编兜底方案，硬件编码器因环境不同可能不可用
4. **引用计数零拷贝：** AVFrame传递必须遵循clone→unref→使用→free的完整链路
5. **音频主时钟：** 以音频为基准同步视频，因为人耳对音频断续更敏感

### 4.3 网络开发最佳实践

1. **TCP_NODELAY：** 实时场景必须禁用Nagle算法
2. **防粘包：** 所有TCP协议必须有长度包头
3. **大帧分片：** 超过1MB的帧必须分片传输
4. **子线程发送：** 网络发送不应阻塞主线程

### 4.4 调试最佳实践

1. **配置文件优先级：** config.ini > 代码默认值，修改IP时需同步更新config.ini
2. **日志输出：** 关键路径添加printf/qDebug，便于问题定位
3. **渐进式集成：** 先调通网络 → 再调音频 → 最后调视频，逐步集成

---

## 五、经验教训

### 5.1 架构层面

| 教训 | 具体表现 | 改进方向 |
|------|----------|----------|
| **初期未考虑H264** | 最初使用JPEG编码，后期升级需大量修改 | 技术选型时应预留扩展空间 |
| **协议设计未预留版本号** | 新旧客户端无法兼容 | 协议包头应包含版本字段 |
| **服务端协议与客户端不一致** | 服务端缺少H264/PLI处理函数 | 协议变更时两端同步更新 |
| **config.ini管理混乱** | 多个构建目录存在不同版本的config.ini | 统一配置管理，使用相对路径 |

### 5.2 开发流程层面

| 教训 | 具体表现 | 改进方向 |
|------|----------|----------|
| **后期才做音视频同步** | 前期各模块独立开发，集成时发现同步问题 | 同步方案应在架构设计阶段确定 |
| **缺少单元测试** | Bug主要靠手动测试发现 | 关键模块（编解码、协议解析）应有单元测试 |
| **代码修改未保留历史** | 直接删除旧代码，难以回溯 | 使用注释保留旧代码（本次已修正） |

### 5.3 技术细节层面

| 教训 | 具体表现 | 改进方向 |
|------|----------|----------|
| **AVFrame生命周期管理** | 多次出现use-after-free | 制定明确的所有权规则文档 |
| **SDL与Qt主线程冲突** | SDL_Init影响Qt事件循环 | SDL仅初始化音频子系统 |
| **WebRTC VAD采样率限制** | 48kHz下需要特殊处理 | 使用前先验证采样率兼容性 |

---

## 六、未来改进方向

### 6.1 功能扩展

| 改进项 | 优先级 | 说明 |
|--------|--------|------|
| 文字聊天 | P1 | 房间内文字消息功能 |
| 文件传输 | P2 | 房间内文件共享 |
| 屏幕标注 | P2 | 共享屏幕时支持画笔标注 |
| 录制功能 | P2 | 会议录制为本地文件 |
| 回声消除(AEC) | P1 | WebRTC AEC模块集成 |
| 自适应码率 | P1 | 根据网络状况动态调整编码码率 |

### 6.2 性能优化

| 改进项 | 优先级 | 说明 |
|--------|--------|------|
| UDP+RTP传输 | P1 | 音视频数据改用UDP+RTP，降低延迟 |
| SFU架构 | P2 | 服务端选择性转发，替代全量广播 |
| SVC分层编码 | P3 | H264 SVC，不同用户接收不同质量层 |
| WebRTC集成 | P2 | 考虑直接使用WebRTC协议栈 |

### 6.3 工程改进

| 改进项 | 优先级 | 说明 |
|--------|--------|------|
| 单元测试框架 | P1 | Google Test集成 |
| CI/CD流水线 | P2 | 自动编译+测试+部署 |
| 日志系统升级 | P1 | 替换printf为结构化日志（spdlog） |
| 配置管理 | P2 | 统一配置文件，支持命令行参数覆盖 |
| 跨平台支持 | P3 | Linux/macOS客户端适配 |

---

## 七、项目文件结构

```
VCC视频会议/
├── AudioApi/                    # 音频模块
│   ├── audioread.h/cpp          # SDL采集 + Opus编码
│   ├── audiowrite.h/cpp         # Opus解码 + SDL播放
│   ├── World.h                  # 全局头文件
│   ├── opus/                    # Opus库（1.4）
│   ├── SDL2-2.0.10/             # SDL2库
│   ├── speex/                   # Speex库（已废弃，保留兼容）
│   └── WebRtc_Vad/              # WebRTC VAD模块
├── VideoApi/                    # 视频模块
│   ├── videoread.h/cpp          # OpenCV摄像头采集
│   ├── screenread.h/cpp         # 桌面截图
│   ├── AVSyncManager.h/cpp      # 音视频同步管理器
│   └── face_recognition/        # 人脸识别
├── VideoRender/                 # 视频编解码与渲染
│   ├── h264encoder.h/cpp        # H264编码器
│   ├── h264decoder.h/cpp        # H264解码器
│   ├── yuvglwidget.h/cpp        # OpenGL YUV渲染
│   ├── myopenglwidget.h/cpp     # OpenGL QImage渲染
│   └── usershow.h/cpp           # 用户视频显示控件
├── netapi/                      # 网络模块
│   ├── net/                     # TCP/UDP实现
│   │   ├── INet.h               # 网络接口
│   │   ├── TcpClient.h/cpp      # TCP客户端
│   │   ├── TcpServer.h/cpp      # TCP服务端
│   │   ├── UdpNet.h/cpp         # UDP
│   │   └── packdef.h            # 协议定义
│   ├── mediator/                # 中介者
│   │   ├── INetMediator.h       # 中介者接口
│   │   ├── TcpClientMediator.h/cpp
│   │   └── TcpServerMediator.h/cpp
│   └── tools/                   # 工具类
│       ├── md5.h/cpp            # MD5加盐哈希
│       └── ThreadWorker.h       # 线程工作基类
├── VCC视频会议Dialog/                # UI界面
│   ├── logindialog.h/cpp        # 登录注册界面
│   ├── VCC视频会议dialog.h/cpp       # 主界面
│   └── roomdialog.h/cpp         # 会议房间界面
├── ckernel.h/cpp                # 核心业务控制器
└── VCC视频会议.pro                   # Qt项目文件

server/
├── include/
│   ├── clogic.h                 # 业务逻辑头文件
│   ├── packdef.h                # 协议定义
│   ├── TCPKernel.h              # 网络内核
│   ├── block_epoll_net.h        # epoll网络
│   └── Mysql.h                  # MySQL封装
├── src/
│   ├── clogic.cpp               # 业务逻辑实现
│   ├── TCPKernel.cpp            # 网络内核实现
│   ├── block_epoll_net.cpp      # epoll实现
│   └── Mysql.cpp                # MySQL实现
└── main.cpp                     # 服务端入口
```

---

## 八、协议包格式速查表

| 协议名 | 包类型 | 发送方 | 接收方 | 关键字段 |
|--------|--------|--------|--------|----------|
| LOGIN_RQ | 0+1 | 客户端 | 服务端 | tel, password |
| LOGIN_RS | 0+2 | 服务端 | 客户端 | result, userId, userName, icon |
| REGISTER_RQ | 0+3 | 客户端 | 服务端 | tel, password, name |
| REGISTER_RS | 0+4 | 服务端 | 客户端 | result |
| CREATEROOM_RQ | 0+5 | 客户端 | 服务端 | userId |
| CREATEROOM_RS | 0+6 | 服务端 | 客户端 | roomId, 成员列表 |
| JOINROOM_RQ | 0+7 | 客户端 | 服务端 | userId, roomId |
| JOINROOM_RS | 0+8 | 服务端 | 客户端 | roomId, 成员列表 |
| LEAVEROOM_RQ | 0+9 | 客户端 | 服务端 | userId |
| LEAVEROOM_RS | 0+10 | 服务端 | 客户端 | userId |
| ROOMMEMBER_RQ | 0+11 | 服务端 | 客户端 | userId, userName, icon |
| AUDIOFRAME | 0+12 | 客户端 | 服务端(转发) | userId, roomId, timestamp, audioData |
| VIDEOFRAME | 0+13 | 客户端 | 服务端(转发) | userId, roomId, timestamp, jpegData |
| VIDEOFRAME_H264 | 0+17 | 客户端 | 服务端(转发) | userId, roomId, frameType, frameSeq, fragments, h264Data |
| PLI_REQUEST | 0+18 | 客户端 | 服务端(转发) | userId, roomId |

---

> **文档版本：** v1.1
> **最后更新：** 2026-08-01
> **v1.1更新摘要：** 增补已落地的基于 WebRTC VAD 的静音检测功能（功能需求表新增条目、音频模块开发新增实现说明、性能优化记录新增静音带宽优化、技术难点汇总新增第 21 项）。
> **适用范围：** VCC视频会议 音视频会议系统项目团队
