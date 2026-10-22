// 音视频同步管理器实现文件
// 功能：实现 AVSyncManager 类的所有方法
// 策略：音频主时钟（audio_clock 为基准，视频同步到音频）
// 参考：MediaPlayer/videoplayer.cpp 的 synchronize_video + audio_clock 方案

#include "AVSyncManager.h"
#include <QDateTime>

// 同步参数常量定义
// 同步阈值 10ms（微秒），参考 ffplay 的 SYNC_THRESHOLD
// 原理：音视频时间差在阈值内视为同步正常，避免微抖动频繁触发等待
const double AVSyncManager::SYNC_THRESHOLD_US = 10000.0;

// 最大等待时间 100ms（毫秒）
// 原理：防止视频帧等待过久导致UI卡死，超过此值直接显示
const int AVSyncManager::MAX_WAIT_MS = 100;

// 时钟超时时间 5000ms（毫秒）
// 原理：超过此时间未收到音频帧，认为时钟失效，视频不再等待
const int AVSyncManager::CLOCK_TIMEOUT_MS = 5000;

// 私有构造函数（单例模式）
// 初始化本地音频/视频时钟
AVSyncManager::AVSyncManager(): m_localAudioClock(0.0) , m_localVideoClock(0.0)
{
    qDebug() << "[AVSync] AVSyncManager 已初始化";
}

// 获取单例实例
// 线程安全的 Meyers' Singleton（C++11 保证局部静态变量初始化线程安全）
AVSyncManager& AVSyncManager::getInstance()
{
    static AVSyncManager instance;
    return instance;
}

// 添加用户同步上下文
// 调用时机：收到新成员加入房间时
// 线程安全：通过 m_mutex 保护
void AVSyncManager::addUser(int userId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // 如果用户已存在，不重复添加
    if (m_syncContexts.find(userId) != m_syncContexts.end())
    {
        qDebug() << "[AVSync] 用户" << userId << "已存在，跳过添加";
        return;
    }
    // 创建新的同步上下文（构造函数自动初始化为0）
    m_syncContexts[userId] = SyncContext();
    qDebug() << "[AVSync] 添加用户同步上下文, userId:" << userId
             << ", 当前用户数:" << m_syncContexts.size();
}

// 移除用户同步上下文
// 调用时机：成员离开房间时
// 线程安全：通过 m_mutex 保护
void AVSyncManager::removeUser(int userId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_syncContexts.find(userId);
    if (it != m_syncContexts.end())
    {
        m_syncContexts.erase(it);
        qDebug() << "[AVSync] 移除用户同步上下文, userId:" << userId
                 << ", 剩余用户数:" << m_syncContexts.size();
    }
    else
    {
        qDebug() << "[AVSync] 移除用户时未找到, userId:" << userId;
    }
}

// 更新音频主时钟
// 调用时机：收到远端音频帧时（slot_dealAudioFrameRq）
// 参数：userId - 用户ID, pts - 音频帧PTS（微秒）
// 原理：音频以恒定速率播放（8kHz/20ms），audio_clock 作为主基准驱动视频同步
// 参考：MediaPlayer/videoplayer.cpp:688-696 的 audio_clock 更新逻辑
void AVSyncManager::updateAudioClock(int userId, double pts)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_syncContexts.find(userId);
    if (it == m_syncContexts.end())
    {
        // 用户不存在，自动创建上下文
        // 原因：音频帧可能在成员通知之前到达（网络乱序）
        m_syncContexts[userId] = SyncContext();
        qDebug() << "[AVSync] updateAudioClock: 自动创建用户上下文, userId:" << userId;
        it = m_syncContexts.find(userId);
    }

    SyncContext& ctx = it->second;
    if (ctx.isPaused)
    {
        // 用户已暂停，不更新时钟
        return;
    }

    // 更新音频时钟
    ctx.audioClock = pts;
    // 记录更新时间（用于超时检测）
    ctx.lastAudioUpdateTime = QDateTime::currentMSecsSinceEpoch();

    // 调试日志（低频输出，避免刷屏）
    //【P2修复：日志风暴】本函数每收到一帧音频（50帧/s/用户）都会执行，
    // 原无条件 qDebug 在多人会议下产生海量日志，拖慢 UI 线程并刷爆日志文件。
    // 修改效果: 删除逐帧日志；自动创建上下文的日志（低频）保留。
    // 原代码保留备查：
    // qDebug() << "[AVSync] 更新音频时钟, userId:" << userId
    //          << ", audioClock(us):" << ctx.audioClock;
}

