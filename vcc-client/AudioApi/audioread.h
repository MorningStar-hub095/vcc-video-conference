#ifndef AUDIOREAD_H
#define AUDIOREAD_H

#include <QObject>
#include <QTimer>   // VAD 动态阈值调整定时器需要 QTimer
#include "World.h"

// AudioRead 类重构
// 修改原因：音频采集从 QAudioInput（Qt定时器轮询）迁移到 SDL 采集（独立线程回调）
//          编码器从 Speex（8kHz窄带）迁移到 Opus（48kHz宽带，VOIP模式）
// 修改内容：
//   1. 移除 QAudioInput/QIODevice/QTimer 成员
//   2. 新增 SDL_AudioDeviceID 采集设备
//   3. 新增 OpusEncoder 编码器
//   4. 新增 SDL 采集回调函数（静态，SDL线程调用）
// 预期效果：采集延迟更稳定（SDL线程驱动），音质提升（48kHz Opus）
// 参考：AudioDemo2/OpusApi/sdlaudioread.h


class AudioRead : public QObject
{
    Q_OBJECT
public:
    explicit AudioRead(QObject *parent = nullptr);
    ~AudioRead();

public slots:
    // 开始采集（接口不变，内部实现替换为 SDL）
    void start();
    // 暂停采集（接口不变，内部实现替换为 SDL）
    void pause();

signals:
    // 音频帧信号（接口不变，数据内容从 Speex 变为 Opus）
    void SIG_audioFrame( QByteArray bt );

    // 静音状态变化信号（WebRTC VAD 检测结果上报）
    // isMute=true : 进入静音状态（连续 MUTE_CONFIRM_THRESHOLD 帧判定为无声）
    // isMute=false: 退出静音状态（开始说话）
    // 用途：ckernel 接收后联动 RoomDialog 更新麦克风 UI 反馈
    void SIG_muteStateChanged(bool isMute);

private slots:
    // VAD 模式动态调整槽（QTimer 每 5 秒触发一次）
    // 功能：根据环境噪声 EMA 估计自动切换 VAD 灵敏度模式（1=宽松/2=中等/3=严格）
    void slot_adjustVadMode();

private:
    // QAudioInput 相关成员
    // 修改原因：QAudioInput 定时器轮询模式精度不足，高负载下丢帧
    // 修改内容：替换为 SDL 采集设备
    /* 原代码（QAudioInput采集）：
    QAudioInput * m_audio_in;    // 采集声卡 音频输入
    QIODevice * m_buffer_in;     // 对应的缓冲区
    QTimer * m_timer;            // 定时器，20ms轮询
    QAudioFormat format;         // 音频格式
    */

    // Speex 编码器相关变量
    // 修改原因：Speex 已停止维护（2014），Opus 是其官方继任者
    // 修改内容：替换为 OpusEncoder
    /* 原代码（Speex编码器）：
    SpeexBits bits_enc;
    void *Enc_State;
    */

    // SDL 采集设备
    // 功能：通过 SDL_OpenAudioDevice(iscapture=1) 打开麦克风采集
    // 驱动模式：SDL 内部音频线程每 20ms 调用 audioCallback
    SDL_AudioDeviceID m_sdlCaptureDev;
    bool m_sdlIsOpen;  // SDL 采集设备是否已打开

    // Opus 编码器
    // 参数：48kHz 采样率，单声道，VOIP 模式
    // 特性：内置 DTX（不连续传输），静音时带宽降至 ~1kbps
    OpusEncoder *m_opusEncoder;

    // WebRTC VAD 静音检测（与 Opus DTX 双重保障）
    VadInst *handle;
    //【修复-P2：VAD 句柄失败仍被使用】create/init 任一失败置 false，
    // 回调线程据此跳过 WebRtcVad_Process（对无效句柄调用是未定义行为）。
    bool m_vadEnabled;

    // SDL 采集回调函数（静态函数，SDL 音频线程调用）
    // 功能：每 20ms 被调用一次，接收 960 采样（1920字节）
    // 流程：VAD 静音检测 → Opus 编码 → 发射 SIG_audioFrame 信号
    // 参考：AudioDemo2/OpusApi/sdlaudioread.cpp::audioCallback
    static void audioCallback(void *userdata, Uint8 *stream, int len);

    int m_AudioState;

    // ===== WebRTC VAD 静音检测增强成员 =====
    // 动态阈值调整相关（slot_adjustVadMode 使用）
    int m_vadMode;                  // 当前 VAD 模式（0-3，默认 2=VoIP 推荐）
    float m_noiseLevel;             // 当前帧 RMS 能量（audioCallback 写入，slot_adjustVadMode 读取，单 float 良性跨线程）
    float m_noiseLevelEMA;          // 噪声水平的指数移动平均（EMA）
    int m_consecutiveSilentFrames;  // 连续静音帧计数
    int m_consecutiveSpeechFrames;  // 连续有声帧计数
    QTimer *m_vadAdjustTimer;       // VAD 模式动态调整定时器（5 秒间隔）

    // 状态平滑相关（避免说话停顿瞬间频繁切换状态）
    bool m_currentMuteState;        // 当前已确认的静音状态
    bool m_pendingMuteState;        // 待确认的静音状态
    int m_muteConfirmFrames;        // 状态确认帧计数
    static const int MUTE_CONFIRM_THRESHOLD = 3;  // 连续 3 帧(≈60ms)确认才切换状态

    // 工具方法：计算 PCM 数据的 RMS 能量（归一化到 0.0-1.0）
    float calculateRMS(int16_t *data, int samples);
};

#endif // AUDIOREAD_H
