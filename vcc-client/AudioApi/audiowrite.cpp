#include "audiowrite.h"

//  AudioWrite 构造函数重构
// 修改原因：
//   1. 解码器从 Speex 迁移到 Opus
//   2. SDL 播放参数从 8kHz/160samples 调整为 48kHz/960samples
// 修改内容：
//   1. Opus 解码器初始化（48kHz, 单声道）
//   2. SDL 播放设备参数调整为 48kHz
// 预期效果：与 AudioRead 的 48kHz Opus 编码形成完整链路
// 参考：AudioDemo2/OpusApi/sdlaudiowrite.cpp 构造函数

AudioWrite::AudioWrite(QObject *parent) : QObject(parent)
{
    //  Speex 解码器初始化
    // 修改原因：Speex 已停止维护，迁移到 Opus
    /* 原代码（Speex解码器初始化）：
    speex_bits_init(&bits_dec);
    Dec_State = speex_decoder_init(speex_lib_get_mode(SPEEX_MODEID_NB));
    */

    //  Opus 解码器初始化（48kHz, 单声道）
    // 与 AudioRead 的 Opus 编码器参数对齐
    int err;
    m_opusDecoder = opus_decoder_create(48000, 1, &err);
    if (err != OPUS_OK || !m_opusDecoder) {
        qDebug() << "[Opus] 解码器创建失败:" << opus_strerror(err);
    } else {
        qDebug() << "[Opus] 解码器创建成功, 48kHz/单声道";
    }

    //  QAudioInput 播放设备初始化
    /* 原代码：
    format.setSampleRate(8000);
    format.setChannelCount(1);
    format.setSampleSize(16);
    format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::UnSignedInt);
    QAudioDeviceInfo info = QAudioDeviceInfo::defaultInputDevice();
    if (!info.isFormatSupported(format)) {
        QMessageBox::information(NULL , "提示", "打开音频设备失败");
        format = info.nearestFormat(format);
    }
    m_audio_out = new QAudioOutput( format , this );
    m_buffer_out = m_audio_out->start();
    */

    //  SDL 播放设备参数调整为 48kHz
    // 修改原因：与 AudioRead 的 48kHz Opus 编码采样率一致
    // 修改内容：freq 8000→48000, samples 160→960
    // 预期效果：播放采样率与编码采样率匹配，避免重采样
    SDL_AudioSpec wantedSpec;
    SDL_AudioSpec obtainedSpec;
    SDL_zero(wantedSpec);
    wantedSpec.freq = 48000;          //  原为 8000，升级为 48000（Opus 原生优化）
    wantedSpec.format = AUDIO_S16LSB; //  16位有符号小端序
    wantedSpec.channels = 1;          //  单声道
    wantedSpec.samples = 960;         //  原为 160，升级为 960（20ms @ 48kHz）
    wantedSpec.callback = audioCallback;  //  回调函数
    wantedSpec.userdata = this;           //  传递this指针

    //  打开SDL播放设备（iscapture=0表示播放）
    m_sdlDev = SDL_OpenAudioDevice(NULL, 0, &wantedSpec, &obtainedSpec, 0);
    if (m_sdlDev == 0) {
        qDebug() << "[SDL] 打开播放设备失败:" << SDL_GetError();
        m_sdlIsOpen = false;
    } else {
        qDebug() << "[SDL] 播放设备已打开, devID:" << m_sdlDev
                 << "freq:" << obtainedSpec.freq
                 << "samples:" << obtainedSpec.samples;
        m_sdlIsOpen = true;
        // 开始播放（取消暂停）
        SDL_PauseAudioDevice(m_sdlDev, 0);
    }
}

//  析构函数重构
// 修改原因：替换 Speex 解码器释放为 Opus 解码器释放

