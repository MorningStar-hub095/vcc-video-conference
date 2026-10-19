#ifndef H264ENCODER_H
#define H264ENCODER_H

#include <QObject>
#include <QImage>
#include <QByteArray>
#include <QString>

// 引入 FFmpeg C 头文件
extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libswscale/swscale.h"
}

// H.264 编码器封装类
// 职责: 接收 QImage(RGB888) -> sws_scale 转 YUV420P -> libx264 编码 -> 返回 H.264 NAL
// 设计要点:
//   1. 单例可复用，避免频繁初始化开销
//   2. 支持运行时请求关键帧（用于 PLI 机制）
//   3. 编码参数 preset=ultrafast + tune=zerolatency 满足实时性
//   4. 禁用 B 帧降低延迟
class H264Encoder : public QObject
{
    Q_OBJECT
public:
    explicit H264Encoder(QObject *parent = nullptr);
    ~H264Encoder() override;

    // 初始化编码器
    // width/height: 图像尺寸（建议 320x240 或 640x480）
    // fps: 帧率（30）
    // bitrate: 目标码率（200000 = 200kbps）
    bool init(int width, int height, int fps = 30, int bitrate = 200000);

    // 编码一帧
    // img: 输入 QImage（Format_RGB888 或 Format_ARGB32）
    // 返回: H.264 NAL 单元数据（可能含 SPS/PPS+I 帧或 P 帧）
    QByteArray encode(const QImage &img);

    // 请求下一帧强制为 I 帧（用于新成员加入房间场景）
    void requestKeyFrame();

    // 释放资源
    void close();

    // 检查是否已初始化
    bool isReady() const { return m_codecCtx != nullptr; }

    //  查询编码器当前配置的分辨率，用于判断是否需要重新初始化
    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }

signals:
    // 编码完成信号（可选异步使用）
    void SIG_encoded(const QByteArray &h264Data, bool isKeyFrame);

private:
    // FFmpeg 上下文
    AVCodecContext  *m_codecCtx;        // 编码器上下文
    AVFrame         *m_frame;            // 输入帧容器（YUV420P）
    AVPacket        *m_pkt;              // 输出包容器
    SwsContext      *m_swsCtx;           // RGB->YUV420P 转换器（当前未使用，保留接口）

    // 状态
    int              m_width;
    int              m_height;
    int              m_fps;
    int64_t          m_frameIndex;       // 帧计数（PTS 来源）
    bool             m_requestKeyFrame;  // 下一帧强制 I 帧标志
    QString          m_encoderName;      // 实际使用的编码器名称（用于诊断）

    // 强制I帧最小间隔限流时间戳
    // 修改原因: 原requestKeyFrame()无条件置位标志，每个PLI都强制一个I帧。
    //           弱网下多个接收端同时请求或重复请求会触发I帧风暴，与接收端
    //           PLI限流(PLI_THROTTLE_INTERVAL_MS)配合形成双端保险。
    // 修改内容: 新增上次强制I帧的毫秒时间戳，H264_KEYFRAME_MIN_INTERVAL_MS(300ms)
    //           窗口内的重复请求丢弃
    // 影响范围: 仅限制强制I帧频率，正常GOP周期性I帧(gop_size=fps*2)不受影响
    qint64           m_lastForcedKeyFrameMs;   // 上次强制I帧的毫秒时间戳(0=从未)

    // 内部辅助
    bool initCodec(int width, int height, int fps, int bitrate);
    void freeResources();
};

#endif // H264ENCODER_H
