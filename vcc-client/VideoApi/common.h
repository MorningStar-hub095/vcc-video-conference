#ifndef COMMON_H
#define COMMON_H


#include "highgui/highgui.hpp"
#include "imgproc/imgproc.hpp"
#include "core/core.hpp"
#include "opencv2\imgproc\types_c.h"

using namespace cv;
//进一步提高全局帧率
//原值：15fps，画面有延迟感
//第一版修改：25fps
//第二版修改：30fps，达到主流视频会议软件（如 Zoom、Teams）的流畅度标准
//30fps 时摄像头单帧 320x240 JPEG 约 15KB，每秒约 450KB，局域网和宽带均可承受
#define FRAME_RATE (30)



#endif // COMMON_H
