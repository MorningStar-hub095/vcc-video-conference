#include "audioread.h"
#include <cmath>   // calculateRMS 计算均方根时 sqrt 需要

// AudioRead 构造函数重构
// 修改原因：音频采集从 QAudioInput 迁移到 SDL，编码器从 Speex 迁移到 Opus
// 修改内容：
//   1. Opus 编码器初始化（48kHz, 单声道, VOIP模式, 32kbps, DTX启用）
//   2. WebRTC VAD 初始化（保留，与 Opus DTX 双重保障）
//   3. SDL 采集设备延迟到 start() 中打开
// 预期效果：构造时不打开音频设备，避免启动时设备占用
// 参考：AudioDemo2/OpusApi/sdlaudioread.cpp 构造函数

AudioRead::AudioRead(QObject *parent) : QObject(parent)
{
    m_AudioState = stopped;
    m_sdlIsOpen = false;
    m_sdlCaptureDev = 0;
    m_opusEncoder = nullptr;

    // Speex 编码器初始化
    // 修改原因：Speex 已停止维护，迁移到 Opus
    /* 原代码（Speex初始化）：
    speex_bits_init(&bits_enc);
    Enc_State = speex_encoder_init(speex_lib_get_mode(SPEEX_MODEID_NB));
    int tmp = SPEEX_QUALITY;
    speex_encoder_ctl(Enc_State, SPEEX_SET_QUALITY, &tmp);
    */

    //  QAudioInput 音频格式配置
    // 修改原因：迁移到 SDL 采集，参数在 start() 中配置
    /* 原代码（QAudioInput格式配置）：
    format.setSampleRate(8000);
    format.setChannelCount(1);
    format.setSampleSize(16);
    format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::UnSignedInt);
    QAudioDeviceInfo info = QAudioDeviceInfo::defaultInputDevice();
    if (!info.isFormatSupported(format)) {
        QMessageBox::information(NULL, "提示", "打开音频设备失败");
        format = info.nearestFormat(format);
    }
    m_audio_in = NULL;
    m_buffer_in = NULL;
    */

    //  QTimer 定时器初始化
    // 修改原因：SDL 采集由独立线程回调驱动，不再需要 QTimer
    /* 原代码（QTimer初始化）：
    m_timer = new QTimer;
    connect(m_timer, SIGNAL(timeout()), this, SLOT(ReadMore()));
    */

    //  Opus 编码器初始化
    // 参数：48kHz 采样率（Opus 原生优化速率），单声道，VOIP 模式
    // VOIP 模式针对语音通话优化，内置 DTX 和 PLC
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

    //  WebRTC VAD 静音检测初始化
    // 与 Opus DTX 双重保障：VAD 在编码前清零静音帧，DTX 在编码时压缩静音帧
#ifdef USE_VAD
    if( 0 != WebRtcVad_Create( &handle ))
    {
        qDebug()<< "[VAD] vad create fail";
    }
    if( 0 != WebRtcVad_Init(handle))
    {
        qDebug()<< "[VAD] vad init fail";
    }
    // 设置 VAD 灵敏度模式为 2（中等偏高，VoIP 推荐档）
    // 模式 0=宽松 / 1=中等偏低 / 2=中等偏高(推荐) / 3=严格
    // 后续由 slot_adjustVadMode 根据环境噪声动态调整
    WebRtcVad_set_mode(handle, 2);
    qDebug()<< "[VAD] vad mode set to 2 (VoIP recommended)";
#endif

    // ===== VAD 动态阈值调整成员初始化 =====
    m_vadMode = 2;                  // 默认中等灵敏度（与上方 set_mode 一致）
    m_noiseLevel = 0.0f;            // 当前帧 RMS（audioCallback 每帧更新）
    m_noiseLevelEMA = 0.0f;         // 噪声 EMA 初始化为 0
    m_consecutiveSilentFrames = 0;
    m_consecutiveSpeechFrames = 0;

    // 每 5 秒评估一次环境噪声，动态调整 VAD 模式
    // 注意：QTimer 在主线程触发 slot_adjustVadMode，读取 SDL 线程写入的 m_noiseLevel
    //       （单 float 读写，实际为良性跨线程竞争，与文档第八章设计一致，不引入互斥锁）
    m_vadAdjustTimer = new QTimer(this);
    m_vadAdjustTimer->setInterval(5000);  // 5 秒间隔
    connect(m_vadAdjustTimer, &QTimer::timeout,
            this, &AudioRead::slot_adjustVadMode);
    m_vadAdjustTimer->start();

    // ===== 静音状态平滑成员初始化 =====
    m_currentMuteState = false;     // 初始状态：有声
    m_pendingMuteState = false;
    m_muteConfirmFrames = 0;
}

