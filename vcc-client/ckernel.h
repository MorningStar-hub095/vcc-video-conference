#ifndef CKERNEL_H
#define CKERNEL_H

#include <QObject>
#include <QMap>
#include <QByteArray>
#include "vccdialog.h"
#include "TcpClientMediator.h"
#include "packdef.h"
#include "logindialog.h"
#include "roomdialog.h"

#include "audioread.h"
#include "audiowrite.h"

#include "videoread.h"

#include "screenread.h"

// 分片重组主动扫描定时器所需（新增成员 m_reassembleScanTimer 的完整类型）
#include <QTimer>
#include <QThread> //【修复-P2：桌面共享移出 UI 线程】m_pScreenThread
#include <mutex> //【P2修复：SIG_SendVideo 队列泄漏兜底】保护在途缓冲登记表
#include <set>   //【P2修复】同上
#include <thread> //【P2修复：detached 线程竞态】成员线程对象

#include "threadworker.h"

// H.264编解码器头文件
#include "h264encoder.h"
#include "h264decoder.h"

// 音视频同步管理器头文件
// 功能：将音视频同步逻辑从 ckernel 中抽离为独立模块
// 策略：音频主时钟，每用户独立时钟上下文
#include "AVSyncManager.h"

//协议映射表使用的类型
class ckernel;
typedef void (ckernel::*PFUN)(uint sock,char* buf,int nlen);

class SendVideoWorker;
class ckernel : public QObject
{
    Q_OBJECT
public:
    explicit ckernel(QObject *parent = nullptr);

    //单例
    static ckernel* GetInstance()
    {
        static ckernel kernel;
        return & kernel;
    }//此版本有局限性，无法回收

signals:
    void SIG_SendVideo( char* buf , int nlen );


public slots:
    //设置协议映射关系
    void setNetPackMap();
    //初始化配置
    void initConfig();
    //回收
    void slot_destroy();
    //发送登录信息
    void slot_loginCommit(QString tel , QString pass);
    //发送注册信息
    //新增 email 参数，用于注册时传递邮箱
    void slot_registerCommit(QString tel, QString pass, QString name, QString email);
    //创建房间
    void slot_createroom();
    //加入房间
    void slot_joinroom();
    //退出房间
    void slot_quitRoom();
    //音频信息处理
    void slot_pauseAudio();
    void slot_startAudio();
    //视频信息处理
    void slot_pauseVideo();
    void slot_startVideo();
    //桌面信息处理
    void slot_startScreen();
    void slot_pauseScreen();
    //刷新图片显示
    void slot_refreshVideo(int id , QImage & img);
    //发送音频帧
    void slot_audioFrame(QByteArray ba);
    // 静音状态变化处理槽（接收 AudioRead::SIG_muteStateChanged 信号）
    // 功能：记录日志并联动 RoomDialog 更新麦克风 UI 反馈
    void slot_muteStateChanged(bool isMute);
    //发送视频帧
    void slot_sendVideoFrame(QImage img);
    // 编码+发送的实际执行函数
    // 修改原因: H264编码在UI线程耗时30-100ms，阻塞paintEvent导致视频画面不显示
    // 修改内容: 将编码+发送逻辑从slot_sendVideoFrame提取到独立函数
    //           通过QTimer::singleShot(0)延迟调用，让paintEvent优先执行
    void encodeAndSendVideoFrame(QImage img);
    //多线程发送视频
    void slot_SendVideo( char* buf , int nlen );
    //-------------网络信息处理-----------------------//
    void slot_dealData(uint sock,char* buf,int nlen);
    //登录回复
    void slot_dealLoginRS(uint sock,char* buf,int nlen);
    //注册回复
    void slot_dealRegisterRS(uint sock,char* buf,int nlen);
    //创建房间回复
    void slot_dealCreateRoomRS(uint sock,char* buf,int nlen);
    //加入房间回复处理
    void slot_dealJoinRoomRS(uint sock,char* buf,int nlen);
    //房间成员请求处理
    void slot_dealRoomMemberRq(uint sock,char* buf,int nlen);
    //离开房间处理
    void slot_dealLeaveRoomRq(uint sock,char* buf,int nlen);
    //音频帧处理
    void slot_dealAudioFrameRq(uint sock,char* buf,int nlen);
    //视频帧处理
    void slot_dealVideoFrameRq(uint sock,char* buf,int nlen);
    //补全邮箱回复处理
    void slot_dealUpdateEmailRS(uint sock,char* buf,int nlen);
    //【P2修复：断线重连不重发音视频注册包】AV连接重连成功后补发注册包
    // 功能：TcpClientMediator 内部重连成功会发出 SIG_reconnected，
    //       服务端只认注册包绑定的 fd，不补发则重连后的音视频帧全部被丢弃。
    void slot_avReconnected();