AudioWrite::~AudioWrite()
{
    //  SDL 播放设备释放（不变）
    if (m_sdlIsOpen && m_sdlDev != 0) {
        SDL_PauseAudioDevice(m_sdlDev, 1);  // 暂停播放
        SDL_CloseAudioDevice(m_sdlDev);      // 关闭设备
        m_sdlDev = 0;
        m_sdlIsOpen = false;
        qDebug() << "[SDL] 播放设备已关闭";
    }

    //  Speex 解码器释放
    /* 原代码（Speex解码器释放）：
    speex_decoder_destroy(Dec_State);
    speex_bits_destroy(&bits_dec);
    */

    //  Opus 解码器释放
    if (m_opusDecoder) {
        opus_decoder_destroy(m_opusDecoder);
        m_opusDecoder = nullptr;
        qDebug() << "[Opus] 解码器已销毁";
    }
}

//  slot_playAudio 函数简化
// 修改原因：
//   - 原代码需要先 Speex 解码再入队，现在 Opus 数据直接入队
//   - 解码工作在 audioCallback 中完成（SDL 线程）
// 修改内容：移除 Speex 解码逻辑，Opus 数据直接入队
// 预期效果：简化主线程工作，解码在 SDL 线程中完成
// 参考：AudioDemo2/OpusApi/sdlaudiowrite.cpp::slot_playAudioFrame

void AudioWrite::slot_playAudio( QByteArray bt)
{
    //  Speex 解码逻辑
    // 修改原因：Opus 数据直接入队，解码在 audioCallback 中完成
    /* 原代码（Speex解码+入队）：
#ifdef USE_SPEEX
    char bytes = {0};
    int i = 0;
    float output_frame1 = {0};
    char buf = {0};
    memcpy(bytes,bt.data(),bt.length() / 2);
    speex_bits_read_from(&bits_dec,bytes,bt.length() / 2);
    int error = speex_decode(Dec_State,&bits_dec,output_frame1);
    if (error < 0) {
        qDebug() << " Decode error frame1:" << error;
        return;
    }
    short num = 0;
    for (i = 0;i < 160;i++)
    {
        num = output_frame1;
        buf = num;
        buf = num >> 8;
    }
    memcpy(bytes,bt.data() + bt.length() / 2,bt.length() / 2);
    speex_bits_read_from(&bits_dec,bytes,bt.length() / 2);
    error = speex_decode(Dec_State,&bits_dec,output_frame1);
    if (error < 0) {
        qDebug() << " Decode error frame2:" << error;
        return;
    }
    for (i = 0;i < 160;i++)
    {
        num = output_frame1;
        buf = num;
        buf = num >> 8;
    }
    if (m_sdlIsOpen) {
        QByteArray pcmData(buf, 640);
        std::lock_guard<std::mutex> lck(m_mutex);
        m_audioQueue.emplace_back(pcmData);
    }
    return;
#else
    if( bt.size() < 640) return;
    if (m_sdlIsOpen) {
        QByteArray pcmData(bt.data(), 640);
        std::lock_guard<std::mutex> lck(m_mutex);
        m_audioQueue.emplace_back(pcmData);
    }
#endif
    */

    //  Opus 数据直接入队
    // Opus 编码数据直接入队，解码在 audioCallback 中完成
    // 参考：AudioDemo2/OpusApi/sdlaudiowrite.cpp::slot_playAudioFrame
    if (m_sdlIsOpen) {
        std::lock_guard<std::mutex> lck(m_mutex);
        //【P2修复：音频队列无上限】
        // 修改原因: 原代码无条件入队；当播放消费速度 < 网络到达速度（弱网恢复后
        //           突发、服务端转发风暴）时队列无限增长，内存持续上涨且延迟越来越大。
        // 修改效果: 队列超过 100 帧（约 2 秒 @50帧/s）时丢弃最旧数据再入队，
        //           内存有界、播放延迟有界；正常速率下行为不变。
        // 原代码保留备查：
        //     m_audioQueue.emplace_back(bt);
        while( m_audioQueue.size() >= 100 )
        {
            m_audioQueue.pop_front(); // 丢最旧帧，保实时性
        }
        m_audioQueue.emplace_back(bt);
    }
}

