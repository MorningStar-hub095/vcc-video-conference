#ifndef AVSYNCMANAGER_H
#define AVSYNCMANAGER_H

// 音视频同步管理器头文件
// 功能：将音视频同步逻辑从 ckernel 中抽离为独立模块
// 策略：音频主时钟（audio_clock 为基准，视频同步到音频）
// 特性：
//   1. 每用户独立时钟上下文（支持多人群聊场景）
//   2. 线程安全（std::mutex 保护共享数据）
//   3. 可配置同步参数（阈值、最大等待时间等）
//   4. 单例模式，便于全局统一访问
// 参考：MediaPlayer/videoplayer.cpp 的 synchronize_video + audio_clock 方案

#include <QObject>
#include <map>
#include <mutex>
#include <QTime>
#include <QThread>
#include <QDebug>

// 同步上下文结构体
// 每个远端用户独立维护一套 audio_clock / video_clock
// 参考 MediaPlayer/videoplayer.h 中 VideoState 的 audio_clock/video_clock 成员
struct SyncContext
{
    double audioClock;             // 音频主时钟（微秒），每收到音频帧时更新
    double videoClock;             // 视频辅助时钟（微秒），每显示视频帧时更新
    qint64 lastAudioUpdateTime;    // 上次音频时钟更新的系统时间戳（毫秒，用于超时检测）
    qint64 lastVideoUpdateTime;    // 上次视频时钟更新的系统时间戳（毫秒）
    bool isPaused;                 // 该用户是否暂停同步

    //构造函数，初始化所有成员
    SyncContext()
        : audioClock(0.0)
        , videoClock(0.0)
        , lastAudioUpdateTime(0)
        , lastVideoUpdateTime(0)
        , isPaused(false)
    {
    }
};

// 音视频同步管理器类
// 单例模式，负责管理所有用户的音视频同步状态
// 核心职责：
//   1. 维护每用户的 audio_clock / video_clock
//   2. 提供视频帧同步等待计算（synchronizeVideo）
//   3. 提供时钟查询和重置接口
class AVSyncManager
{
public:
    // 获取单例实例
    // 线程安全的 Meyers' Singleton（C++11 保证局部静态变量初始化线程安全）
    static AVSyncManager& getInstance();

    // 添加用户同步上下文
    // 调用时机：收到新成员加入房间时
    // 参数：userId - 用户ID
    void addUser(int userId);

    // 移除用户同步上下文
    // 调用时机：成员离开房间时
    // 参数：userId - 用户ID
    void removeUser(int userId);

    // 更新音频主时钟
    // 调用时机：收到远端音频帧时（slot_dealAudioFrameRq）
    // 参数：userId - 用户ID, pts - 音频帧PTS（微秒）
    // 原理：音频以恒定速率播放，audio_clock 作为主基准驱动视频同步
    // 参考：MediaPlayer/videoplayer.cpp:688-696 的 audio_clock 更新逻辑
    void updateAudioClock(int userId, double pts);

    // 视频帧同步等待计算
    // 调用时机：收到远端视频帧准备显示前（slot_dealVideoFrameRq / slot_dealVideoFrameH264Rq）
    // 参数：userId - 用户ID, video_pts - 视频帧PTS（微秒）, frameDelay - 帧间延迟（微秒）
    // 返回值：需要等待的毫秒数（0 = 立即显示，>0 = 需等待，-1 = 错误/用户不存在）
    // 算法：
    //   1. 用 video_pts 更新 video_clock（若 PTS > 0）
    //   2. 推进 video_clock += frameDelay（为下一帧准备）
    //   3. 比对 diff = video_pts - audio_clock
    //   4. diff > SYNC_THRESHOLD → 视频超前，返回等待毫秒数
    //   5. diff 在阈值内 → 同步正常，返回 0
    //   6. diff < -SYNC_THRESHOLD → 视频落后，返回 0（立即显示）
    // 参考：MediaPlayer/videoplayer.cpp:98-105 的同步等待循环
    int synchronizeVideo(int userId, double video_pts, double frameDelay);