//  析构函数重构
// 修改原因：替换 QAudioInput/Speex 资源释放为 SDL/Opus 资源释放
// 修改内容：
//   1. SDL 采集设备关闭
//   2. Opus 编码器销毁
//   3. WebRTC VAD 释放（保留）

AudioRead::~AudioRead()
{
    //  QTimer/QAudioInput 释放
    /* 原代码：
    if( m_timer )
    {
        m_timer->stop();
        delete m_timer;
        m_timer = NULL;
    }
    if( m_audio_in )
    {
        m_audio_in->stop();
        delete m_audio_in;
        m_audio_in = NULL;
    }
    */

    //  SDL 采集设备释放
    if (m_sdlIsOpen && m_sdlCaptureDev != 0) {
        SDL_PauseAudioDevice(m_sdlCaptureDev, 1);  // 暂停采集
        SDL_CloseAudioDevice(m_sdlCaptureDev);       // 关闭设备
        m_sdlCaptureDev = 0;
        m_sdlIsOpen = false;
        qDebug() << "[SDL-Capture] 采集设备已关闭";
    }

    //  Opus 编码器销毁
    if (m_opusEncoder) {
        opus_encoder_destroy(m_opusEncoder);
        m_opusEncoder = nullptr;
        qDebug() << "[Opus] 编码器已销毁";
    }

    // VAD 动态调整定时器释放（先停后删，避免悬空回调）
    if (m_vadAdjustTimer) {
        m_vadAdjustTimer->stop();
        delete m_vadAdjustTimer;
        m_vadAdjustTimer = nullptr;
        qDebug() << "[VAD] 动态调整定时器已释放";
    }

    //  WebRTC VAD 释放
#ifdef USE_VAD
    WebRtcVad_Free(handle);
#endif
}

//  ReadMore() 函数
// 修改原因：QTimer 轮询模式替换为 SDL 回调模式，数据采集在 audioCallback 中完成
// 修改内容：整个函数不再使用，编码逻辑迁移到 audioCallback 中
/* 原代码（ReadMore函数，Qt定时器驱动的采集+编码）：
void AudioRead::ReadMore()
{
#ifdef USE_SPEEX
    if (!m_audio_in) return;
    QByteArray m_buffer(2048,0);
    qint64 len = m_audio_in->bytesReady();
    if (len < 640) return;
    qint64 l = m_buffer_in->read(m_buffer.data(), 640);
    QByteArray frame;
#ifdef USE_VAD
    char* bufferdata = (char*)m_buffer.data();
    for( int i = 0; i < 640 ; i += 320)
    {
        char* tmp = bufferdata + i;
        if( 0 == WebRtcVad_Process( handle , 8000 , (int16_t*)tmp , 160 ))
        {
            memset( tmp , 0 , 320);
        }
    }
#endif
    char bytes = {0};
    int i = 0;
    float input_frame1;
    char buf = {0};
    char* pInData = (char*)m_buffer.data() ;
    memcpy( buf , pInData , 640);
    int nbytes = 0;
    {
        short num = 0;
        for (i = 0;i < 160;i++)
        {
            num = (uint8_t)buf | (((uint8_t)buf) << 8);
            input_frame1 = num;
        }
        speex_bits_reset(&bits_enc);
        speex_encode(Enc_State,input_frame1,&bits_enc);
        nbytes = speex_bits_write(&bits_enc,bytes,800);
        frame.append(bytes,nbytes);
        for (i = 0;i < 160;i++)
        {
            num = (uint8_t)buf | (((uint8_t)buf) << 8);
            input_frame1 = num;
        }
        speex_bits_reset(&bits_enc);
        speex_encode(Enc_State,input_frame1,&bits_enc);
        nbytes = speex_bits_write(&bits_enc,bytes,800);
        frame.append(bytes,nbytes);
        qDebug() << "nbytes = " << frame.size();
        Q_EMIT SIG_audioFrame (frame);
        return;
    }
#else
    if (!m_audio_in) return;
    QByteArray m_buffer(2048,0);
    qint64 len = m_audio_in->bytesReady();
    if (len < 640) return;
    qint64 l = m_buffer_in->read(m_buffer.data(), 640);
    QByteArray frame;
    frame.append(m_buffer.data(),640);
    Q_EMIT SIG_audioFrame( frame );
#endif
}
*/

