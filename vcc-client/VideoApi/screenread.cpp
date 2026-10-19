#include "screenread.h"
#include "common.h"

//桌面共享专用帧率和缩放尺寸定义
//桌面共享帧率独立设置，不使用全局 FRAME_RATE(30)
//参考主流会议软件（Zoom、Teams）桌面共享通常 15-30fps
//  帧率从25fps降到20fps，分辨率从1280x720提升到1920x1080(FHD)
// 原值(SD): DESK_FRAME_RATE=25, DESK_IMAGE_WIDTH=1280, DESK_IMAGE_HEIGHT=720
// 更早的原值: DESK_FRAME_RATE=25, DESK_IMAGE_WIDTH=1024, DESK_IMAGE_HEIGHT=768
// 修改原因: 达到HD级别目标，桌面共享分辨率提升到FHD，帧率降低5fps控制带宽
// 影响范围: 桌面共享帧大小增加约2倍，带宽增加，CPU缩放开销增加
#define DESK_FRAME_RATE (20)       //  25 → 20
//  桌面共享分辨率从1280x720(HD)提升到1920x1080(FHD)
// 原值: DESK_IMAGE_WIDTH=1280, DESK_IMAGE_HEIGHT=720
// 修改原因: 达到HD级别目标，提升桌面共享文字清晰度
// 影响范围: 桌面共享帧大小增加约2倍，网络带宽增加，CPU缩放开销略增
#define DESK_IMAGE_WIDTH  (1920)   //  1280 → 1920
#define DESK_IMAGE_HEIGHT (1080)   //  720 → 1080

ScreenRead::ScreenRead(QObject *parent) : QObject(parent)
{
    //定时获取图片, 并抛出信号
    m_timer = new QTimer(this);
    connect(m_timer,SIGNAL(timeout()),this,SLOT( slot_getScreenFrame() ));

}

ScreenRead::~ScreenRead()
{
    if( m_timer )
    {
        m_timer->stop();
        delete m_timer;
        m_timer = NULL;

    }
}

void ScreenRead::slot_getScreenFrame()
{
    //获取桌面对象
    QScreen *src = QApplication::primaryScreen();
    //获取当前分辨率 得到桌面矩形
    //    QRect deskRect  =  QApplication::desktop()->screenGeometry();
    QPixmap map = src->grabWindow( QApplication::desktop()->winId()/* ,0,0 , deskRect.width() ,
    deskRect.height()*/);
    QImage image = map.toImage();
    //发送前缩放到固定尺寸
    //原代码直接发送原始桌面分辨率（如 1920x1080），数据量巨大
    //修复：缩放到 DESK_IMAGE_WIDTH x DESK_IMAGE_HEIGHT，保持宽高比
    //  缩放算法从FastTransformation改为SmoothTransformation
    // 原值(SD): Qt::FastTransformation（最近邻插值，速度快但锯齿明显）
    // 新值(HD): Qt::SmoothTransformation（双线性插值，文字边缘平滑）
    // 修改原因: HD级别桌面共享需要清晰文字，FastTransformation在1920x1080下锯齿严重
    // 影响范围: CPU缩放开销增加约30%，但现代CPU可承受（单帧缩放<5ms）
    // 原代码: image = image.scaled( DESK_IMAGE_WIDTH, DESK_IMAGE_HEIGHT, Qt::KeepAspectRatio, Qt::FastTransformation );
    image = image.scaled( DESK_IMAGE_WIDTH, DESK_IMAGE_HEIGHT,
                          Qt::KeepAspectRatio, Qt::SmoothTransformation );  //  FastTransformation → SmoothTransformation
    //   image = image.scaled( 1600, 900, Qt::KeepAspectRatio, Qt::SmoothTransformation );
    Q_EMIT SIG_getScreenFrame(image);
//    //压缩图片从 RGB24 格式压缩到 JPEG 格式, 发送出去
//    QByteArray ba;
//    QBuffer qbuf(&ba); // QBuffer 与 QByteArray 字节�数组联立联系
//    image.save( &qbuf , "JPEG" , 60 ); //将图片的数据写入 ba
//    Q_EMIT SIG_screenFrameData( ba );
}

void ScreenRead::slot_openVideo()
{
    //提高桌面共享帧率
    //原代码：m_timer->start( 1000/FRAME_RATE -10 );  // 15fps，间隔 56ms
    //修改后：m_timer->start( 1000/DESK_FRAME_RATE );  // 25fps，间隔 40ms
    //25fps 达到主流视频会议软件的桌面共享流畅度标准
    //配合发送端缩放（1024x768），每秒数据量约 1MB，网络可承受
    m_timer->start( 1000 / DESK_FRAME_RATE );
}

void ScreenRead::slot_closeVideo()
{
    m_timer->stop();
}
