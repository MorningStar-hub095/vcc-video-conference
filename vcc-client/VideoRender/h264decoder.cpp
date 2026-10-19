#include "h264decoder.h"
#include <QDebug>
#include <cstring>  // for memcpy

// 构造函数初始化成员
H264Decoder::H264Decoder(QObject *parent): QObject(parent) , m_codecCtx(nullptr) , m_frame(nullptr) , m_pkt(nullptr) , m_initialized(false)
{

}

// 析构函数确保资源释放
H264Decoder::~H264Decoder()
{
    close();
}

// 初始化解码器
// 参考: MediaPlayer/videoplayer.cpp 的 find_stream_index + avcodec_open2 流程
// 改进: 使用新版 API（avcodec_send_packet/avcodec_receive_frame）
//       添加硬件解码器探测
bool H264Decoder::init()
{
    if (m_initialized) {
        return true;
    }
    return initCodec();
}

// 内部初始化编码器
// 解码器选择逻辑：逐个尝试解码器，每个都执行 avcodec_open2 验证可用性
// 原Bug: 与编码器相同，仅用 avcodec_find_decoder_by_name 查找第一个注册的解码器，
//        硬件解码器(如h264_dxva2)已注册但可能无法打开，导致直接返回false，
//        不会继续尝试h264软解码器。
// 修复方案: 将"查找"和"打开"合并到同一个循环中，avcodec_open2失败则尝试下一个解码器。
// 硬件解码回退链: h264_dxva2 -> h264_cuvid -> h264_qsv -> h264(软解)
bool H264Decoder::initCodec()
{
    const char *decoder_names[] = {
        "h264_dxva2",   // Windows 通用硬解
        "h264_cuvid",   // N 卡硬解
        "h264_qsv",     // Intel 硬解
        "h264"          // 软解兜底
    };
    const AVCodec *codec = nullptr;
    bool codecOpened = false;

    for (size_t i = 0; i < sizeof(decoder_names)/sizeof(decoder_names[0]); ++i) {
        codec = avcodec_find_decoder_by_name(decoder_names[i]);
        if (!codec) {
            qDebug() << "H264Decoder: decoder" << decoder_names[i] << "not registered, skip";
            continue;
        }

        // 分配临时解码器上下文用于测试打开
        AVCodecContext *testCtx = avcodec_alloc_context3(codec);
        if (!testCtx) {
            qDebug() << "H264Decoder: alloc context failed for" << decoder_names[i] << ", skip";
            continue;
        }

        testCtx->refcounted_frames = 1;

        // 尝试打开解码器
        int ret = avcodec_open2(testCtx, codec, nullptr);
        if (ret < 0) {
            //  记录解码器打开失败的原因
            qDebug() << "H264Decoder: avcodec_open2 failed for" << decoder_names[i]
                     << ", ret=" << ret << ", trying next decoder";
            avcodec_free_context(&testCtx);
            continue;
        }

        // 解码器打开成功
        qDebug() << "H264Decoder: using decoder" << decoder_names[i]
                 << "(verified by avcodec_open2)";
        m_codecCtx = testCtx;
        codecOpened = true;
        break;
    }

    if (!codecOpened) {
        // 所有命名解码器都失败了，尝试通过ID查找兜底
        qDebug() << "H264Decoder: all named decoders failed, trying AV_CODEC_ID_H264 fallback";
        codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) {
            qCritical() << "H264Decoder: codec H264 not found at all";
            return false;
        }

        m_codecCtx = avcodec_alloc_context3(codec);
        if (!m_codecCtx) {
            qCritical() << "H264Decoder: alloc context failed for fallback";
            return false;
        }

        m_codecCtx->refcounted_frames = 1;

        int ret = avcodec_open2(m_codecCtx, codec, nullptr);
        if (ret < 0) {
            qCritical() << "H264Decoder: avcodec_open2 failed for fallback, ret=" << ret;
            avcodec_free_context(&m_codecCtx);
            m_codecCtx = nullptr;
            return false;
        }
        qDebug() << "H264Decoder: using fallback AV_CODEC_ID_H264 decoder";
    }

    // 分配帧和包容器
    m_frame = av_frame_alloc();
    m_pkt = av_packet_alloc();
    if (!m_frame || !m_pkt) {
        qCritical() << "H264Decoder: alloc frame/pkt failed";
        freeResources();
        return false;
    }

    m_initialized = true;
    qDebug() << "H264Decoder: init success";
    return true;
}

