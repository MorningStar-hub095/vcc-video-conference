# WeChat 音视频会议系统 - 项目类图（完整重构版）

> **文档版本：** v2.1
> **最后更新：** 2026-08-01
> **更新说明：** 新增 WebRTC VAD 静音检测（动态阈值/状态平滑/UI联动）相关成员与方法
> **重构说明：** 基于源码头文件全面重构，完整标注每个类的属性（名称、类型、可见性）和方法（名称、参数、返回值、可见性），使用标准 UML 符号标识所有类间关系。

---

## 一、整体架构分层

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          UI 层（Qt Widgets 用户界面）                         │
│  LoginDialog(QDialog)  WeChatDialog(QDialog)  RoomDialog(QDialog)           │
│  UserShow(QWidget)     MyOpenGLWidget(QOpenGLWidget)                        │
├─────────────────────────────────────────────────────────────────────────────┤
│                       核心调度层（业务中枢 - 单例模式）                         │
│                              ckernel(QObject)                               │
│  协议映射表 PFUN m_netPackMap[100] │ 三路TCP连接 │ 模块生命周期管理            │
├──────────┬──────────┬───────────┬──────────┬────────────────────────────────┤
│  音频层   │  视频采集层│ 视频编解码层│  渲染层   │    同步层                     │
│AudioRead │VideoRead │H264Encoder│YuvGLWidget│  AVSyncManager(单例)          │
│AudioWrite│ScreenRead│H264Decoder│MyOpenGLW │  SyncContext(结构体)           │
│(SDL+Opus)│(OpenCV)  │(FFmpeg)   │(OpenGL)   │  (音频主时钟策略)               │
├──────────┴──────────┴───────────┴──────────┴────────────────────────────────┤
│                    网络传输层（接口抽象 + 中介者模式）                          │
│  INet(抽象接口) ──实现── TcpClient / TcpServer / UdpNet                     │
│  INetMediator(抽象中介者) ──继承── TcpClientMed / TcpServerMed / UdpMed     │
├─────────────────────────────────────────────────────────────────────────────┤
│                         工具/协议层                                          │
│  ThreadWorker(QObject)  SendVideoWorker  MD5  packdef.h(协议结构体)         │
└─────────────────────────────────────────────────────────────────────────────┘
```

### UML 关系符号说明

| 符号 | 关系类型 | 说明 |
|------|---------|------|
| `<|--` | 继承（泛化） | 子类继承父类，实线空心三角 |
| `<|..` | 实现（接口） | 类实现接口，虚线空心三角 |
| `*--` | 组合（Composition） | 强拥有关系，部分随整体创建/销毁，实线实心菱形 |
| `o--` | 聚合（Aggregation） | 弱拥有关系，部分可独立于整体存在，实线空心菱形 |
| `-->` | 关联（Association） | 类之间有结构关系，实线箭头 |
| `..>` | 依赖（Dependency） | 类之间有使用关系，虚线箭头 |

### 可见性符号说明

| 符号 | 可见性 | 说明 |
|------|--------|------|
| `+` | public | 公有访问 |
| `-` | private | 私有访问 |
| `#` | protected | 受保护访问 |

---

## 二、完整类图（Mermaid 格式）

### 2.1 核心调度层

```mermaid
classDiagram
    direction TB

    class ckernel {
        <<singleton, QObject>>
        -WeChatDialog* m_pWeChatDlg
        -LoginDialog* m_pLoginDlg
        -RoomDialog* m_pRoomdialog
        -INetMediator* m_pClient
        -INetMediator* m_pAVClient[2]
        -QString m_serverIP
        -PFUN m_netPackMap[100]
        -int m_id
        -int m_roomid
        -QString m_UserName
        -AudioRead* m_pAudioRead
        -map~int,AudioWrite*~ m_mapIDToAudioWrite
        -VideoRead* m_pVideoRead
        -ScreenRead* m_pScreenRead
        -QSharedPointer~SendVideoWorker~ m_pSendVideoWorker
        -H264Encoder* m_h264Encoder
        -map~int,H264Decoder*~ m_mapIDToH264Decoder
        -bool m_useH264
        -int m_h264FrameSeq
        -bool m_encodingPending
        -map~int,map~int,H264FragmentGroup~~ m_h264Reassemble
        +ckernel(QObject* parent)
        +static ckernel* GetInstance()
        +void setNetPackMap()
        +void initConfig()
        +void slot_destroy()
        +void slot_loginCommit(QString tel, QString pass)
        +void slot_registerCommit(QString tel, QString pass, QString name, QString email)
        +void slot_createroom()
        +void slot_joinroom()
        +void slot_quitRoom()
        +void slot_pauseAudio()
        +void slot_startAudio()
        +void slot_pauseVideo()
        +void slot_startVideo()
        +void slot_startScreen()
        +void slot_pauseScreen()
        +void slot_refreshVideo(int id, QImage& img)
        +void slot_audioFrame(QByteArray ba)
        +void slot_muteStateChanged(bool isMute)
        +void slot_sendVideoFrame(QImage img)
        +void encodeAndSendVideoFrame(QImage img)
        +void slot_SendVideo(char* buf, int nlen)
        +void slot_dealData(uint sock, char* buf, int nlen)
        +void slot_dealLoginRS(uint sock, char* buf, int nlen)
        +void slot_dealRegisterRS(uint sock, char* buf, int nlen)
        +void slot_dealCreateRoomRS(uint sock, char* buf, int nlen)
        +void slot_dealJoinRoomRS(uint sock, char* buf, int nlen)
        +void slot_dealRoomMemberRq(uint sock, char* buf, int nlen)
        +void slot_dealLeaveRoomRq(uint sock, char* buf, int nlen)
        +void slot_dealAudioFrameRq(uint sock, char* buf, int nlen)
        +void slot_dealVideoFrameRq(uint sock, char* buf, int nlen)
        +void slot_dealVideoFrameH264Rq(uint sock, char* buf, int nlen)
        +void slot_dealPliRequest(uint sock, char* buf, int nlen)
        +void slot_h264FrameDecoded(int userId, AVFrame* frame)
        +void slot_dealUpdateEmailRS(uint sock, char* buf, int nlen)
        ~void SIG_SendVideo(char* buf, int nlen)~
    }

    class H264FragmentGroup {
        +int totalFragments
        +int receivedCount
        +QMap~int,QByteArray~ fragments
        +qint64 timestamp
        +int frameType
    }

    class PFUN {
        <<typedef>>
        void (ckernel::*)(uint sock, char* buf, int nlen)
    }

    ckernel *-- H264FragmentGroup : m_h264Reassemble 内部结构
    ckernel ..> PFUN : m_netPackMap 类型

    note for ckernel "单例模式（Meyers' Singleton）\nC++11 线程安全局部静态变量\n协议映射表 m_netPackMap 实现 O(1) 消息分发\n策略模式：PFUN 函数指针数组"
```

### 2.2 网络传输层（接口抽象 + 中介者模式）

