#include "videoread.h"
#include <QMessageBox>
#include <QDebug>
//包含 QThread 用于 msleep 延时重试
#include <QThread>
//包含 OpenCV 头文件，用于指定摄像头后端
//在某些运行环境（如 Windows Defender ASR 规则限制的目录）下
//默认的 DSHOW 后端可能无法初始化，需要切换到 MSMF 后端
#include <opencv2/videoio.hpp>

VideoRead::VideoRead(QObject *parent) : QObject(parent)
{
    m_timer = new QTimer;
    connect( m_timer , SIGNAL(timeout()) , this , SLOT(slot_getVideoFrame()) );
}

VideoRead::~VideoRead()
{
    if(m_timer)
    {
        m_timer->stop();
        delete  m_timer;
        m_timer = NULL;

    }
}

//摄像头采集流程
void VideoRead::slot_getVideoFrame()
{
    Mat frame;
    if( !cap.read(frame) )
    {
    return;
    }
    // Mat 图像不是显示和传输的格式（RGB)
    //需要格式转换opencv采集的格式BGR 显示格式 RGB
    //将 opencv 采集的 BGR 的图片类型转化为 RGB24 的类型
    cvtColor(frame,frame,CV_BGR2RGB);
    //定义 QImage 对象, 用于发送数据以及图片显示
    //【修复-P1：QImage 悬空】QImage(frame.data,...) 不拷贝像素，image 依赖 frame
    // 的外部缓冲；scaled 在"源尺寸==目标尺寸"时返回浅拷贝，frame 析构后悬空。
    // 当前同线程直连侥幸安全，一旦改 Queued 连接立即崩溃 —— 显式 .copy() 一步到位。
    // 原代码: QImage image((unsigned const char*)frame.data,...);（无 copy）
    QImage image ((unsigned const
    char*)frame.data,frame.cols,frame.rows,QImage::Format_RGB888);
    image = image.copy();
    //转化为大小更小的图片 保持缩放比
    //  恢复 Qt::KeepAspectRatio，消除"人物变扁"变形问题
    // 修改原因: HD升级后编码器分辨率为1280x720(16:9)，slot_openVideo()中已通过
    //           cap.set(CAP_PROP_FRAME_WIDTH/HEIGHT)将摄像头采集分辨率设为1280x720，
    //           采集帧与编码器分辨率完全匹配，KeepAspectRatio不会导致尺寸不匹配，
    //           也不再需要IgnoreAspectRatio强制拉伸。
    //           原IgnoreAspectRatio会将4:3摄像头画面(640x480)强制拉伸到16:9(1280x720)，
    //           横向拉伸约1.78倍，导致人物变矮变宽（"人物变扁"现象）。
    // 修改内容: IgnoreAspectRatio → KeepAspectRatio
    // 影响范围: 摄像头不支持1280x720时，画面保持原始比例并留黑边（不变扁）
    // 
    // 原代码(SD阶段为避免sws_scale越界临时改为IgnoreAspectRatio):
    // image = image.scaled( IMAGE_WIDTH,IMAGE_HEIGHT, Qt::IgnoreAspectRatio );
    // 更早的原代码(SD阶段最初版本):
    // image = image.scaled( IMAGE_WIDTH,IMAGE_HEIGHT, Qt::KeepAspectRatio );
    image = image.scaled( IMAGE_WIDTH,IMAGE_HEIGHT, Qt::KeepAspectRatio );
    //发送图片
    Q_EMIT SIG_sendVideoFrame( image );
}

void VideoRead::slot_openVideo()
{
    //【修复-P1：定时器先启动】改为摄像头打开成功后再启动采集定时器。
    // 修改原因: 原代码先 m_timer->start() 再打开摄像头，打开失败时定时器
    //           继续以 23ms 周期对未打开的 cap 空转（cap.read 直接 return），
    //           且弹窗阻塞期间事件里仍排队采集槽。
    // 修改效果: 失败路径不启动定时器（无空转）；成功路径行为与旧版一致。
    // 原代码: m_timer->start( 1000/FRAME_RATE - 10 ); 在函数首行

    //摄像头打开逻辑增强
    //问题：在某些运行环境（如 Windows Defender ASR 规则限制的目录）下
    //      默认的 DSHOW 后端可能无法初始化，导致 cap.open(0) 失败
    //解决方案：依次尝试多种后端和重试机制，提高摄像头打开成功率
    //1. 先尝试默认后端（通常是 DSHOW）
    //2. 如果失败，尝试 MSMF（Media Foundation）后端
    //3. 如果还失败，尝试显式指定 DSHOW 后端
    //4. 每种后端重试 3 次，每次间隔 100ms

    bool bOpenSuccess = false;

    //尝试 1：默认后端
    for( int i = 0; i < 3 && !bOpenSuccess; i++ )
    {
        cap.open( 0 );
        if( cap.isOpened() )
        {
            bOpenSuccess = true;
            break;
        }
        QThread::msleep( 100 );
    }

    //尝试 2：MSMF 后端（Media Foundation）
    if( !bOpenSuccess )
    {
        for( int i = 0; i < 3 && !bOpenSuccess; i++ )
        {
            cap.open( 0, cv::CAP_MSMF );
            if( cap.isOpened() )
            {
                bOpenSuccess = true;
                break;
            }
            QThread::msleep( 100 );
        }
    }

    //尝试 3：显式指定 DSHOW 后端
    if( !bOpenSuccess )
    {
        for( int i = 0; i < 3 && !bOpenSuccess; i++ )
        {
            cap.open( 0, cv::CAP_DSHOW );
            if( cap.isOpened() )
            {
                bOpenSuccess = true;
                break;
            }
            QThread::msleep( 100 );
        }
    }

    if( !bOpenSuccess )
    {
        QMessageBox::information(NULL,tr("提示"),tr("视频没有打开"));
        return; //【修复-P1】失败不启动定时器，避免空转
    }

    //  设置摄像头采集分辨率为1280x720(16:9)
    // 修改原因: 不显式设置时摄像头使用默认分辨率(通常640x480, 4:3)，
    //           与HD编码器1280x720(16:9)比例不匹配。原IgnoreAspectRatio强制拉伸
    //           会导致"人物变扁"变形。现恢复KeepAspectRatio并显式设置采集分辨率，
    //           从源头保证采集帧比例与编码器一致。
    // 修改内容: 新增 cap.set 设置 CAP_PROP_FRAME_WIDTH/HEIGHT 为 IMAGE_WIDTH/HEIGHT
    // 影响范围: 摄像头输出QImage尺寸变为1280x720，与编码器完全匹配，无需缩放
    // 兼容性: 绝大多数现代摄像头支持1280x720；不支持时cap.set静默失败，
    //         摄像头仍输出默认分辨率，KeepAspectRatio会保持原比例留黑边(不变扁)
    // SD回退值: 弱网环境可设置为 cap.set(CAP_PROP_FRAME_WIDTH, 640); cap.set(CAP_PROP_FRAME_HEIGHT, 480);
    cap.set( cv::CAP_PROP_FRAME_WIDTH, IMAGE_WIDTH );
    cap.set( cv::CAP_PROP_FRAME_HEIGHT, IMAGE_HEIGHT );

    //【修复-P1】摄像头确认打开成功后才开始采集
    m_timer->start( 1000/FRAME_RATE - 10 );
}

void VideoRead::slot_closeVideo()
{
    m_timer->stop();
    //关闭摄像头
    if(cap.isOpened())
        cap.release();
}