//  SDL 采集回调函数（SDL 音频线程调用，每 20ms 触发一次）
// 功能：接收麦克风采集的 PCM 数据，进行 VAD 静音检测和 Opus 编码
// 参数：
//   - userdata: AudioRead 实例指针（通过 wantedSpec.userdata 传入）
//   - stream: PCM 数据缓冲区（960采样 × 2字节 = 1920字节）
//   - len: 缓冲区长度（1920字节）
// 线程安全：此函数在 SDL 音频线程中调用，通过 Qt 信号跨线程传递数据
// 参考：AudioDemo2/OpusApi/sdlaudioread.cpp::audioCallback

void AudioRead::audioCallback(void *userdata, Uint8 *stream, int len)
{
    AudioRead *audio = (AudioRead *)userdata;

    // 校验数据长度：960采样 × 2字节 = 1920字节
    if (len < 1920) return;

    //  WebRTC VAD 静音检测
    // 每 960 采样检测一次，返回 0 表示静音，1 表示有声
    // 静音帧清零，减少 Opus 编码的无效数据
    /* 原代码（基础VAD：仅检测+清零，无状态平滑、无UI联动）：
#ifdef USE_VAD
    if (0 == WebRtcVad_Process(audio->handle, 48000, (int16_t*)stream, 960))
    {
        // 静音帧清零（Opus DTX 会进一步压缩静音帧）
        memset(stream, 0, len);
    }
#endif
    */
    // 增强版 VAD：RMS 能量统计 + 状态平滑 + 静音状态信号上报
    // 修改原因：原基础版无法感知静音状态、易因停顿抖动、不能自适应环境噪声
    bool isMute = false;
#ifdef USE_VAD
    // VAD 检测：返回 0=静音，1=有声，-1=错误（错误时按有声处理，避免误清零）
    int vadResult = WebRtcVad_Process(audio->handle, 48000,
                                       (int16_t*)stream, 960);
    isMute = (vadResult == 0);

    // 计算当前帧 RMS 能量，供 slot_adjustVadMode 动态阈值调整使用
    audio->m_noiseLevel = audio->calculateRMS((int16_t*)stream, 960);

    // 状态平滑：连续 MUTE_CONFIRM_THRESHOLD(3) 帧确认才切换状态
    // 避免说话停顿瞬间频繁切换（3 帧 ≈ 60ms）
    if (isMute != audio->m_pendingMuteState) {
        // 待确认状态发生变化，重置计数器开始确认
        audio->m_pendingMuteState = isMute;
        audio->m_muteConfirmFrames = 1;
    } else {
        // 待确认状态延续，累加计数
        audio->m_muteConfirmFrames++;
        // 达到确认阈值且状态确实改变，触发静音状态变化信号
        if (audio->m_muteConfirmFrames >= AudioRead::MUTE_CONFIRM_THRESHOLD &&
            audio->m_pendingMuteState != audio->m_currentMuteState) {
            audio->m_currentMuteState = audio->m_pendingMuteState;
            // 跨线程信号（QueuedConnection 由 ckernel 端自动处理）
            Q_EMIT audio->SIG_muteStateChanged(audio->m_currentMuteState);
            qDebug() << "[VAD] 静音状态切换:" << audio->m_currentMuteState;
        }
    }

    // 静音帧清零（Opus DTX 会进一步压缩静音帧，与原逻辑保持一致）
    if (isMute) {
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

//  start() 函数重构
// 修改原因：从 QAudioInput 启动替换为 SDL 采集设备启动
// 修改内容：
//   1. 首次调用时打开 SDL 采集设备（延迟初始化）
//   2. 后续调用只需 SDL_PauseAudioDevice(0) 恢复采集
// 预期效果：采集启动更可靠，支持暂停/恢复
// 参考：AudioDemo2/OpusApi/sdlaudioread.h::slot_openAudio

void AudioRead::start()
{
    //  QAudioInput 启动逻辑
    /* 原代码：
    if(m_AudioState == stopped || m_AudioState == pausing)
    {
        m_audio_in = new QAudioInput( format , this );
        m_buffer_in = m_audio_in->start();
        m_timer->start(20);
        m_AudioState = playing;
    }
    */

    //  SDL 采集设备启动
    if (m_AudioState == stopped || m_AudioState == pausing)
    {
        // 首次调用时打开 SDL 采集设备（延迟初始化）
        if (!m_sdlIsOpen) {
            SDL_AudioSpec desiredSpec;
            SDL_AudioSpec obtainedSpec;
            SDL_zero(desiredSpec);

            // 采集参数：48kHz/16bit/单声道/960采样（20ms）
            // 与 AudioDemo2 的 SDLAudioRead 参数一致
            desiredSpec.freq = 48000;           // 采样率 48kHz（Opus 原生优化）
            desiredSpec.format = AUDIO_S16LSB;  // 16位有符号小端序
            desiredSpec.channels = 1;           // 单声道
            desiredSpec.samples = 960;          // 每次回调 960 采样（20ms @ 48kHz）
            desiredSpec.callback = audioCallback;  // 采集回调函数
            desiredSpec.userdata = this;           // 传递 AudioRead 实例指针

            // 打开 SDL 采集设备（iscapture=1 表示录音）
            // 参考：AudioDemo2/sdlaudioread.cpp 中 SDL_OpenAudioDevice(NULL, 1, ...)
            m_sdlCaptureDev = SDL_OpenAudioDevice(NULL, 1,
                                &desiredSpec, &obtainedSpec, 0);
            if (m_sdlCaptureDev == 0) {
                qDebug() << "[SDL-Capture] 打开采集设备失败:" << SDL_GetError();
                return;
            }
            m_sdlIsOpen = true;
            qDebug() << "[SDL-Capture] 采集设备已打开, devID:" << m_sdlCaptureDev
                     << "freq:" << obtainedSpec.freq
                     << "samples:" << obtainedSpec.samples;
        }

        // 开始采集（取消暂停）
        SDL_PauseAudioDevice(m_sdlCaptureDev, 0);
        m_AudioState = playing;
        qDebug() << "[SDL-Capture] 采集已启动";
    }
}

//  pause() 函数重构
// 修改原因：从 QAudioInput 暂停替换为 SDL 采集设备暂停
// 修改内容：SDL_PauseAudioDevice(1) 暂停采集，保留设备不关闭
// 预期效果：支持快速恢复采集，无需重新打开设备
// 参考：AudioDemo2/OpusApi/sdlaudioread.h::slot_closeAudio

void AudioRead::pause()
{
    //  QAudioInput 暂停逻辑（已废弃，保留注释）
    /* 原代码：
    if(m_AudioState == playing)
    {
        m_timer->stop();
        if(m_audio_in)
        {
            m_audio_in->stop();
            delete  m_audio_in;
            m_audio_in = NULL;
        }
        m_AudioState = pausing ;
    }
    */

    //  SDL 采集设备暂停
    if (m_AudioState == playing)
    {
        if (m_sdlIsOpen && m_sdlCaptureDev != 0) {
            SDL_PauseAudioDevice(m_sdlCaptureDev, 1);  // 暂停采集
        }
        m_AudioState = pausing;
        qDebug() << "[SDL-Capture] 采集已暂停";
    }
}

// ===== WebRTC VAD 静音检测增强方法实现 =====

// 计算 PCM 数据的 RMS 能量值（归一化到 0.0-1.0）
// 参数：data - int16_t PCM 数据；samples - 采样数
// 返回：归一化 RMS（0.0=完全静音，1.0=最大音量）
// 用途：供 slot_adjustVadMode 估计环境噪声水平，动态调整 VAD 灵敏度
float AudioRead::calculateRMS(int16_t *data, int samples)
{
    if (!data || samples <= 0) return 0.0f;

    double sum = 0.0;
    for (int i = 0; i < samples; i++) {
        sum += (double)data[i] * data[i];
    }
    double rms = sqrt(sum / samples);
    // 归一化到 0.0-1.0（int16_t 最大值 32767）
    return (float)(rms / 32767.0);
}

// 动态调整 VAD 模式（QTimer 每 5 秒触发一次）
// 策略：用噪声 EMA 估计环境噪声水平，按三档选择 VAD 灵敏度
//   噪声 EMA < 0.02  → 模式 1（宽松，安静环境，避免漏检轻声说话）
//   噪声 EMA < 0.10  → 模式 2（中等，办公室/居家，平衡）
//   噪声 EMA >= 0.10 → 模式 3（严格，嘈杂环境，避免误判噪声为语音）
// 仅当模式变化时才调用 WebRtcVad_set_mode，避免频繁切换
void AudioRead::slot_adjustVadMode()
{
    // EMA 平滑系数：0.95 偏向历史值，避免短期波动影响估计
    const float EMA_ALPHA = 0.95f;

    // 更新噪声水平 EMA 估计（当前帧 m_noiseLevel 视为噪声水平采样）
    m_noiseLevelEMA = EMA_ALPHA * m_noiseLevelEMA +
                      (1.0f - EMA_ALPHA) * m_noiseLevel;

    // 根据噪声水平动态选择 VAD 模式
    int newMode = m_vadMode;
    if (m_noiseLevelEMA < 0.02f) {
        // 噪声水平极低（安静环境），使用宽松模式，避免漏检轻声说话
        newMode = 1;
    } else if (m_noiseLevelEMA < 0.10f) {
        // 噪声水平中等（办公室/居家环境），使用推荐模式
        newMode = 2;
    } else {
        // 噪声水平较高（嘈杂环境），使用严格模式，避免误判环境噪声为语音
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