```mermaid
classDiagram
    direction TB

    class INet {
        <<abstract, interface>>
        #INetMediator* m_pMediator
        -static bool m_isLoadlib
        +INet()
        +virtual ~INet()
        +virtual bool InitNet(const char* szBufIP, unsigned short port)*
        +virtual void UnInitNet()*
        +virtual bool SendData(unsigned int lSendIP, char* buf, int nlen)*
        +virtual bool IsConnected()
        +static string GetIPString(long ip)
        +static set~long~ GetValidIPList()
        #virtual void RecvData()*
    }

    class TcpClient {
        -SOCKET m_sock
        -HANDLE m_hThreadHandle
        -bool m_isStop
        -atomic~bool~ m_isConnected
        +TcpClient(INetMediator* pMediator)
        +~TcpClient()
        +bool InitNet(const char* szBufIP, unsigned short port)
        +void UnInitNet()
        +bool SendData(unsigned int lSendIP, char* buf, int nlen)
        +bool IsConnected()
        #static unsigned int __stdcall RecvThread(void* lpvoid)
        #virtual void RecvData()
    }

    class TcpServer {
        -SOCKET m_sock
        -map~uint,SOCKET~ m_mapThreadIdToSocket
        -list~HANDLE~ m_hThreadHandleLst
        -bool m_isStop
        +TcpServer(INetMediator* pMediator)
        +~TcpServer()
        +bool InitNet(const char* szBufIP, unsigned short port)
        +void UnInitNet()
        +bool SendData(unsigned int lSendIP, char* buf, int nlen)
        #static unsigned int __stdcall RecvThread(void* lpvoid)
        #static unsigned int __stdcall AcceptThread(void* lpvoid)
        #virtual void RecvData()
    }

    class UdpNet {
        -SOCKET m_sock
        -HANDLE m_hThreadHandle
        -bool m_isStop
        -int m_port
        +UdpNet(INetMediator* pMediator)
        +~UdpNet()
        +bool InitNet(const char* szBufIP, unsigned short port)
        +void UnInitNet()
        +bool SendData(unsigned int lSendIP, char* buf, int nlen)
        #static unsigned int __stdcall RecvThread(void* lpvoid)
        #virtual void RecvData()
    }

    class INetMediator {
        <<abstract, QObject>>
        #INet* m_pNet
        #char m_szBufIP[18]
        #unsigned short m_port
        +INetMediator()
        +virtual ~INetMediator()
        +virtual bool OpenNet(const char* szBufIP, unsigned short port)*
        +virtual void CloseNet()*
        +virtual bool SendData(unsigned int lSendIP, char* buf, int nlen)*
        +virtual void DealData(unsigned int lSendIP, char* buf, int nlen)*
        +void setIpAndPort(const char* szBufIP, unsigned short port)
        +virtual bool IsConnected()
        +virtual void disConnect()
        ~void SIG_ReadyData(uint lSendIP, char* buf, int nlen)~
        ~void SIG_disConnect()~
    }

    class TcpClientMediator {
        +TcpClientMediator()
        +~TcpClientMediator()
        +bool OpenNet(const char* szBufIP, unsigned short port)
        +void CloseNet()
        +bool SendData(unsigned int lSendIP, char* buf, int nlen)
        +void DealData(unsigned int lSendIP, char* buf, int nlen)
        +void disConnect()
        +bool IsConnected()
    }

    class TcpServerMediator {
        +TcpServerMediator()
        +~TcpServerMediator()
        +bool OpenNet(const char* szBufIP, unsigned short port)
        +void CloseNet()
        +bool SendData(unsigned int lSendIP, char* buf, int nlen)
        +void DealData(unsigned int lSendIP, char* buf, int nlen)
    }

    class UdpMediator {
        +UdpMediator()
        +~UdpMediator()
        +bool OpenNet(const char* szBufIP, unsigned short port)
        +void CloseNet()
        +bool SendData(unsigned int lSendIP, char* buf, int nlen)
        +void DealData(unsigned int lSendIP, char* buf, int nlen)
    }

    INet <|-- TcpClient : 继承
    INet <|-- TcpServer : 继承
    INet <|-- UdpNet : 继承

    INetMediator <|-- TcpClientMediator : 继承
    INetMediator <|-- TcpServerMediator : 继承
    INetMediator <|-- UdpMediator : 继承

    INetMediator o-- INet : m_pNet 聚合
    INet --> INetMediator : m_pMediator 关联

    TcpClientMediator ..> TcpClient : creates 依赖
    TcpServerMediator ..> TcpServer : creates 依赖
    UdpMediator ..> UdpNet : creates 依赖

    note for INet "接口模式（Interface Pattern）\n纯虚函数定义网络层抽象\nInitNet/SendData/UnInitNet/RecvData\n支持 TCP/UDP 无缝切换"
    note for INetMediator "中介者模式（Mediator Pattern）\n桥接网络层与业务层\n通过 Qt 信号 SIG_ReadyData 解耦\nDealData() 将数据向上传递"
```

### 2.3 音频层（SDL2 + Opus）

```mermaid
classDiagram
    direction TB

    class AudioRead {
        <<QObject>>
        -SDL_AudioDeviceID m_sdlCaptureDev
        -bool m_sdlIsOpen
        -OpusEncoder* m_opusEncoder
        -VadInst* handle
        -int m_AudioState
        -int m_vadMode
        -float m_noiseLevel
        -float m_noiseLevelEMA
        -int m_consecutiveSilentFrames
        -int m_consecutiveSpeechFrames
        -QTimer* m_vadAdjustTimer
        -bool m_currentMuteState
        -bool m_pendingMuteState
        -int m_muteConfirmFrames
        -static const int MUTE_CONFIRM_THRESHOLD
        +AudioRead(QObject* parent)
        +~AudioRead()
        +void start()
        +void pause()
        +void slot_adjustVadMode()
        -float calculateRMS(int16_t* data, int samples)
        -static void audioCallback(void* userdata, Uint8* stream, int len)
        ~void SIG_audioFrame(QByteArray bt)~
        ~void SIG_muteStateChanged(bool isMute)~
    }

    class AudioWrite {
        <<QObject>>
        -SDL_AudioDeviceID m_sdlDev
        -bool m_sdlIsOpen
        -deque~QByteArray~ m_audioQueue
        -mutex m_mutex
        -OpusDecoder* m_opusDecoder
        +AudioWrite(QObject* parent)
        +~AudioWrite()
        +void slot_playAudio(QByteArray bt)
        -static void audioCallback(void* userdata, Uint8* stream, int len)
    }

    note for AudioRead "音频采集（SDL 线程驱动）\nSDL 回调每 20ms 采集 960 采样\nWebRTC VAD 静音检测 → Opus 编码\n编码后 60-120 字节/帧"
    note for AudioWrite "音频播放（生产者-消费者模式）\nslot_playAudio: mutex 加锁入队 deque\naudioCallback: mutex 加锁出队 → Opus 解码\nSDL_MixAudioFormat 混音播放"
```

### 2.4 视频采集层（OpenCV + Qt Screen）

```mermaid
classDiagram
    direction TB

    class VideoRead {
        <<QObject>>
        -QTimer* m_timer
        -cv::VideoCapture cap
        +VideoRead(QObject* parent)
        +~VideoRead()
        +void slot_openVideo()
        +void slot_closeVideo()
        +void slot_getVideoFrame()
        ~void SIG_sendVideoFrame(QImage img)~
    }

    class ScreenRead {
        <<QObject>>
        -QTimer* m_timer
        +ScreenRead(QObject* parent)
        +~ScreenRead()
        +void slot_openVideo()
        +void slot_closeVideo()
        +void slot_getScreenFrame()
        ~void SIG_getScreenFrame(QImage img)~
    }

    class MyFaceDetect {
        <<QObject>>
        +MyFaceDetect()
        +FaceDetectInit()
        +detectAndDisplay(Mat frame, vector~Rect~ faces)
    }

    VideoRead ..> MyFaceDetect : optional 依赖

    note for VideoRead "摄像头采集\nOpenCV VideoCapture 多后端支持\nQTimer 每 23ms 触发采集\nBGR→RGB→scaled(640×480)\n缩放模式: Qt::IgnoreAspectRatio"
    note for ScreenRead "桌面共享采集\nQScreen::grabWindow(0) 截图\nQTimer 每 40ms 触发（25fps）"
```

### 2.5 视频编解码层（FFmpeg H.264）