//  SDL 音频回调函数重构
// 修改原因：解码逻辑从 Speex 替换为 Opus
// 修改内容：
//   1. 从队列取出 Opus 编码数据
//   2. Opus 解码为 PCM（opus_decode）
//   3. SDL_MixAudioFormat 混音到播放缓冲区
// 预期效果：Opus 解码在 SDL 线程中完成，与编码端形成完整链路
// 参考：AudioDemo2/OpusApi/sdlaudiowrite.cpp::audioCallback

void AudioWrite::audioCallback(void *userdata, Uint8 *stream, int len)
{
    AudioWrite *audio = (AudioWrite *)userdata;
    // 先将SDL缓冲区清零（静音填充）
    memset(stream, 0, len);

    //【修复-P0：音频队列锁外访问（并发 UB）】empty()/front() 移入互斥锁内
    // 修改原因: 原代码 empty()/front() 在锁外访问 std::deque，仅 pop_front 持锁。
    //           SDL 回调线程（audioCallback）与主线程 slot_playAudio（持锁 push，
    //           见本文件第 170-173 行）每 20ms 并发一次，deque 扩容/元素移动期间
    //           锁外读取会撕裂——长期运行大概率崩溃（隐藏最深的一个 P0）。
    // 修改效果: empty/front/pop 三个操作全部移入同一 lock_guard 作用域，
    //           取帧与入队完全互斥；取帧逻辑与解码逻辑不变。
    // 原代码:
    //     if (!audio->m_audioQueue.empty()) {
    //         // 从队列头部取出一帧 Opus 编码数据
    //         QByteArray recvBuffer = audio->m_audioQueue.front();
    //         {
    //             std::lock_guard<std::mutex> lck(audio->m_mutex);
    //             audio->m_audioQueue.pop_front();
    //         }
    QByteArray recvBuffer;
    {
        std::lock_guard<std::mutex> lck(audio->m_mutex);
        if (!audio->m_audioQueue.empty()) {
            // 从队列头部取出一帧 Opus 编码数据（整体在锁内完成）
            recvBuffer = audio->m_audioQueue.front();
            audio->m_audioQueue.pop_front();
        }
    }
    if (!recvBuffer.isEmpty()) {

        //  Speex 解码逻辑（已在上一轮迁移中废弃）
        // 解码工作已迁移到此处的 Opus 解码

        //  Opus 解码
        // 输入：Opus 编码数据（约 60-120 字节）
        // 输出：PCM 数据（960采样 × 2字节 = 1920字节）
        // 参考：AudioDemo2/OpusApi/sdlaudiowrite.cpp::audioCallback
        //【修复-P2：解码缓冲容量不足】48kHz 单声道 Opus 单包最大可输出
        // 120ms = 5760 采样（FEC/PLC 或畸形包会触发），4096 会导致解码截断失败。
        // 原代码: opus_int16 decodedData[4096];
        opus_int16 decodedData[5760];
        int frameSizeDecoded = opus_decode(audio->m_opusDecoder,
                                            (const unsigned char*)recvBuffer.data(),
                                            recvBuffer.size(),
                                            decodedData,
                                            sizeof(decodedData)/sizeof(decodedData[0]),
                                            0);
        if (frameSizeDecoded < 0) {
            qDebug() << "[Opus] 解码错误:" << opus_strerror(frameSizeDecoded);
            return;
        }

        //  将解码后的 PCM 数据混音到 SDL 缓冲区
        // 修改内容：数据源从 Speex 解码结果替换为 Opus 解码结果
        // 数据长度：frameSizeDecoded × sizeof(opus_int16)（约 1920 字节）
        SDL_MixAudioFormat(stream, (uint8_t*)decodedData, AUDIO_S16LSB,
                           frameSizeDecoded * sizeof(opus_int16), 100);
    }
    // 队列为空时，stream保持为全零（静音），不会产生噪音
}