    // 发送端视频帧同步等待计算
    // 调用时机：本地视频帧发送前（slot_sendVideoFrame）
    // 功能：与本地音频采集时钟比对，确保本地预览音视频同步
    // 参数：video_pts - 视频帧PTS（微秒）, frameDelay - 帧间延迟（微秒）
    // 返回值：需要等待的毫秒数
    int synchronizeLocalVideo(double video_pts, double frameDelay);

    // 获取音频时钟
    // 参数：userId - 用户ID
    // 返回值：音频时钟值（微秒），用户不存在返回 0.0
    double getAudioClock(int userId);

    // 获取视频时钟
    // 参数：userId - 用户ID
    // 返回值：视频时钟值（微秒），用户不存在返回 0.0
    double getVideoClock(int userId);

    // 重置指定用户的时钟
    // 调用时机：用户暂停/恢复时，或需要重新同步时
    // 参数：userId - 用户ID
    void resetUserClock(int userId);

    // 重置所有用户的时钟
    // 调用时机：退出房间时
    void resetAllClocks();

    // 暂停用户同步
    // 调用时机：暂停音频/视频时
    // 参数：userId - 用户ID
    void pauseUser(int userId);

    // 恢复用户同步
    // 调用时机：恢复音频/视频时
    // 参数：userId - 用户ID
    void resumeUser(int userId);

    // 更新本地音频主时钟（发送端使用）
    // 调用时机：本地音频采集帧发出时（slot_audioFrame）
    // 参数：pts - 本地音频PTS（微秒）
    // 功能：维护本地音频时钟，供发送端视频同步使用
    void updateLocalAudioClock(double pts);

    // 获取本地音频时钟
    // 返回值：本地音频时钟值（微秒）
    double getLocalAudioClock();

    // 获取当前系统时间（微秒）
    // 功能：统一时间戳获取方式，便于测试和替换
    // 返回值：当前系统时间（微秒）
    static qint64 getCurrentTimeUs();

private:
    // 私有构造函数（单例模式）
    AVSyncManager();

    // 禁止拷贝和赋值（单例模式）
    AVSyncManager(const AVSyncManager&) = delete;
    AVSyncManager& operator=(const AVSyncManager&) = delete;

    // 获取或创建用户的同步上下文
    // 注意：调用者需已持有 m_mutex 锁
    // 参数：userId - 用户ID
    // 返回值：SyncContext 引用
    SyncContext& getOrCreateContext(int userId);

    // 同步等待的核心计算逻辑
    // 被 synchronizeVideo 和 synchronizeLocalVideo 共同调用
    // 参数：ctx - 同步上下文, video_pts - 视频PTS, frameDelay - 帧间延迟
    // 返回值：需要等待的毫秒数
    int computeSyncDelay(SyncContext& ctx, double video_pts, double frameDelay);

    // 用户同步上下文映射表
    // key: userId, value: SyncContext
    std::map<int, SyncContext> m_syncContexts;

    // 互斥锁，保护 m_syncContexts 的并发访问
    // 音频回调在SDL线程，视频在UI线程，需要线程安全
    std::mutex m_mutex;

    // 本地音频主时钟（发送端使用）
    // 不关联到具体用户ID，是本地采集的音频时钟
    double m_localAudioClock;

    // 本地视频辅助时钟（发送端使用）
    double m_localVideoClock;

    // 本地音频时钟互斥锁
    std::mutex m_localMutex;

    // 同步参数常量
    // 同步阈值：音视频时间差超过此值才触发等待（微秒）
    // 参考 ffplay 的 SYNC_THRESHOLD，避免微抖动频繁触发等待
    static const double SYNC_THRESHOLD_US;   // 10000微秒 = 10ms

    // 最大等待时间（毫秒），防止视频卡死
    static const int MAX_WAIT_MS;            // 100ms

    // 时钟超时时间（毫秒），超过此时间未更新则认为时钟失效
    static const int CLOCK_TIMEOUT_MS;       // 5000ms = 5s
};

#endif // AVSYNCMANAGER_H