```mermaid
classDiagram
    direction TB

    class H264Encoder {
        <<QObject>>
        -AVCodecContext* m_codecCtx
        -AVFrame* m_frame
        -AVPacket* m_pkt
        -SwsContext* m_swsCtx
        -int m_width
        -int m_height
        -int m_fps
        -int64_t m_frameIndex
        -bool m_requestKeyFrame
        -QString m_encoderName
        +H264Encoder(QObject* parent)
        +~H264Encoder()
        +bool init(int width, int height, int fps, int bitrate)
        +QByteArray encode(const QImage& img)
        +void requestKeyFrame()
        +void close()
        +bool isReady() const
        +int getWidth() const
        +int getHeight() const
        -bool initCodec(int width, int height, int fps, int bitrate)
        -void freeResources()
        ~void SIG_encoded(const QByteArray& h264Data, bool isKeyFrame)~
    }

    class H264Decoder {
        <<QObject>>
        -AVCodecContext* m_codecCtx
        -AVFrame* m_frame
        -AVPacket* m_pkt
        -bool m_initialized
        +H264Decoder(QObject* parent)
        +~H264Decoder()
        +bool init()
        +AVFrame* decode(const QByteArray& h264Data)
        +void close()
        +bool isReady() const
        -bool initCodec()
        -void freeResources()
        ~void SIG_decoded(AVFrame* frame)~
    }

    note for H264Encoder "H.264 编码器封装\nQImage(RGB888) → sws_scale → YUV420P\navcodec_send_frame → avcodec_receive_packet\n帧缓冲: av_frame_get_buffer + av_frame_make_writable\n编码前 QImage 深拷贝: img.copy()\n防多线程竞态"
    note for H264Decoder "H.264 解码器封装\n每房间成员独立一个实例（map 映射）\n延迟初始化：首次 decode 时创建上下文\nav_frame_clone 零拷贝跨线程传递\n引用计数式 AVFrame 输出"
```

### 2.6 渲染层（OpenGL）

```mermaid
classDiagram
    direction TB

    class YuvGLWidget {
        <<QOpenGLWidget, QOpenGLFunctions>>
        -GLuint m_texY
        -GLuint m_texU
        -GLuint m_texV
        -bool m_texturesAllocated
        -int m_texWidth
        -int m_texHeight
        -QOpenGLShaderProgram* m_shader
        -QOpenGLVertexArrayObject* m_vao
        -QOpenGLBuffer* m_vbo
        -AVFrame* m_frame
        -QImage m_image
        -bool m_useImageMode
        +YuvGLWidget(QWidget* parent)
        +~YuvGLWidget()
        +void slot_setFrame(AVFrame* frame)
        +void slot_setImage(const QImage& img)
        #void initializeGL() override
        #void paintGL() override
        #void resizeGL(int w, int h) override
        -bool initShader()
        -void initQuad()
        -void initTextures()
        -void uploadYuvTextures(AVFrame* frame)
        -void cleanupGL()
        -void computeVertexCoords(float& halfW, float& halfH)
    }

    class MyOpenGLWidget {
        <<QOpenGLWidget, QOpenGLFunctions>>
        -QImage m_image
        -GLuint m_texture
        +MyOpenGLWidget(QWidget* parent)
        +void slot_setImage(QImage img)
        #void initializeGL() override
        #void paintGL() override
        #void resizeGL(int w, int h) override
        #QSize getImageScaledSize(QSize size)
    }

    note for YuvGLWidget "YUV420P GPU 渲染控件\n接收 AVFrame，分别上传 Y/U/V 三张纹理\nFragment Shader 中 BT.601 矩阵 YUV→RGB\n首帧 glTexImage2D，后续 glTexSubImage2D\nVAO/VBO + Shader（现代 OpenGL）"
    note for MyOpenGLWidget "QImage RGBA 纹理渲染控件\n适用于 JPEG 帧、桌面共享帧\n非 H.264 路径的渲染控件"
```

### 2.7 音视频同步层（单例模式）

```mermaid
classDiagram
    direction TB

    class AVSyncManager {
        <<singleton>>
        -map~int,SyncContext~ m_syncContexts
        -mutex m_mutex
        -double m_localAudioClock
        -double m_localVideoClock
        -mutex m_localMutex
        -static const double SYNC_THRESHOLD_US
        -static const int MAX_WAIT_MS
        -static const int CLOCK_TIMEOUT_MS
        +static AVSyncManager& getInstance()
        +void addUser(int userId)
        +void removeUser(int userId)
        +void updateAudioClock(int userId, double pts)
        +int synchronizeVideo(int userId, double video_pts, double frameDelay)
        +int synchronizeLocalVideo(double video_pts, double frameDelay)
        +double getAudioClock(int userId)
        +double getVideoClock(int userId)
        +void resetUserClock(int userId)
        +void resetAllClocks()
        +void pauseUser(int userId)
        +void resumeUser(int userId)
        +void updateLocalAudioClock(double pts)
        +double getLocalAudioClock()
        +static qint64 getCurrentTimeUs()
        -AVSyncManager()
        -SyncContext& getOrCreateContext(int userId)
        -int computeSyncDelay(SyncContext& ctx, double video_pts, double frameDelay)
    }

    class SyncContext {
        <<struct>>
        +double audioClock
        +double videoClock
        +qint64 lastAudioUpdateTime
        +qint64 lastVideoUpdateTime
        +bool isPaused
        +SyncContext()
    }

    AVSyncManager *-- "0..*" SyncContext : m_syncContexts 组合

    note for AVSyncManager "单例模式（Meyers' Singleton）\nC++11 线程安全局部静态变量\n策略：音频主时钟，视频跟随音频\n每用户独立时钟上下文（SyncContext）\n线程安全：std::mutex 保护共享数据"
    note for SyncContext "每远端用户独立维护一套 audio_clock / video_clock\naudioClock: 音频主时钟（微秒）\nvideoClock: 视频辅助时钟（微秒）\nisPaused: 该用户是否暂停同步"
```

### 2.8 UI 层