// 视频帧同步等待计算（接收端使用）
// 调用时机：收到远端视频帧准备显示前
// 参数：userId - 用户ID, video_pts - 视频帧PTS（微秒）, frameDelay - 帧间延迟（微秒）
// 返回值：需要等待的毫秒数（0 = 立即显示，>0 = 需等待，-1 = 错误）
// 算法参考：MediaPlayer/videoplayer.cpp:98-105 的同步等待循环
int AVSyncManager::synchronizeVideo(int userId, double video_pts, double frameDelay)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_syncContexts.find(userId);
    if (it == m_syncContexts.end())
    {
        // 用户不存在，返回-1表示错误
        qDebug() << "[AVSync] synchronizeVideo: 用户不存在, userId:" << userId;
        return -1;
    }

    SyncContext& ctx = it->second;
    if (ctx.isPaused)
    {
        // 用户已暂停，直接显示（不等待）
        return 0;
    }

    return computeSyncDelay(ctx, video_pts, frameDelay);
}

// 发送端视频帧同步等待计算
// 调用时机：本地视频帧发送前（slot_sendVideoFrame）
// 功能：与本地音频采集时钟比对，确保本地预览音视频同步
// 参数：video_pts - 视频帧PTS（微秒）, frameDelay - 帧间延迟（微秒）
// 返回值：需要等待的毫秒数
int AVSyncManager::synchronizeLocalVideo(double video_pts, double frameDelay)
{
    std::lock_guard<std::mutex> lock(m_localMutex);

    // 创建临时同步上下文用于计算
    SyncContext ctx;
    ctx.audioClock = m_localAudioClock;
    ctx.videoClock = m_localVideoClock;
    ctx.isPaused = false;

    int waitMs = computeSyncDelay(ctx, video_pts, frameDelay);

    // 更新本地视频时钟（computeSyncDelay 已更新 ctx.videoClock）
    m_localVideoClock = ctx.videoClock;

    return waitMs;
}

// 同步等待的核心计算逻辑
// 被 synchronizeVideo 和 synchronizeLocalVideo 共同调用
// 参数：ctx - 同步上下文引用, video_pts - 视频PTS, frameDelay - 帧间延迟
// 返回值：需要等待的毫秒数（0 = 立即显示）
// 算法：
//   1. 若 video_pts > 0，用 PTS 更新 videoClock
//   2. 计算当前帧的显示 PTS = videoClock
//   3. 推进 videoClock += frameDelay（为下一帧准备）
//   4. 比对 diff = video_pts - audioClock
//   5. diff > SYNC_THRESHOLD → 视频超前，返回等待毫秒数
//   6. diff 在阈值内 → 同步正常，返回 0
//   7. diff < -SYNC_THRESHOLD → 视频落后，返回 0（立即显示）
// 参考：MediaPlayer/videoplayer.cpp 的 synchronize_video 函数
int AVSyncManager::computeSyncDelay(SyncContext& ctx, double video_pts, double frameDelay)
{
    // 步骤1: 更新视频时钟
    if (video_pts > 0.0)
    {
        // PTS存在，用PTS重置video_clock
        // 参考 MediaPlayer/videoplayer.cpp:37 is->video_clock = pts;
        ctx.videoClock = video_pts;
    }
    // 若PTS为0或负数，保持video_clock不变（线性外推）

    // 步骤2: 获取当前帧的显示PTS
    double currentVideoPts = ctx.videoClock;

    // 步骤3: 推进视频时钟（为下一帧做准备）
    // 参考 MediaPlayer/videoplayer.cpp:50 is->video_clock += frame_delay;
    ctx.videoClock += frameDelay;

    // 步骤4: 检查音频时钟有效性
    // 若音频时钟为0（未收到音频帧）或超时，不等待
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (ctx.audioClock <= 0.0)
    {
        // 音频时钟未初始化，直接显示视频
        qDebug() << "[AVSync] 音频时钟未初始化，视频直接显示";
        return 0;
    }
    if (ctx.lastAudioUpdateTime > 0 &&
        (now - ctx.lastAudioUpdateTime) > CLOCK_TIMEOUT_MS)
    {
        // 音频时钟超时（超过5秒未更新），认为音频流已断
        qDebug() << "[AVSync] 音频时钟超时, 距上次更新:" << (now - ctx.lastAudioUpdateTime) << "ms";
        return 0;
    }

    // 步骤5: 比对 video_pts 与 audio_clock
    // diff > 0 表示视频超前于音频，diff < 0 表示视频落后于音频
    double diff = currentVideoPts - ctx.audioClock;

    // 调试日志
    //【P2修复：日志风暴】本函数每视频帧执行（30帧/s/用户），逐帧日志在多人会议下
    // 刷屏且拖慢 UI 线程。修改效果: 删除逐帧比对日志；异常分支（超时/未初始化）日志保留。
    // 原代码保留备查：
    // qDebug() << "[AVSync] 同步比对 video_pts:" << currentVideoPts
    //          << ", audioClock:" << ctx.audioClock
    //          << ", diff(us):" << diff;

    // 步骤6: 判断是否需要等待
    if (diff > SYNC_THRESHOLD_US)
    {
        // 视频超前于音频，需要等待音频追上
        // 将微秒差值转换为毫秒
        int waitMs = (int)(diff / 1000.0);
        // 限制最大等待时间，避免UI卡死
        if (waitMs > MAX_WAIT_MS)
        {
            waitMs = MAX_WAIT_MS;
        }
        //【P2修复：日志风暴】仅在等待量异常大（>200ms，说明音视频明显失衡）时打日志，
        // 正常范围等待不再逐帧输出。原代码保留备查：
        // qDebug() << "[AVSync] 视频超前, 等待:" << waitMs << "ms";
        if( waitMs > 200 )
            qDebug() << "[AVSync] 视频超前, 等待:" << waitMs << "ms";
        return waitMs;
    }

    // diff 在  范围内 → 同步正常
    // diff < -SYNC_THRESHOLD → 视频落后，立即显示（追赶策略）
    return 0;
}

