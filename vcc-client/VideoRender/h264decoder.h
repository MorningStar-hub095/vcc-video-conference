#ifndef H264DECODER_H
#define H264DECODER_H

#include <QObject>
#include <QImage>
#include <QByteArray>

// 引入 FFmpeg C 头文件
extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libswscale/swscale.h"
}

// H.264 解码器封装类
// 职责: 接收 H.264 NAL -> FFmpeg 解码 -> 输出 AVFrame(YUV420P)
// 设计要点:
//   1. 每个房间成员独立一个解码器实例（多路并行）
//   2. 延迟初始化：首次 decode 时才创建上下文
//   3. 支持引用计数式 AVFrame 输出（零拷贝跨线程）
//   4. 自动硬件解码器探测，写入 config.ini 缓存
class H264Decoder : public QObject
{
    Q_OBJECT
public:
    explicit H264Decoder(QObject *parent = nullptr);
    ~H264Decoder() override;

    // 初始化解码器（延迟初始化，首次 decode 时调用也可）
    bool init();

    // 解码一个 H.264 NAL 包
    // h264Data: 输入 H.264 数据（含 SPS/PPS+I 帧 或 P 帧）
    // 返回: 解码后的 AVFrame（YUV420P），引用计数 +1，调用方需 av_frame_unref
    //       解码失败或需要更多数据时返回 nullptr
    AVFrame* decode(const QByteArray &h264Data);

    // 释放资源
    void close();

    // 检查是否已初始化
    bool isReady() const { return m_codecCtx != nullptr; }

signals:
    // 解码完成信号（可选异步使用）
    void SIG_decoded(AVFrame *frame);

private:
    // FFmpeg 上下文
    AVCodecContext  *m_codecCtx;
    AVFrame         *m_frame;
    AVPacket        *m_pkt;

    // 状态
    bool             m_initialized;

    // 内部辅助
    bool initCodec();
    void freeResources();
};

#endif // H264DECODER_H