```mermaid
classDiagram
    direction TB

    class LoginDialog {
        <<QDialog>>
        -Ui::LoginDialog* ui
        +LoginDialog(QWidget* parent)
        +~LoginDialog()
        +void closeEvent(QCloseEvent* event)
        -void on_pb_commit_clicked()
        -void on_pb_clear_register_clicked()
        -void on_pb_commit_register_clicked()
        -void on_pb_clear_clicked()
        -void on_cb_login_type_currentIndexChanged(int index)
        ~void SIG_loginCommit(QString tel, QString password)~
        ~void SIG_registerCommit(QString tel, QString password, QString name, QString email)~
        ~void SIG_close()~
    }

    class WeChatDialog {
        <<QDialog>>
        -Ui::WeChatDialog* ui
        +WeChatDialog(QWidget* parent)
        +~WeChatDialog()
        +void closeEvent(QCloseEvent* event)
        +void setInfo(QString name, int icon)
        -void on_pb_create_clicked()
        -void on_pb_join_clicked()
        -void on_pb_book_clicked()
        -void on_pb_set_clicked()
        ~void SIG_close()~
        ~void SIG_createRoom()~
        ~void SIG_joinRoom()~
        ~void SIG_bookRoom()~
        ~void SIG_settings()~
    }

    class RoomDialog {
        <<QDialog>>
        -Ui::RoomDialog* ui
        -QVBoxLayout* m_mainLayout
        -map~int,UserShow*~ m_mapIDToUserShow
        -QRect m_normalGeometry
        -bool m_isMaximized
        +RoomDialog(QWidget* parent)
        +~RoomDialog()
        +void slot_setInfo(QString roomid)
        +void slot_addUserShow(UserShow* user)
        +void slot_refreshUser(int id, QImage& img)
        +void slot_refreshUserYuv(int id, AVFrame* frame)
        +void slot_removeUserShow(UserShow* user)
        +void slot_removeUserShow(int id)
        +void slot_setAudioCheck(bool check)
        +void updateMuteStatus(bool isMute)
        +void slot_clearUserShow()
        +void slot_setVideoCheck(bool check)
        +void slot_setBigImgId(int id, QString name)
        +void slot_setScreenCheck(bool check)
        +bool slot_hasUser(int id)
        -void on_pb_close_clicked()
        -void on_pb_quit_clicked()
        -void closeEvent(QCloseEvent* event)
        -void on_cb_audio_clicked()
        -void on_cb_video_clicked()
        -void on_cb_desk_clicked()
        -void on_pb_max_clicked()
        -void on_pb_min_clicked()
        ~void SIG_close()~
        ~void SIG_AudioPause()~
        ~void SIG_AudioStart()~
        ~void SIG_VideoStart()~
        ~void SIG_VideoPause()~
        ~void SIG_ScreenStart()~
        ~void SIG_ScreenPause()~
    }

    class UserShow {
        <<QWidget>>
        -Ui::UserShow* ui
        -int m_id
        -QString m_UserName
        -QImage m_img
        -QImage m_defaultImg
        -QTimer m_timer
        -QTime m_lastTime
        -YuvGLWidget* m_glWidget
        +UserShow(QWidget* parent)
        +~UserShow()
        +int getId() const
        +void slot_setInfo(int id, QString name)
        +void paintEvent(QPaintEvent* event)
        +void slot_setImage(QImage& img)
        +void slot_setFrame(AVFrame* frame)
        +void mousePressEvent(QMouseEvent* event)
        +void resizeEvent(QResizeEvent* event)
        +void slot_checkTimer()
        ~void SIG_itemClicked(int id, QString name)~
    }

    RoomDialog o-- "1..*" UserShow : m_mapIDToUserShow 聚合
    UserShow *-- "0..1" YuvGLWidget : m_glWidget 组合

    note for UserShow "用户视频显示控件\n双模式渲染：\n  slot_setImage(QImage) → QPainter 绘制\n  slot_setFrame(AVFrame) → YuvGLWidget GPU 渲染\npaintEvent 中 m_img.scaled() 使用深拷贝数据\n防止 Qt 隐式共享浅拷贝导致的竞态崩溃"
    note for RoomDialog "房间界面控制器\n管理多个 UserShow 控件（map 映射）\nslot_refreshUser: JPEG 路径（QImage）\nslot_refreshUserYuv: H.264 路径（AVFrame）\n双路径共存，由 ckernel 按编码模式选择"
```

### 2.9 工具/线程层

```mermaid
classDiagram
    direction TB

    class ThreadWorker {
        <<QObject>>
        #QThread* m_pThread
        +ThreadWorker(QObject* parent)
        +~ThreadWorker()
    }

    class SendVideoWorker {
        <<ThreadWorker>>
        +void slot_SendVideo(char* buf, int nlen)
    }

    class worker {
        <<ThreadWorker>>
        +~worker()
        +void slot_doWork()
    }

    class MD5 {
        -ulong _state[4]
        -ulong _count[2]
        -byte _buffer[64]
        -byte _digest[16]
        -bool _finished
        -static const byte PADDING[64]
        -static const char HEX[16]
        -static const size_t BUFFER_SIZE
        +MD5()
        +MD5(const void* input, size_t length)
        +MD5(const string& str)
        +MD5(ifstream& in)
        +void update(const void* input, size_t length)
        +void update(const string& str)
        +void update(ifstream& in)
        +const byte* digest()
        +string toString()
        +void reset()
        -void update(const byte* input, size_t length)
        -void final()
        -void transform(const byte block[64])
        -void encode(const ulong* input, byte* output, size_t length)
        -void decode(const byte* input, ulong* output, size_t length)
        -string bytesToHexString(const byte* input, size_t length)
    }

    ThreadWorker <|-- SendVideoWorker : 继承
    ThreadWorker <|-- worker : 继承

    note for ThreadWorker "模板方法模式（Template Method）\n基类管理 QThread 生命周期\n子类通过 moveToThread 移入工作线程\n信号驱动工作（slot_doWork/slot_SendVideo）"
    note for SendVideoWorker "视频发送工作线程\nslot_SendVideo 转调 ckernel::slot_SendVideo\n将阻塞式 SendData 从 UI 线程移至工作线程"
    note for MD5 "MD5 哈希工具类\n不可拷贝（private 拷贝构造）\n用于密码加盐哈希\n密钥: vC0nF2024$SecR1t^S4ltK3y!"
```

---

## 三、完整类关系总图

