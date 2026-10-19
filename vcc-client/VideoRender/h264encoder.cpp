#include "h264encoder.h"
#include <QDebug>
#include <cstring>  // for memset
// 强制I帧限流所需的时间戳获取
#include <QDateTime>
// PLI限流常量定义（PLI_THROTTLE_INTERVAL_MS/H264_KEYFRAME_MIN_INTERVAL_MS）
// 工程INCLUDEPATH已含netapi/net目录（与ckernel.cpp的包含方式一致）
#include "packdef.h"

// 构造函数初始化成员
H264Encoder::H264Encoder(QObject *parent): QObject(parent) , m_codecCtx(nullptr) , m_frame(nullptr) , m_pkt(nullptr) ,
m_swsCtx(nullptr) , m_width(0) , m_height(0) , m_fps(30) , m_frameIndex(0) , m_requestKeyFrame(false)
{
    // 强制I帧限流时间戳初始化
    // 修改原因: 新增成员m_lastForcedKeyFrameMs用于300ms最小间隔限流（见头文件
    //           m_lastForcedKeyFrameMs注释），需初始化为0表示"从未强制过I帧"
    // 修改内容: 构造函数体内初始化（不改动原初始化列表）
    m_lastForcedKeyFrameMs = 0;
}

// 析构函数确保资源释放
H264Encoder::~H264Encoder()
{
    close();
}