// 解码 H.264 数据
// 流程: 包装 QByteArray 为 AVPacket -> avcodec_send_packet -> avcodec_receive_frame
// 注意: 返回的 AVFrame 引用计数 +1，调用方使用后必须 av_frame_unref
AVFrame* H264Decoder::decode(const QByteArray &h264Data)
{
    if (!m_initialized && !init()) {
        return nullptr;
    }
    if (h264Data.isEmpty()) {
        return nullptr;
    }

    // 使用 av_new_packet + memcpy 替代 av_packet_from_data
    // 原Bug: av_packet_from_data 不复制数据，而是接管传入指针的所有权，
    //        av_packet_unref 时会通过 av_free() 释放 QByteArray 的内部缓冲区。
    //        之后 QByteArray 析构时再次 free 同一块内存 → double-free → 堆损坏 → 崩溃。
    // 修复: 使用 av_new_packet 分配 FFmpeg 管理的缓冲区，再 memcpy 数据，
    //        这样 av_packet_unref 只释放 FFmpeg 自己的缓冲区，不影响 QByteArray。
    int ret = av_new_packet(m_pkt, h264Data.size());
    if (ret < 0) {
        qWarning() << "H264Decoder: av_new_packet failed, ret=" << ret
                   << "size=" << h264Data.size();
        return nullptr;
    }
    memcpy(m_pkt->data, h264Data.constData(), h264Data.size());

    // 发送包到解码器
    ret = avcodec_send_packet(m_codecCtx, m_pkt);
    // av_packet_unref 现在安全释放 FFmpeg 分配的缓冲区，不会影响 QByteArray
    av_packet_unref(m_pkt);
    if (ret < 0) {
        qWarning() << "H264Decoder: avcodec_send_packet failed," << ret;
        return nullptr;
    }

    // 接收解码后的帧
    //  修复只取第一帧的问题，改为循环接收所有帧
    // 原实现: 仅调用一次 avcodec_receive_frame，如果packet包含多帧会丢失
    // 新实现: 循环调用 avcodec_receive_frame 直到返回 EAGAIN 或 EOF
    // 注意: H.264 通常一包一帧，但某些编码器(如带B帧的high profile)可能产生多帧
    //       HD级别使用high profile，必须正确处理多帧情况
    // 原代码(保留为注释):
    // ret = avcodec_receive_frame(m_codecCtx, m_frame);
    // if (ret < 0) {
    //     return nullptr;
    // }
    // AVFrame *sharedFrame = av_frame_clone(m_frame);
    // av_frame_unref(m_frame);
    // return sharedFrame;

    AVFrame *sharedFrame = nullptr;
    while (true) {
        ret = avcodec_receive_frame(m_codecCtx, m_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            // 需要更多数据或解码器已刷新
            break;
        }
        if (ret < 0) {
            qWarning() << "H264Decoder: avcodec_receive_frame failed," << ret;
            break;
        }

        // 复制帧引用（引用计数 +1）
        // 调用方负责 av_frame_unref 释放
        //  保留最后一帧作为返回值（通常是最新解码的帧）
        if (sharedFrame) {
            // 如果之前已有帧，释放旧帧引用
            av_frame_free(&sharedFrame);
        }
        sharedFrame = av_frame_clone(m_frame);
        av_frame_unref(m_frame);  // 释放本次接收的引用
    }

    // 移除 Q_EMIT SIG_decoded(sharedFrame)
    // 原Bug: decode() 同时返回帧指针并发射 SIG_decoded 信号。
    //        调用方(slot_dealVideoFrameH264Rq)获取返回值后会 av_frame_free(&frame)。
    //        如果 SIG_decoded 被 connect 到 slot_h264FrameDecoded，
    //        该槽函数也会 av_frame_free(&frame)，导致同一帧被 free 两次。
    //        即使当前未 connect，这也是一个定时炸弹。
    // 修复: 移除信号发射，decode() 仅通过返回值传递帧。
    // 原代码: if (sharedFrame) { Q_EMIT SIG_decoded(sharedFrame); }
    return sharedFrame;
}

// 释放资源
void H264Decoder::close()
{
    freeResources();
}

// 内部资源释放
void H264Decoder::freeResources()
{
    if (m_frame) {
        av_frame_free(&m_frame);
    }
    if (m_pkt) {
        av_packet_free(&m_pkt);
    }
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
    }
    m_initialized = false;
}