```mermaid
classDiagram
    direction TB

    %% ===== 核心调度层 =====
    class ckernel {
        <<singleton, QObject>>
        -INetMediator* m_pClient
        -INetMediator* m_pAVClient[2]
        -WeChatDialog* m_pWeChatDlg
        -LoginDialog* m_pLoginDlg
        -RoomDialog* m_pRoomdialog
        -AudioRead* m_pAudioRead
        -map~int,AudioWrite*~ m_mapIDToAudioWrite
        -VideoRead* m_pVideoRead
        -ScreenRead* m_pScreenRead
        -H264Encoder* m_h264Encoder
        -map~int,H264Decoder*~ m_mapIDToH264Decoder
        -QSharedPointer~SendVideoWorker~ m_pSendVideoWorker
        -bool m_useH264
        -int m_h264FrameSeq
        -bool m_encodingPending
        -PFUN m_netPackMap[100]
        -int m_id
        -int m_roomid
        -QString m_UserName
        -QString m_serverIP
        +static ckernel* GetInstance()
        +void slot_muteStateChanged(bool isMute)
        +void slot_sendVideoFrame(QImage img)
        +void encodeAndSendVideoFrame(QImage img)
        +void slot_dealData(uint sock, char* buf, int nlen)
        +void slot_dealVideoFrameH264Rq(uint sock, char* buf, int nlen)
        +void slot_dealPliRequest(uint sock, char* buf, int nlen)
        +void slot_h264FrameDecoded(int userId, AVFrame* frame)
    }

    %% ===== 网络层 =====
    class INet {
        <<abstract>>
        #INetMediator* m_pMediator
        +virtual bool InitNet(const char*, unsigned short)*
        +virtual void UnInitNet()*
        +virtual bool SendData(uint, char*, int)*
        #virtual void RecvData()*
    }
    class TcpClient {
        -SOCKET m_sock
        -atomic~bool~ m_isConnected
        +bool InitNet(const char*, unsigned short)
        +bool SendData(uint, char*, int)
    }
    class INetMediator {
        <<abstract, QObject>>
        #INet* m_pNet
        +virtual bool OpenNet(const char*, unsigned short)*
        +virtual void CloseNet()*
        +virtual bool SendData(uint, char*, int)*
        ~void SIG_ReadyData(uint, char*, int)~
    }
    class TcpClientMediator {
        +bool OpenNet(const char*, unsigned short)
        +bool SendData(uint, char*, int)
        +bool IsConnected()
    }

    %% ===== 音频层 =====
    class AudioRead {
        <<QObject>>
        -OpusEncoder* m_opusEncoder
        -VadInst* handle
        -int m_vadMode
        -QTimer* m_vadAdjustTimer
        +void start()
        +void pause()
        +void slot_adjustVadMode()
        ~void SIG_audioFrame(QByteArray)~
        ~void SIG_muteStateChanged(bool)~
    }
    class AudioWrite {
        <<QObject>>
        -OpusDecoder* m_opusDecoder
        -deque~QByteArray~ m_audioQueue
        -mutex m_mutex
        +void slot_playAudio(QByteArray)
    }

    %% ===== 视频采集层 =====
    class VideoRead {
        <<QObject>>
        -cv::VideoCapture cap
        +void slot_getVideoFrame()
        ~void SIG_sendVideoFrame(QImage)~
    }
    class ScreenRead {
        <<QObject>>
        +void slot_getScreenFrame()
        ~void SIG_getScreenFrame(QImage)~
    }

    %% ===== 视频编解码层 =====
    class H264Encoder {
        <<QObject>>
        -AVCodecContext* m_codecCtx
        -AVFrame* m_frame
        -SwsContext* m_swsCtx
        +bool init(int, int, int, int)
        +QByteArray encode(const QImage&)
        +void requestKeyFrame()
    }
    class H264Decoder {
        <<QObject>>
        -AVCodecContext* m_codecCtx
        -AVFrame* m_frame
        +AVFrame* decode(const QByteArray&)
        +bool isReady() const
    }

    %% ===== 渲染层 =====
    class YuvGLWidget {
        <<QOpenGLWidget>>
        -GLuint m_texY, m_texU, m_texV
        -QOpenGLShaderProgram* m_shader
        -AVFrame* m_frame
        +void slot_setFrame(AVFrame*)
        +void slot_setImage(const QImage&)
        #void paintGL()
    }
    class MyOpenGLWidget {
        <<QOpenGLWidget>>
        -QImage m_image
        -GLuint m_texture
        +void slot_setImage(QImage)
        #void paintGL()
    }

    %% ===== 同步层 =====
    class AVSyncManager {
        <<singleton>>
        -map~int,SyncContext~ m_syncContexts
        -mutex m_mutex
        +static AVSyncManager& getInstance()
        +int synchronizeVideo(int, double, double)
        +void updateAudioClock(int, double)
    }
    class SyncContext {
        <<struct>>
        +double audioClock
        +double videoClock
        +bool isPaused
    }

    %% ===== UI 层 =====
    class LoginDialog {
        <<QDialog>>
        ~void SIG_loginCommit(QString, QString)~
        ~void SIG_registerCommit(QString, QString, QString, QString)~
    }
    class WeChatDialog {
        <<QDialog>>
        ~void SIG_createRoom()~
        ~void SIG_joinRoom()~
    }
    class RoomDialog {
        <<QDialog>>
        -map~int,UserShow*~ m_mapIDToUserShow
        +void slot_addUserShow(UserShow*)
        +void slot_refreshUserYuv(int, AVFrame*)
        +void updateMuteStatus(bool isMute)
    }
    class UserShow {
        <<QWidget>>
        -QImage m_img
        -YuvGLWidget* m_glWidget
        +void slot_setImage(QImage&)
        +void slot_setFrame(AVFrame*)
    }

    %% ===== 工具层 =====
    class ThreadWorker {
        <<QObject>>
        #QThread* m_pThread
    }
    class SendVideoWorker {
        <<ThreadWorker>>
        +void slot_SendVideo(char*, int)
    }
    class MD5 {
        +string toString()
        +void update(const string&)
    }

    %% ===== 继承关系 =====
    INet <|-- TcpClient
    INetMediator <|-- TcpClientMediator
    ThreadWorker <|-- SendVideoWorker

    %% ===== 聚合/组合关系 =====
    INetMediator o-- INet : m_pNet
    ckernel o-- "1" INetMediator : m_pClient 信令连接
    ckernel o-- "2" INetMediator : m_pAVClient 音视频连接
    ckernel o-- "1" LoginDialog : m_pLoginDlg
    ckernel o-- "1" WeChatDialog : m_pWeChatDlg
    ckernel o-- "1" RoomDialog : m_pRoomdialog
    ckernel o-- "1" AudioRead : m_pAudioRead
    ckernel o-- "0..*" AudioWrite : m_mapIDToAudioWrite
    ckernel o-- "1" VideoRead : m_pVideoRead
    ckernel o-- "1" ScreenRead : m_pScreenRead
    ckernel o-- "1" H264Encoder : m_h264Encoder
    ckernel o-- "0..*" H264Decoder : m_mapIDToH264Decoder
    ckernel o-- "1" SendVideoWorker : m_pSendVideoWorker
    RoomDialog o-- "1..*" UserShow : m_mapIDToUserShow
    UserShow *-- "0..1" YuvGLWidget : m_glWidget
    AVSyncManager *-- "0..*" SyncContext : m_syncContexts

    %% ===== 依赖关系 =====
    ckernel ..> AVSyncManager : singleton 访问
    ckernel ..> MD5 : 密码哈希
    TcpClientMediator ..> TcpClient : creates
    ckernel ..> H264Encoder : 编码调度
    ckernel ..> H264Decoder : 解码管理

    note for ckernel "ckernel 是系统业务中枢，采用单例模式\n通过协议映射表 m_netPackMap 实现 O(1) 消息分发\n持有三路独立 TCP 连接（信令+音频+视频）\n管理所有子模块的生命周期"
```

---

## 四、协议结构体定义

```mermaid
classDiagram
    direction TB

    class STRU_REGISTER_RQ {
        <<struct>>
        +PackType m_nType
        +char m_tel[60]
        +char m_name[60]
        +char m_password[60]
        +char m_email[60]
    }

    class STRU_REGISTER_RS {
        <<struct>>
        +PackType m_nType
        +int m_lresult
    }

    class STRU_LOGIN_RQ {
        <<struct>>
        +PackType m_nType
        +char m_tel[40]
        +char m_password[40]
    }

    class STRU_LOGIN_RS {
        <<struct>>
        +PackType m_nType
        +int m_userid
        +int m_lresult
        +char m_name[60]
    }

    class STRU_CREATEROOM_RQ {
        <<struct>>
        +PackType m_nType
        +int m_UserID
    }

    class STRU_CREATEROOM_RS {
        <<struct>>
        +PackType m_nType
        +int m_RoomID
        +int m_lresult
    }

    class STRU_JOINROOM_RQ {
        <<struct>>
        +PackType m_nType
        +int m_UserID
        +int m_RoomID
    }

    class STRU_JOINROOM_RS {
        <<struct>>
        +PackType m_nType
        +int m_RoomID
        +int m_lresult
    }

    class STRU_ROOM_MEMBER_RQ {
        <<struct>>
        +PackType m_nType
        +int m_UserID
        +char m_szUser[60]
    }

    class STRU_LEAVEROOM_RQ {
        <<struct>>
        +PackType m_nType
        +int m_nUserID
        +int m_RoomID
        +char szUserName[60]
    }

    class STRU_AUDIO_REGISTER {
        <<struct>>
        +PackType m_nType
        +int m_userid
    }

    class STRU_VIDEO_REGISTER {
        <<struct>>
        +PackType m_nType
        +int m_userid
    }

    class STRU_UPDATE_EMAIL_RQ {
        <<struct>>
        +PackType m_nType
        +int m_userid
        +char m_email[60]
    }

    class STRU_UPDATE_EMAIL_RS {
        <<struct>>
        +PackType m_nType
        +int m_lresult
    }

    class STRU_VIDEO_FRAME_H264_HEADER {
        <<struct>>
        +PackType m_nType
        +int m_userid
        +int m_roomid
        +int m_min
        +int m_sec
        +int m_msec
        +int m_frameType
        +int m_frameSeq
        +int m_totalFragments
        +int m_fragmentIndex
    }

    class STRU_PLI_REQUEST {
        <<struct>>
        +PackType m_nType
        +int m_userid
        +int m_roomid
    }

    class UserInfo {
        <<struct>>
        +int m_sock
        +int m_id
        +int m_roomid
        +char m_userName[60]
    }

    note for STRU_VIDEO_FRAME_H264_HEADER "H.264 分片传输包头\nframeType: 0=I帧, 1=P帧\nframeSeq: 帧序号（同一帧多片共享）\ntotalFragments: 总分片数\nfragmentIndex: 当前分片（0-based）\n后接 char m_h264Data[] 可变长数据\n分片大小: H264_FRAGMENT_SIZE=4096字节"
```