// 初始化编码器
// 参考: MediaPlayer/videoplayer.cpp 中 avcodec_find_decoder + avcodec_open2 流程
// 改进: 使用新版 API（avcodec_send_frame/avcodec_receive_packet）
bool H264Encoder::init(int width, int height, int fps, int bitrate)
{
    if (m_codecCtx) {
        qWarning() << "H264Encoder already initialized";
        return false;
    }

    m_width = width;
    m_height = height;
    m_fps = fps;

    // 编码器选择逻辑：逐个尝试编码器，每个都执行 avcodec_open2 验证可用性
    // 原Bug: 仅用 avcodec_find_encoder_by_name 查找第一个注册的编码器(如h264_nvenc)，
    //        在无NVIDIA GPU的系统上该编码器已注册但无法打开，导致直接返回false，
    //        不会继续尝试libx264软编码器，最终回退到JPEG。
    // 修复方案: 将"查找"和"打开"合并到同一个循环中，avcodec_open2失败则尝试下一个编码器。
    // 回退链: h264_nvenc -> h264_qsv -> libx264 -> AV_CODEC_ID_H264兜底
    const char *encoder_names[] = {"h264_nvenc", "h264_qsv", "libx264"};
    const AVCodec *codec = nullptr;
    bool codecOpened = false;

    for (size_t i = 0; i < sizeof(encoder_names)/sizeof(encoder_names[0]); ++i) {
        codec = avcodec_find_encoder_by_name(encoder_names[i]);
        if (!codec) {
            qDebug() << "H264Encoder: encoder" << encoder_names[i] << "not registered, skip";
            continue;
        }

        // 分配临时编码器上下文用于测试打开
        AVCodecContext *testCtx = avcodec_alloc_context3(codec);
        if (!testCtx) {
            qDebug() << "H264Encoder: alloc context failed for" << encoder_names[i] << ", skip";
            continue;
        }

        // 设置基本编码参数
        testCtx->width = width;
        testCtx->height = height;
        testCtx->time_base = AVRational{1, fps};
        testCtx->framerate = AVRational{fps, 1};
        testCtx->bit_rate = bitrate;
        testCtx->gop_size = fps * 2;       // 2 秒一个 I 帧
        testCtx->max_b_frames = 0;         // 禁用 B 帧（实时通信）
        testCtx->pix_fmt = AV_PIX_FMT_YUV420P;

        // 设置编码参数字典
        //  编码参数从SD配置提升到HD配置
        // 原值(SD): preset=veryfast, crf=23, profile=main
        // 新值(HD): preset=faster, crf=20, profile=high
        // 修改原因: HD级别要求更高压缩效率和画质，faster preset提供更好压缩比，
        //           crf=20提升画质(降低量化误差)，high profile启用CABAC等高级特性
        // 影响范围: 编码CPU负载增加约30%，I帧从15-40KB增至50-150KB，画质显著提升
        AVDictionary *testOpts = nullptr;
        av_dict_set(&testOpts, "preset", "faster", 0);   //  veryfast → faster
        av_dict_set(&testOpts, "tune", "zerolatency", 0);
        av_dict_set(&testOpts, "crf", "20", 0);           //  23 → 20
        av_dict_set(&testOpts, "profile", "high", 0);     //  main → high

        // 尝试打开编码器
        int ret = avcodec_open2(testCtx, codec, &testOpts);
        av_dict_free(&testOpts);

        if (ret < 0) {
            // 记录编码器打开失败的原因，帮助排查硬件编码器不可用问题
            qDebug() << "H264Encoder: avcodec_open2 failed for" << encoder_names[i]
                     << ", ret=" << ret << ", trying next encoder";
            avcodec_free_context(&testCtx);
            continue;
        }

        // 编码器打开成功，使用此编码器
        qDebug() << "H264Encoder: using encoder" << encoder_names[i]
                 << "(verified by avcodec_open2)";
        m_codecCtx = testCtx;
        m_encoderName = QString(encoder_names[i]);
        codecOpened = true;
        break;
    }

    if (!codecOpened) {
        // 所有命名编码器都失败了，尝试通过ID查找兜底
        qDebug() << "H264Encoder: all named encoders failed, trying AV_CODEC_ID_H264 fallback";
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) {
            qCritical() << "H264Encoder: codec H264 not found at all";
            return false;
        }

        m_codecCtx = avcodec_alloc_context3(codec);
        if (!m_codecCtx) {
            qCritical() << "H264Encoder: alloc context failed for fallback";
            return false;
        }

        m_codecCtx->width = width;
        m_codecCtx->height = height;
        m_codecCtx->time_base = AVRational{1, fps};
        m_codecCtx->framerate = AVRational{fps, 1};
        m_codecCtx->bit_rate = bitrate;
        m_codecCtx->gop_size = fps * 2;
        m_codecCtx->max_b_frames = 0;
        m_codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;

        AVDictionary *opts = nullptr;
        //  兜底编码器同步修改为HD参数
        // 原值(SD): preset=veryfast, crf=23, profile=main
        av_dict_set(&opts, "preset", "faster", 0);    //  veryfast → faster
        av_dict_set(&opts, "tune", "zerolatency", 0);
        av_dict_set(&opts, "crf", "20", 0);            //  23 → 20
        av_dict_set(&opts, "profile", "high", 0);      //  main → high

        int ret = avcodec_open2(m_codecCtx, codec, &opts);
        av_dict_free(&opts);
        if (ret < 0) {
            qCritical() << "H264Encoder: avcodec_open2 failed for fallback, ret =" << ret;
            avcodec_free_context(&m_codecCtx);
            m_codecCtx = nullptr;
            return false;
        }
        qDebug() << "H264Encoder: using fallback AV_CODEC_ID_H264 encoder";
        m_encoderName = "AV_CODEC_ID_H264(fallback)";
    }

    // 分配帧和包容器
    m_frame = av_frame_alloc();
    m_pkt = av_packet_alloc();
    if (!m_frame || !m_pkt) {
        qCritical() << "H264Encoder: alloc frame/pkt failed";
        freeResources();
        return false;
    }

    // 设置帧参数（格式/尺寸），缓冲区将通过 av_frame_get_buffer 分配
    m_frame->format = AV_PIX_FMT_YUV420P;
    m_frame->width = width;
    m_frame->height = height;

    // 创建 RGB->YUV420P 转换器 (SwsContext)
    // FFmpeg DLLs 是 MinGW 编译的(使用 msvcrt.dll)，配合 .dll.a 导入库，
    // sws_scale 可以安全使用，比手动转换更可靠(SIMM 优化、边界检查完善)
    m_swsCtx = sws_getContext(width, height, AV_PIX_FMT_RGB24,
                              width, height, AV_PIX_FMT_YUV420P,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_swsCtx) {
        qCritical() << "H264Encoder: sws_getContext failed";
        freeResources();
        return false;
    }

    // 使用标准的 av_frame_get_buffer 分配帧缓冲区
    int ret = av_frame_get_buffer(m_frame, 0);
    if (ret < 0) {
        qCritical() << "H264Encoder: av_frame_get_buffer failed, ret=" << ret;
        freeResources();
        return false;
    }

    // 验证缓冲区分配结果
    qDebug() << "H264Encoder: frame buffer allocated via av_frame_get_buffer, linesize:"
             << m_frame->linesize[0] << m_frame->linesize[1] << m_frame->linesize[2]
             << "data[0]:" << (void*)m_frame->data[0]
             << "data[1]:" << (void*)m_frame->data[1]
             << "data[2]:" << (void*)m_frame->data[2]
             << "buf[0]:" << (void*)m_frame->buf[0];
    if (!m_frame->data[0] || !m_frame->data[1] || !m_frame->data[2]) {
        qCritical() << "H264Encoder: frame data pointers are null after av_frame_get_buffer";
        freeResources();
        return false;
    }
    if (m_frame->linesize[0] <= 0 || m_frame->linesize[1] <= 0 || m_frame->linesize[2] <= 0) {
        qCritical() << "H264Encoder: invalid frame linesizes after av_frame_get_buffer";
        freeResources();
        return false;
    }

    qDebug() << "H264Encoder: init success," << width << "x" << height << "@" << fps << "fps"
             << "encoder:" << m_encoderName << "buffer:av_frame_get_buffer sws:sws_scale";
    return true;
}