    // H.264视频帧处理槽函数
    // 处理H.264视频帧包（与JPEG路径共存）
    void slot_dealVideoFrameH264Rq(uint sock, char* buf, int nlen);
    // PLI请求处理槽函数
    // 收到关键帧请求时强制下一帧为I帧
    void slot_dealPliRequest(uint sock, char* buf, int nlen);
    // H.264帧解码完成回调
    // 由解码线程触发，传递解码后的AVFrame到UI线程
    void slot_h264FrameDecoded(int userId, AVFrame *frame);

    // 分片重组缓冲区主动扫描槽（50ms周期）
    // 修改原因: 原超时检测纯被动（仅收到新分片时执行），视频流完全中断
    //           （对端崩溃/断网）时新分片永不到达：残留分片组驻留内存、
    //           超时PLI永不触发。
    // 修改内容: 新增50ms周期主动扫描，独立于分片到达事件清理过期组并触发限流PLI
    // 影响范围: 流中断场景下过期组必被清理；正常场景与被动清理互为冗余（幂等）
    void slot_scanReassembleBuffer();


private:
    // PLI关键帧请求限流发送
    // 修改原因: 原超时分支无条件发送PLI，弱网下形成"I帧超时→更多PLI→更多I帧"
    //           的正反馈风暴（详见packdef.h中PLI_THROTTLE_INTERVAL_MS注释）
    // 修改内容: 新增限流函数，每用户PLI_THROTTLE_INTERVAL_MS(500ms)窗口内
    //           最多发送一次PLI，窗口内重复请求直接丢弃
    // 影响范围: 超时分支/被动清理循环/主动扫描定时器统一调用此函数发送PLI
    void sendPliRequestThrottled(int userId);

    VccDialog *  m_pVccDlg;
    LoginDialog *   m_pLoginDlg;
    INetMediator *  m_pClient;
    QString         m_serverIP;
    RoomDialog *    m_pRoomdialog;

    //协议映射表
    PFUN            m_netPackMap[ DEF_PACK_COUNT ];


    int m_id;
    int m_roomid;
    //【安全修复-P0：音视频通道劫持】登录成功时保存服务端下发的会话令牌，
    // 音/视频通道注册包必须携带（与 packdef.h 的 m_token 字段配套）。
    int m_token;
    //添加 m_UserName 成员保存当前登录用户名
    //原代码未保存用户名，导致创建房间时无法正确设置自己的 UserShow 信息
    QString m_UserName;

    ///音频 1个采集 多个播放 每一个房间成员 1:1 map映射
    AudioRead * m_pAudioRead;
    std::map<int , AudioWrite*>m_mapIDToAudioWrite;

    ///音频 1个采集 多个播放 每一个房间成员 1:1 map映射
    VideoRead * m_pVideoRead;

    ScreenRead * m_pScreenRead;
    //【修复-P2：桌面共享移出 UI 线程】ScreenRead 所在工作线程（moveToThread）
    QThread * m_pScreenThread;

    enum client_type{ audio_client = 0 , video_client = 1 };
    INetMediator * m_pAVClient[2];

    QSharedPointer<SendVideoWorker> m_pSendVideoWorker;

    // H.264编码器成员
    // 全局唯一编码器实例（替代JPEG软编）
    H264Encoder * m_h264Encoder;
    // H.264解码器池
    // 每个房间成员独立一个解码器（与m_mapIDToAudioWrite同款映射）
    std::map<int, H264Decoder *> m_mapIDToH264Decoder;
    // H.264模式开关
    // true=使用H.264编解码, false=回退到JPEG（兼容旧客户端）
    bool m_useH264;
    // H.264帧序号计数器
    // 用于分片打包时的frameSeq字段
    int m_h264FrameSeq;

    // 编码防堆积标志
    // 原Bug: slot_sendVideoFrame 通过 QTimer::singleShot(0) 将 H264 编码延迟到事件队列，
    //        但 H264 编码耗时 30-100ms，而视频定时器每 23ms 触发一次。
    //        当编码耗时 > 23ms 时，encodeAndSendVideoFrame 调用在事件队列中堆积，
    //        paintEvent 被饿死，导致视频画面无法刷新。
    // 修复: 用标志位防止编码堆积。若上一帧编码未完成，跳过当前帧的编码，
    //        只刷新本地预览（paintEvent），确保画面持续显示。
    bool m_encodingPending;