---

## 五、关键业务流程 - 信号槽交互路径

### 5.1 用户登录流程

```mermaid
sequenceDiagram
    participant U as 用户
    participant LD as LoginDialog
    participant CK as ckernel
    participant NET as TcpClientMediator
    participant SVR as 服务器

    U->>LD: 输入手机号/密码
    U->>LD: 点击登录按钮
    LD->>LD: on_pb_commit_clicked()
    LD->>LD: MD5加盐哈希密码
    LD-->>CK: SIG_loginCommit(tel, password)
    CK->>CK: slot_loginCommit()
    CK->>CK: 构造 STRU_LOGIN_RQ 包
    CK->>NET: SendData(m_id, buf, len)
    NET->>SVR: TCP发送登录请求
    SVR-->>NET: TCP返回登录响应
    NET-->>CK: SIG_ReadyData(sock, buf, len)
    CK->>CK: slot_dealData → m_netPackMap 分发
    CK->>CK: slot_dealLoginRS()
    alt 登录成功
        CK->>LD: hide()
        CK->>CK: show WeChatDialog
    else 登录失败
        CK->>LD: QMessageBox提示错误
    end
```

### 5.2 创建/加入房间流程

```mermaid
sequenceDiagram
    participant U as 用户
    participant WD as WeChatDialog
    participant CK as ckernel
    participant NET as TcpClientMediator
    participant SVR as 服务器
    participant RD as RoomDialog
    participant AR as AudioRead
    participant VR as VideoRead

    U->>WD: 点击创建房间
    WD-->>CK: SIG_createRoom()
    CK->>CK: slot_createroom()
    CK->>NET: SendData(STRU_CREATEROOM_RQ)
    NET->>SVR: TCP发送
    SVR-->>NET: 创建成功(返回RoomID)
    NET-->>CK: SIG_ReadyData
    CK->>CK: slot_dealCreateRoomRS()
    CK->>CK: m_roomid = roomId
    CK->>WD: hide()
    CK->>RD: show()
    CK->>RD: slot_setInfo(roomid)
    CK->>AR: start() 开始音频采集
    CK->>VR: slot_openVideo() 开始视频采集
    Note over CK: 注册音频/视频TCP连接
    CK->>NET: m_pAVClient[0] 发送 STRU_AUDIO_REGISTER
    CK->>NET: m_pAVClient[1] 发送 STRU_VIDEO_REGISTER
```

### 5.3 音频采集到播放全链路

```mermaid
sequenceDiagram
    participant SDL_IN as SDL采集线程
    participant AR as AudioRead
    participant CK as ckernel
    participant RD as RoomDialog
    participant NET as 音频TCP连接
    participant SVR as 服务器
    participant CK2 as 远端ckernel
    participant AW as AudioWrite
    participant SDL_OUT as SDL播放线程

    SDL_IN->>AR: audioCallback(stream, 1920字节)
    AR->>AR: WebRTC VAD检测(返回0=静音/1=有声/-1=错误)
    AR->>AR: calculateRMS 更新 m_noiseLevel
    AR->>AR: 状态平滑(连续3帧≈60ms确认)
    alt 静音状态切换
        AR-->>CK: SIG_muteStateChanged(isMute)
        CK->>CK: slot_muteStateChanged()
        CK->>RD: updateMuteStatus(isMute)
        RD->>RD: cb_audio 显示 音频(无声)/音频
    end
    AR->>AR: 静音帧 memset 清零(配合Opus DTX)
    AR->>AR: opus_encode(960samples → 60-120字节)
    AR-->>CK: SIG_audioFrame(QByteArray)
    CK->>CK: slot_audioFrame()
    CK->>CK: 构造 STRU_AUDIO_FRAME 包
    CK->>CK: AVSyncManager::updateLocalAudioClock(pts)
    CK->>NET: m_pAVClient[audio_client] SendData()
    NET->>SVR: TCP音频连接发送
    SVR-->>CK2: 透明转发
    CK2->>CK2: slot_dealAudioFrameRq()
    CK2->>CK2: AVSyncManager::updateAudioClock(userId, pts)
    CK2-->>AW: slot_playAudio(opusData)
    AW->>AW: mutex加锁 入队 m_audioQueue
    SDL_OUT->>AW: audioCallback()
    AW->>AW: mutex加锁 出队
    AW->>AW: opus_decode(60-120字节 → 1920字节PCM)
    AW->>AW: SDL_MixAudioFormat() 混音播放

    Note over AR: 并行支路: QTimer每5秒触发 slot_adjustVadMode
    Note over AR: 按噪声EMA自适应切换VAD模式(1=宽松/2=中等/3=严格)
```

### 5.4 视频采集到渲染全链路

```mermaid
sequenceDiagram
    participant CAM as OpenCV摄像头
    participant VR as VideoRead
    participant CK as ckernel
    participant ENC as H264Encoder
    participant WORK as SendVideoWorker
    participant NET as 视频TCP连接
    participant SVR as 服务器
    participant CK2 as 远端ckernel
    participant DEC as H264Decoder
    participant SYNC as AVSyncManager
    participant RD as RoomDialog
    participant US as UserShow
    participant GL as YuvGLWidget

    CAM->>VR: cap.read(frame) BGR
    VR->>VR: cvtColor BGR→RGB
    VR->>VR: scaled(640×480, IgnoreAspectRatio)
    VR-->>CK: SIG_sendVideoFrame(QImage)
    CK->>CK: slot_sendVideoFrame()
    CK->>ENC: encode(QImage)
    ENC->>ENC: img.copy() 深拷贝防竞态
    ENC->>ENC: av_frame_make_writable
    ENC->>ENC: sws_scale RGB24→YUV420P
    ENC->>ENC: avcodec_send_frame/receive_packet
    ENC-->>CK: H264 NAL数据(QByteArray)
    CK->>CK: 构造 STRU_VIDEO_FRAME_H264_HEADER
    CK->>CK: 按4KB分片循环
    CK-->>WORK: SIG_SendVideo(buf, len)
    WORK->>WORK: slot_SendVideo() 子线程执行
    WORK->>NET: SendData 分片发送
    NET->>SVR: TCP视频连接发送
    SVR-->>CK2: 透明转发
    CK2->>CK2: slot_dealVideoFrameH264Rq()
    CK2->>CK2: 分片重组(m_h264Reassemble)
    CK2->>DEC: decode(h264Data)
    DEC->>DEC: avcodec_send_packet/receive_frame
    DEC->>DEC: av_frame_clone() 零拷贝
    DEC-->>CK2: AVFrame YUV420P
    CK2->>SYNC: synchronizeVideo(userId, pts, frameDelay)
    SYNC->>SYNC: diff = video_pts - audioClock
    alt diff > 10ms
        SYNC-->>CK2: waitMs (等待)
        CK2->>CK2: QThread::msleep(waitMs)
    else diff ≤ 10ms
        SYNC-->>CK2: 0 (立即显示)
    end
    CK2->>RD: slot_refreshUserYuv(userId, frame)
    RD->>US: slot_setFrame(AVFrame)
    US->>GL: slot_setFrame(AVFrame)
    GL->>GL: uploadYuvTextures
    GL->>GL: GLSL BT.601 YUV→RGB 渲染
```

### 5.5 PLI 关键帧请求流程