// 编码一帧
// 流程: QImage(RGB888) -> sws_scale 转换 YUV420P -> avcodec_send_frame -> avcodec_receive_packet
// 使用标准 FFmpeg 帧管理: av_frame_get_buffer + av_frame_make_writable
// 确保 AVBufferRef 引用计数正确工作，防止编码器内部缓冲区生命周期问题
QByteArray H264Encoder::encode(const QImage &img)
{
    if (!m_codecCtx || !m_frame) {
        qWarning() << "H264Encoder::encode: encoder not ready";
        return QByteArray();
    }

    // 验证编码器上下文完整性
    if (m_codecCtx->width != m_width || m_codecCtx->height != m_height
        || m_codecCtx->pix_fmt != AV_PIX_FMT_YUV420P) {
        qWarning() << "H264Encoder::encode: codecCtx corrupted, w=" << m_codecCtx->width
                   << "h=" << m_codecCtx->height << "fmt=" << m_codecCtx->pix_fmt;
        return QByteArray();
    }

    // 使用 img.copy() 强制深拷贝，防止 use-after-free 崩溃
    QImage rgbImg;
    if (img.format() == QImage::Format_RGB888) {
        rgbImg = img.copy();
    } else {
        rgbImg = img.convertToFormat(QImage::Format_RGB888);
    }

    // 防御性尺寸校验（第一道防线为 videoread.cpp 的 KeepAspectRatio + 摄像头采集分辨率设置）
    //  此处保持 IgnoreAspectRatio 不变（防御性兜底，正常流程不会走到此分支）
    // 修改原因: 若摄像头不支持1280x720，videoread.cpp的KeepAspectRatio会返回960x720等
    //           非标准尺寸，此处必须用IgnoreAspectRatio强制拉伸到精确的m_width×m_height，
    //           否则sws_scale按m_width读取会越界崩溃。变形优先于崩溃。
    //           正常流程下(videoread.cpp已cap.set设置1280x720)不会进入此分支。
    // 影响范围: 仅在摄像头不支持HD分辨率时触发，画面会有轻微变形但不崩溃
    if (rgbImg.width() != m_width || rgbImg.height() != m_height) {
        rgbImg = rgbImg.scaled(m_width, m_height, Qt::IgnoreAspectRatio);
    }

    if (rgbImg.isNull() || !rgbImg.constBits()) {
        qWarning() << "H264Encoder::encode: rgbImg is null after conversion";
        return QByteArray();
    }

    // 调用 av_frame_make_writable 确保帧缓冲区可写
    int ret = av_frame_make_writable(m_frame);
    if (ret < 0) {
        qWarning() << "H264Encoder::encode: av_frame_make_writable failed," << ret;
        return QByteArray();
    }

    // 验证帧缓冲区有效性
    if (!m_frame->data[0] || !m_frame->data[1] || !m_frame->data[2]
        || m_frame->linesize[0] <= 0 || m_frame->linesize[1] <= 0 || m_frame->linesize[2] <= 0) {
        qWarning() << "H264Encoder::encode: frame buffer invalid";
        return QByteArray();
    }

    // RGB -> YUV420P 转换 (使用 FFmpeg sws_scale)
    // FFmpeg DLLs 是 MinGW 编译的，配合 .dll.a 导入库可以安全使用
    {
        const uint8_t *srcData[1] = { rgbImg.constBits() };
        int srcLinesize[1] = { rgbImg.bytesPerLine() };
        sws_scale(m_swsCtx, srcData, srcLinesize, 0, m_height,
                  m_frame->data, m_frame->linesize);
    }

    // 设置 PTS 和关键帧标志
    m_frame->pts = m_frameIndex++;
    m_frame->pict_type = m_requestKeyFrame ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    m_requestKeyFrame = false;

    // 发送帧到编码器并接收编码后的包
    QByteArray result;
    bool isKeyFrame = false;

    ret = avcodec_send_frame(m_codecCtx, m_frame);
    if (ret < 0) {
        qWarning() << "H264Encoder: avcodec_send_frame failed," << ret;
        return QByteArray();
    }

    // 接收编码后的包
    while (ret >= 0) {
        ret = avcodec_receive_packet(m_codecCtx, m_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            qWarning() << "H264Encoder: avcodec_receive_packet failed," << ret;
            break;
        }

        if (m_pkt->data && m_pkt->size > 0) {
            result.append(reinterpret_cast<const char*>(m_pkt->data), m_pkt->size);
            if (m_pkt->flags & AV_PKT_FLAG_KEY) {
                isKeyFrame = true;
            }
        }
        av_packet_unref(m_pkt);
    }

    return result;
}

