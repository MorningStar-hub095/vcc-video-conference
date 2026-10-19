#ifndef VIDEOREAD_H
#define VIDEOREAD_H

#include <QObject>
#include <QImage>
#include <QTimer>
#include "common.h"

//图片的宽高
//  分辨率从640x480(VGA)提升到1280x720(HD)
// 原值(SD): IMAGE_WIDTH=640, IMAGE_HEIGHT=480
// 更早的原值: IMAGE_WIDTH=320, IMAGE_HEIGHT=240 (QVGA)
// 修改原因: 达到HD级别目标，像素数从307,200提升到921,600(×3)
// 影响范围: 摄像头采集输出QImage尺寸、H264编码输入尺寸、网络带宽增加约2.5倍
// SD回退值: 弱网环境下可回退到640×480
#define IMAGE_WIDTH (1280)
#define IMAGE_HEIGHT (720)
// 
// #define IMAGE_WIDTH (640)
// #define IMAGE_HEIGHT (480)

class VideoRead : public QObject
{
    Q_OBJECT
public:
    explicit VideoRead(QObject *parent = nullptr);
    ~VideoRead();

signals:
    void SIG_sendVideoFrame(QImage img);
public slots:
    void slot_getVideoFrame();
    void slot_openVideo();
    void slot_closeVideo();
private:
    QTimer * m_timer;

    //opencv获取图片的对象
    cv::VideoCapture cap;

};

#endif // VIDEOREAD_H