```mermaid
sequenceDiagram
    participant SVR as 服务器
    participant CK2 as 远端ckernel(发送方)
    participant CK as ckernel(新加入成员)
    participant RD as RoomDialog

    SVR-->>CK: DEF_PACK_ROOM_MEMBER 新成员通知
    CK->>CK: slot_dealRoomMemberRq()
    CK->>CK: 创建 UserShow 控件
    CK->>RD: slot_addUserShow(user)
    CK->>CK: AVSyncManager::addUser(userId)
    CK->>CK: new H264Decoder 加入 m_mapIDToH264Decoder
    Note over CK: 新成员需要关键帧才能解码
    CK->>CK: 构造 STRU_PLI_REQUEST
    CK->>SVR: 发送PLI请求
    SVR-->>CK2: 转发PLI请求
    CK2->>CK2: slot_dealPliRequest()
    CK2->>CK2: m_h264Encoder->requestKeyFrame()
    Note over CK2: 下一帧强制为I帧
    CK2->>SVR: 发送I帧
    SVR-->>CK: 转发I帧
    CK->>CK: 解码I帧成功
```

---

## 六、设计模式总结

| 设计模式 | 应用位置 | 具体实现 | UML 关系 | 说明 |
|---------|---------|---------|---------|------|
| **接口模式** | INet → TcpClient/TcpServer/UdpNet | 纯虚函数 InitNet/SendData/UnInitNet/RecvData | `INet <|-- TcpClient` | 网络层抽象，支持 TCP/UDP 无缝切换 |
| **中介者模式** | INetMediator → TcpClientMediator 等 | DealData() 桥接网络层与业务层 | `INetMediator o-- INet` | Qt 信号 SIG_ReadyData 解耦网络与业务 |
| **单例模式** | ckernel::GetInstance() | Meyers 单例（C++11 线程安全局部静态变量） | `ckernel ..> ckernel` | 全局唯一业务中枢 |
| **单例模式** | AVSyncManager::getInstance() | Meyers 单例（C++11 线程安全） | `AVSyncManager ..> AVSyncManager` | 全局唯一同步管理器 |
| **观察者模式** | Qt 信号槽机制 | connect(sender, SIGNAL, receiver, SLOT) | `-->> SIG_xxx` | 模块间松耦合通信 |
| **策略模式** | m_netPackMap[100] 协议映射表 | PFUN 函数指针数组，O(1) 消息分发 | `ckernel ..> PFUN` | m_netPackMap[type - 10000] = slot_dealXxx |
| **生产者-消费者** | AudioWrite::m_audioQueue | std::deque + std::mutex | `AudioWrite *-- m_audioQueue` | SDL 回调线程消费，Qt 主线程生产 |
| **模板方法** | ThreadWorker → SendVideoWorker | 基类管理 QThread 生命周期 | `ThreadWorker <|-- SendVideoWorker` | moveToThread + 信号驱动工作 |
| **零拷贝** | H264Decoder::decode() → AVFrame | av_frame_clone 引用计数 | `H264Decoder ..> AVFrame` | 跨线程传递不复制像素数据 |
| **组合模式** | AVSyncManager → SyncContext | 每用户独立同步上下文 | `AVSyncManager *-- SyncContext` | 部分随整体创建/销毁 |

---

## 七、协议类型常量表

| 常量名 | 值 | 方向 | 对应结构体 | 说明 |
|--------|-----|------|-----------|------|
| DEF_PACK_REGISTER_RQ | 10000 | C→S | STRU_REGISTER_RQ | 注册请求 |
| DEF_PACK_REGISTER_RS | 10001 | S→C | STRU_REGISTER_RS | 注册响应 |
| DEF_PACK_LOGIN_RQ | 10002 | C→S | STRU_LOGIN_RQ | 登录请求 |
| DEF_PACK_LOGIN_RS | 10003 | S→C | STRU_LOGIN_RS | 登录响应 |
| DEF_PACK_CREATEROOM_RQ | 10004 | C→S | STRU_CREATEROOM_RQ | 创建房间请求 |
| DEF_PACK_CREATEROOM_RS | 10005 | S→C | STRU_CREATEROOM_RS | 创建房间响应 |
| DEF_PACK_JOINROOM_RQ | 10006 | C→S | STRU_JOINROOM_RQ | 加入房间请求 |
| DEF_PACK_JOINROOM_RS | 10007 | S→C | STRU_JOINROOM_RS | 加入房间响应 |
| DEF_PACK_ROOM_MEMBER | 10008 | S→C | STRU_ROOM_MEMBER_RQ | 房间成员通知 |
| DEF_PACK_AUDIO_FRAME | 10009 | C→S→C | (内联结构) | 音频帧(转发) |
| DEF_PACK_VIDEO_FRAME | 10010 | C→S→C | (内联结构) | 视频帧-JPEG(转发) |
| DEF_PACK_LEAVEROOM_RQ | 10011 | C→S | STRU_LEAVEROOM_RQ | 离开房间请求 |
| DEF_PACK_LEAVEROOM_RS | 10012 | S→C | STRU_ROOM_MEMBER_RQ | 离开房间通知 |
| DEF_PACK_AUDIO_REGISTER | 10013 | C→S | STRU_AUDIO_REGISTER | 音频通道注册 |
| DEF_PACK_VIDEO_REGISTER | 10014 | C→S | STRU_VIDEO_REGISTER | 视频通道注册 |
| DEF_PACK_UPDATE_EMAIL_RQ | 10015 | C→S | STRU_UPDATE_EMAIL_RQ | 补全邮箱请求 |
| DEF_PACK_UPDATE_EMAIL_RS | 10016 | S→C | STRU_UPDATE_EMAIL_RS | 补全邮箱响应 |
| DEF_PACK_VIDEO_FRAME_H264 | 10017 | C→S→C | STRU_VIDEO_FRAME_H264_HEADER | 视频帧-H264(转发) |
| DEF_PACK_PLI_REQUEST | 10018 | C→S→C | STRU_PLI_REQUEST | PLI关键帧请求 |
| DEF_PACK_VIDEO_SPSPPS | 10019 | C→S→C | (可选) | H264 SPS/PPS参数集 |

---

## 八、类间关系完整说明

### 8.1 继承关系（泛化）

| 父类 | 子类 | 说明 |
|------|------|------|
| INet | TcpClient | TCP 客户端实现 |
| INet | TcpServer | TCP 服务端实现 |
| INet | UdpNet | UDP 网络实现 |
| INetMediator | TcpClientMediator | TCP 客户端中介者 |
| INetMediator | TcpServerMediator | TCP 服务端中介者 |
| INetMediator | UdpMediator | UDP 中介者 |
| ThreadWorker | SendVideoWorker | 视频发送工作线程 |
| ThreadWorker | worker | 示例工作线程 |
| QObject | ckernel, AudioRead, AudioWrite, VideoRead, ScreenRead, H264Encoder, H264Decoder, INetMediator, ThreadWorker | Qt 对象基类 |
| QDialog | LoginDialog, WeChatDialog, RoomDialog | Qt 对话框基类 |
| QWidget | UserShow | Qt 窗口部件基类 |
| QOpenGLWidget | YuvGLWidget, MyOpenGLWidget | Qt OpenGL 控件基类 |

### 8.2 聚合关系（Aggregation - 弱拥有）