// 请求下一帧为 I 帧
void H264Encoder::requestKeyFrame()
{
    // 强制I帧最小间隔限流（H264_KEYFRAME_MIN_INTERVAL_MS=300ms）
    // 修改原因: 原实现无条件置位m_requestKeyFrame，每个PLI请求都强制一个I帧。
    //           弱网下I帧(50-150KB)本身最易重组超时，触发更多PLI → 更多I帧尖峰
    //           → 挤占带宽拖垮P帧的正反馈风暴；且多个接收端几乎同时请求时
    //           会连续产出多个I帧浪费带宽。
    // 修改内容: 新增300ms最小间隔限流，窗口内的重复请求直接丢弃。
    //           窗口内被丢弃的请求无需补偿——若丢帧未恢复，接收端会在其
    //           500ms限流窗口（PLI_THROTTLE_INTERVAL_MS）后再次发送PLI。
    // 影响范围: 仅限制强制I帧频率，正常GOP周期性I帧(gop_size=fps*2)不受影响。
    // 原代码（保留为注释）:
    /*
    m_requestKeyFrame = true;
    */
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastForcedKeyFrameMs == 0 || (now - m_lastForcedKeyFrameMs) >= H264_KEYFRAME_MIN_INTERVAL_MS) {
        m_requestKeyFrame = true;
        m_lastForcedKeyFrameMs = now;
    }
}

// 释放资源
void H264Encoder::close()
{
    freeResources();
}

// 内部资源释放
// 使用标准 av_frame_get_buffer 后，av_frame_free 会通过 AVBufferRef 自动释放缓冲区
// 无需手动释放 m_yuvBuffer（已移除）
void H264Encoder::freeResources()
{
    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }
    if (m_frame) {
        av_frame_free(&m_frame);
    }
    if (m_pkt) {
        av_packet_free(&m_pkt);
    }
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
    }
    m_frameIndex = 0;
}