// 获取音频时钟
// 参数：userId - 用户ID
// 返回值：音频时钟值（微秒），用户不存在返回 0.0
double AVSyncManager::getAudioClock(int userId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_syncContexts.find(userId);
    if (it != m_syncContexts.end())
    {
        return it->second.audioClock;
    }
    return 0.0;
}

// 获取视频时钟
// 参数：userId - 用户ID
// 返回值：视频时钟值（微秒），用户不存在返回 0.0
double AVSyncManager::getVideoClock(int userId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_syncContexts.find(userId);
    if (it != m_syncContexts.end())
    {
        return it->second.videoClock;
    }
    return 0.0;
}

// 重置指定用户的时钟
// 调用时机：用户暂停/恢复时，或需要重新同步时
void AVSyncManager::resetUserClock(int userId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_syncContexts.find(userId);
    if (it != m_syncContexts.end())
    {
        it->second.audioClock = 0.0;
        it->second.videoClock = 0.0;
        it->second.lastAudioUpdateTime = 0;
        it->second.lastVideoUpdateTime = 0;
        qDebug() << "[AVSync] 重置用户时钟, userId:" << userId;
    }
}

// 重置所有用户的时钟
// 调用时机：退出房间时
void AVSyncManager::resetAllClocks()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& pair : m_syncContexts)
    {
        pair.second.audioClock = 0.0;
        pair.second.videoClock = 0.0;
        pair.second.lastAudioUpdateTime = 0;
        pair.second.lastVideoUpdateTime = 0;
        pair.second.isPaused = false;
    }
    qDebug() << "[AVSync] 重置所有用户时钟, 用户数:" << m_syncContexts.size();

    // 同时重置本地时钟
    {
        std::lock_guard<std::mutex> localLock(m_localMutex);
        m_localAudioClock = 0.0;
        m_localVideoClock = 0.0;
    }
}

// 暂停用户同步
// 调用时机：暂停音频/视频时
void AVSyncManager::pauseUser(int userId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_syncContexts.find(userId);
    if (it != m_syncContexts.end())
    {
        it->second.isPaused = true;
        qDebug() << "[AVSync] 暂停用户同步, userId:" << userId;
    }
}

// 恢复用户同步
// 调用时机：恢复音频/视频时
void AVSyncManager::resumeUser(int userId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_syncContexts.find(userId);
    if (it != m_syncContexts.end())
    {
        it->second.isPaused = false;
        // 恢复时重置时钟，避免旧时钟值导致视频长时间等待
        it->second.audioClock = 0.0;
        it->second.videoClock = 0.0;
        qDebug() << "[AVSync] 恢复用户同步, userId:" << userId;
    }
}

// 更新本地音频主时钟（发送端使用）
// 调用时机：本地音频采集帧发出时（slot_audioFrame）
// 参数：pts - 本地音频PTS（微秒）
void AVSyncManager::updateLocalAudioClock(double pts)
{
    std::lock_guard<std::mutex> lock(m_localMutex);
    m_localAudioClock = pts;
}

// 获取本地音频时钟
// 返回值：本地音频时钟值（微秒）
double AVSyncManager::getLocalAudioClock()
{
    std::lock_guard<std::mutex> lock(m_localMutex);
    return m_localAudioClock;
}

// 获取当前系统时间（微秒）
// 功能：统一时间戳获取方式，便于测试和替换
// 返回值：当前系统时间（微秒）
qint64 AVSyncManager::getCurrentTimeUs()
{
    // 使用 QTime 计算当天的微秒数
    // 与现有 ckernel 中的时间戳计算方式保持一致
    QTime now = QTime::currentTime();
    return (qint64)(
        now.hour() * 3600000000LL +
        now.minute() * 60000000LL +
        now.second() * 1000000LL +
        now.msec() * 1000LL
    );
}