| 聚合方 | 被聚合方 | 多重性 | 说明 |
|--------|---------|--------|------|
| ckernel | INetMediator (m_pClient) | 1 | 信令 TCP 连接 |
| ckernel | INetMediator (m_pAVClient) | 2 | 音频+视频 TCP 连接 |
| ckernel | LoginDialog | 1 | 登录对话框 |
| ckernel | WeChatDialog | 1 | 主界面对话框 |
| ckernel | RoomDialog | 1 | 房间对话框 |
| ckernel | AudioRead | 1 | 音频采集器 |
| ckernel | AudioWrite | 0..* | 音频播放器（每成员一个） |
| ckernel | VideoRead | 1 | 摄像头采集器 |
| ckernel | ScreenRead | 1 | 桌面共享采集器 |
| ckernel | H264Encoder | 1 | H264 编码器 |
| ckernel | H264Decoder | 0..* | H264 解码器（每成员一个） |
| ckernel | SendVideoWorker | 1 | 视频发送工作线程 |
| INetMediator | INet | 1 | 网络底层实现 |
| RoomDialog | UserShow | 1..* | 用户显示控件（每成员一个） |

### 8.3 组合关系（Composition - 强拥有）

| 组合方 | 被组合方 | 多重性 | 说明 |
|--------|---------|--------|------|
| UserShow | YuvGLWidget | 0..1 | YUV 渲染控件（延迟创建） |
| AVSyncManager | SyncContext | 0..* | 同步上下文（每用户一个） |
| ckernel | H264FragmentGroup | 0..* | 分片重组缓冲区（内部结构体） |

### 8.4 依赖关系（Dependency - 使用）

| 依赖方 | 被依赖方 | 说明 |
|--------|---------|------|
| ckernel | AVSyncManager | 单例访问同步管理器 |
| ckernel | MD5 | 密码加盐哈希 |
| TcpClientMediator | TcpClient | 创建 TcpClient 实例 |
| TcpServerMediator | TcpServer | 创建 TcpServer 实例 |
| UdpMediator | UdpNet | 创建 UdpNet 实例 |
| VideoRead | MyFaceDetect | 可选人脸检测 |
| ckernel | H264Encoder | 编码调度 |
| ckernel | H264Decoder | 解码管理 |

### 8.5 关联关系（Association - 结构关系）

| 关联方 | 被关联方 | 说明 |
|--------|---------|------|
| INet | INetMediator | m_pMediator 反向关联中介者 |

---

## 九、关键设计说明

### 9.1 三路 TCP 连接架构

```
ckernel
  ├── m_pClient (信令连接) ──→ TcpClientMediator ──→ TcpClient ──→ 服务器:8000
  │     用途: 登录/注册/房间管理/成员通知
  │     特点: 低频小包，要求即时响应
  │
  ├── m_pAVClient[0] (音频连接) ──→ TcpClientMediator ──→ TcpClient ──→ 服务器:8000
  │     用途: Opus 音频帧转发
  │     特点: 高频小包（50帧/秒），要求低延迟
  │
  └── m_pAVClient[1] (视频连接) ──→ TcpClientMediator ──→ TcpClient ──→ 服务器:8000
        用途: H264 视频帧分片转发
        特点: 中频大包（30帧/秒，4KB/分片），要求高吞吐
```

> **设计说明：** 三路 TCP 连接分离信令与媒体流，避免大视频帧阻塞信令通道。每路连接独立的 TcpClientMediator 实例，通过 SIG_ReadyData 信号统一汇入 ckernel::slot_dealData 处理。

### 9.2 双路径视频渲染架构

```
ckernel::slot_sendVideoFrame(QImage)
  │
  ├── H264 路径（m_useH264 = true）
  │     ├── H264Encoder::encode(QImage) → QByteArray
  │     ├── 按 4KB 分片 → STRU_VIDEO_FRAME_H264_HEADER + h264Data
  │     ├── SendVideoWorker 子线程发送
  │     └── 接收端: H264Decoder::decode → AVFrame → YuvGLWidget GPU 渲染
  │
  └── JPEG 路径（m_useH264 = false，兼容回退）
        ├── QImage::save("JPG") → QByteArray
        ├── STRU_VIDEO_FRAME + jpegData
        └── 接收端: slot_refreshUser(QImage) → QPainter CPU 渲染
```

> **设计说明：** H264 路径使用 GPU 加速渲染（YuvGLWidget + OpenGL Shader），JPEG 路径使用 CPU 软渲染（QPainter）。UserShow 控件通过 m_glWidget 的可见性自动切换渲染路径。

### 9.3 QImage 隐式共享线程安全

```mermaid
classDiagram
    direction LR

    class VideoThread {
        <<线程>>
        +MatToQImage 构造QImage
        +emit SIG_sendVideoFrame
    }

    class MainThread {
        <<线程>>
        +slot_setImage
        +paintEvent_scaled
    }

    class WorkerThread {
        <<线程>>
        +encodeAndSendVideoFrame
        +H264Encoder_encode
    }

    VideoThread ..> MainThread : SIG_sendVideoFrame
    VideoThread ..> WorkerThread : QTimer_singleShot

    note for MainThread "竞态风险点 QImage隐式共享COW\nm_img = img 浅拷贝 共享像素数据\npaintEvent读取时可能访问已释放内存\n修复方案 m_img = img.copy 深拷贝"
    note for WorkerThread "竞态风险点 Lambda捕获QImage\nthis img 浅拷贝捕获\n异步执行时可能访问已释放内存\n修复方案 QImage imgCopy = img.copy 深拷贝"
```

> **设计说明：** Qt 的 QImage 采用隐式共享（Copy-On-Write）机制，赋值操作是浅拷贝。在三线程架构（Video/Main/Worker）中，浅拷贝会导致竞态条件：paintEvent 读取像素数据时，原始 QImage 可能已被销毁。修复方案是在 slot_setImage 和 Lambda 捕获前使用 `img.copy()` 强制深拷贝。

### 9.4 FFmpeg 帧缓冲区管理

```
H264Encoder::encode(QImage) 流程：
  │
  ├── 1. QImage 深拷贝：img.copy() → 独立像素数据
  ├── 2. av_frame_make_writable(m_frame) → 确保 YUV 帧缓冲区可写
  ├── 3. sws_scale(m_swsCtx, RGB→YUV420P) → 色彩空间转换
  ├── 4. avcodec_send_frame(m_codecCtx, m_frame) → 发送帧到编码器
  └── 5. avcodec_receive_packet(m_codecCtx, m_pkt) → 接收编码后的包

H264Decoder::decode(QByteArray) 流程：
  │
  ├── 1. avcodec_send_packet(m_codecCtx, m_pkt) → 发送包到解码器
  ├── 2. avcodec_receive_frame(m_codecCtx, m_frame) → 接收解码后的帧
  └── 3. av_frame_clone(m_frame) → 引用计数 +1，零拷贝跨线程传递
```

> **设计说明：** 编码器使用 `av_frame_get_buffer` + `av_frame_make_writable` 标准组合管理帧缓冲区，支持引用计数。解码器使用 `av_frame_clone` 实现零拷贝跨线程传递，避免像素数据复制开销。

---

> **文档版本：** v2.1
> **最后更新：** 2026-08-01
> **更新说明：** 新增 WebRTC VAD 静音检测（动态阈值/状态平滑/UI联动）相关成员与方法
> **数据来源：** 源码头文件静态分析（ckernel.h, INet.h, TcpClient.h, INetMediator.h, audioread.h, audiowrite.h, videoread.h, screenread.h, h264encoder.h, h264decoder.h, yuvglwidget.h, myopenglwidget.h, AVSyncManager.h, threadworker.h, md5.h, roomdialog.h, usershow.h, logindialog.h, wechatdialog.h, packdef.h）
> **重构内容：** 基于源码头文件全面重构类图，完整标注每个类的属性（名称、类型、可见性）和方法（名称、参数、返回值、可见性），使用标准 UML 符号标识所有类间关系（继承、实现、关联、聚合、组合、依赖），添加设计模式注释和关键设计说明。