    // H.264分片重组缓冲区
    // 修改原因: 发送端将H264帧按H264_FRAGMENT_SIZE(4096字节)分片发送，
    //           接收端必须收集同一frameSeq的所有分片后拼接为完整帧再送入解码器，
    //           否则每个分片都不是完整的NAL单元，解码器无法正确解码。
    // 修改内容: 新增分片重组结构体和映射表
    // 数据结构: key=frameSeq, value=分片数组(按fragmentIndex排序)
    //           同时记录总分片数、已接收分片数、时间戳(用于超时清理)
    // 超时机制: 超过H264_REASSEMBLE_TIMEOUT(100ms)未收齐的分片组将被丢弃
    struct H264FragmentGroup {
        int totalFragments = 0;          // 总分片数
        int receivedCount = 0;           // 已接收分片数
        QMap<int, QByteArray> fragments; // fragmentIndex -> 分片数据
        qint64 timestamp = 0;            // 首个分片到达时间(毫秒,用于超时清理)
        int frameType = 1;               // 帧类型(0=I帧, 1=P帧, 从包头复制)
    };
    // 按用户ID分组，每用户维护独立的重组缓冲区
    // key=userId, value=该用户正在重组的帧组(frameSeq -> H264FragmentGroup)
    std::map<int, std::map<int, H264FragmentGroup>> m_h264Reassemble;

    // PLI关键帧请求限流表
    // 修改原因: 配合sendPliRequestThrottled实现每用户500ms限流窗口
    // 修改内容: 新增每用户PLI发送时间戳表，key=userId, value=上次发送PLI的毫秒时间戳
    // 影响范围: 退出房间时随m_h264Reassemble一并清空（slot_quitRoom）
    std::map<int, qint64> m_lastPliTime;

    // 分片重组主动扫描定时器
    // 修改原因: 原超时检测纯被动（仅收到新分片时执行），流中断场景下残留分片组永不清理
    // 修改内容: 新增50ms周期QTimer，构造时启动，周期调用slot_scanReassembleBuffer
    // 影响范围: 空房间时扫描空map开销可忽略（50ms一次遍历空容器）
    QTimer *m_reassembleScanTimer;

    // 注释化原有音视频同步成员，改用独立的 AVSyncManager 模块
    // 原代码：直接在 ckernel 中维护 m_audioClock/m_videoClock 和 synchronizeVideo 函数
    // 修改原因：将同步逻辑抽离为独立模块，支持多用户独立时钟，提高可维护性
    // 替代方案：使用 AVSyncManager::getInstance() 统一管理所有用户的音视频同步
    /* 原有代码（已废弃，保留作为历史参考）：
    // 音视频同步相关成员
    // 策略：音频主时钟（参考MediaPlayer的synchronize_video+audio_clock方案）
    // 原理：音频时钟作为主基准，视频帧显示前与audio_clock比对，超前则等待
    double m_audioClock;   // 音频主时钟（微秒），每收到音频帧时更新
    double m_videoClock;   // 视频辅助时钟（微秒），每显示视频帧时更新

    // 同步等待函数
    // 参考MediaPlayer/videoplayer.cpp的synchronize_video实现
    // 功能：计算视频帧PTS，与audio_clock比对，超前则QThread::msleep等待
    double synchronizeVideo(double pts, double frameDelay);
    */

    //【P2修复：SIG_SendVideo 裸 new[] 队列泄漏兜底】
    // 修改原因: 视频分片缓冲区 new[] 后经队列信号投递给 worker 线程释放；
    //           若 worker 停止（slot_destroy 中 clear()）时事件队列尚有未消费的
    //           投递项，其携带的 char* 随队列一起被丢弃，内存泄漏且无从追回。
    // 修改效果: 发射前登记、worker 消费后注销、worker 停止后统一释放残留，
    //           保证每一块 new[] 出去的缓冲区最终都被 delete[]。
    std::mutex m_videoBufMutex;          //保护下表（主线程登记 / worker 线程注销）
    std::set<char*> m_inflightVideoBufs; //已发射、尚未被 worker 消费的视频包缓冲区

    //【P2修复：detached 线程竞态】网络初始化线程改为可 join 的成员对象
    // 原代码: initConfig 中 std::thread(...).detach()，与主线程生命周期脱钩
    std::thread m_netInitThread;


};

class SendVideoWorker : public ThreadWorker
{
    Q_OBJECT
public slots:
    void slot_SendVideo( char* buf , int nlen ){
        ckernel::GetInstance()->slot_SendVideo( buf , nlen );
    }


};

#endif // CKERNEL_H
