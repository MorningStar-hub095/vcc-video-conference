#ifndef AUDIOWRITE_H
#define AUDIOWRITE_H

#include <QObject>
#include "World.h"

// AudioWrite 类变更
// 修改原因：
//   1. 解码器从 Speex 迁移到 Opus（与 AudioRead 编码器对齐）
//   2. SDL 播放参数从 8kHz 调整为 48kHz（与 Opus 编码采样率一致）
// 修改内容：
//   1. 移除 SpeexBits/Dec_State 成员
//   2. 新增 OpusDecoder *m_opusDecoder
//   3. SDL 播放参数调整为 48kHz/960samples
// 预期效果：与 AudioRead 的 Opus 编码形成完整的编解码链路
// 参考：AudioDemo2/OpusApi/sdlaudiowrite.h


class AudioWrite : public QObject
{
    Q_OBJECT
public:
    explicit AudioWrite(QObject *parent = nullptr);
    ~AudioWrite();

signals:

public slots:
    // 音频帧播放槽函数（接口不变，内部解码逻辑替换）
    void slot_playAudio(QByteArray bt);

private:
    // 原代码使用 QAudioOutput播放音频
    // 已在上一轮迁移中替换为 SDL 播放设备
    /* 原代码（QAudioOutput，已废弃）：
    QAudioOutput * m_audio_out;
    QIODevice * m_buffer_out;
    QAudioFormat format;
    */

    // SDL 音频播放相关成员（已迁移完成）
    SDL_AudioDeviceID m_sdlDev;          // SDL音频设备ID
    bool m_sdlIsOpen;                     // SDL设备是否已打开
    std::deque<QByteArray> m_audioQueue;  // 音频数据队列（Opus编码数据）
    std::mutex m_mutex;                   // 队列访问互斥锁

    // SDL 音频回调函数（SDL内部线程调用）
    static void audioCallback(void *userdata, Uint8 *stream, int len);

    // Speex 解码器相关变量
    // 修改原因：Speex 已停止维护，迁移到 Opus
    // 修改内容：替换为 OpusDecoder
    /* 原代码（Speex解码器）：
    SpeexBits bits_dec;
    void *Dec_State;
    */

    // Opus 解码器
    // 参数：48kHz 采样率，单声道
    // 在 audioCallback 中解码 Opus 数据为 PCM
    OpusDecoder *m_opusDecoder;
};

#endif // AUDIOWRITE_H
