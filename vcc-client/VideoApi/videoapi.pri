HEADERS += \
    $$PWD/common.h \
    $$PWD/myfacedetect.h \
    $$PWD/screenread.h \
    $$PWD/threadworker.h \
    $$PWD/videoread.h \
    $$PWD/AVSyncManager.h

SOURCES += \
    $$PWD/myfacedetect.cpp \
    $$PWD/screenread.cpp \
    $$PWD/threadworker.cpp \
    $$PWD/videoread.cpp \
    $$PWD/AVSyncManager.cpp

# ============================================================
# OpenCV 路径配置
# ============================================================
# 原实现把 OpenCV 的安装路径写死为作者本机的绝对路径，
# 他人克隆后必然编译失败（找不到 lib / 头文件）。
# 现改为可通过环境变量或 qmake 参数覆盖的变量形式，三种取用方式任选其一：
#
#   1) 环境变量（推荐，无需改文件）：
#        export OPENCV_DIR=D:/opencv-release/opencv-release
#   2) qmake 命令行参数：
#        qmake vcc-client.pro OPENCV_DIR=D:/path/to/opencv
#   3) 什么都不设：回退到本文件同级的默认相对位置 opencv/，
#      即把 OpenCV 解压/软链到 vcc-client/VideoApi/opencv/ 下。
#
# 注意：OpenCV 需为 MinGW 32bit 版本（opencv 的 mingw 预编译包），
#      且库名里的 420 对应 OpenCV 4.2.0 的 ABI 号，换版本需同步改。
# ============================================================
OPENCV_DIR = $$(OPENCV_DIR)

# 【修复-P1（发布项）：OPENCV_DIR 空值回退未落地】
# 修改原因: 本文件头部注释承诺"三种取用方式任选其一：不设环境变量时回退到
#           同级 opencv/ 目录"，但原代码只有 $$OPENCV_DIR = $$(OPENCV_DIR) 一行，
#           未实现 isEmpty 回退。环境变量未设置时 INCLUDEPATH 退化为 "/include"，
#           LIBS 指向 "/lib/*.dll.a"，qmake 必然失败
#           （2026-10-21 全量构建实测复现），README 第 3 种获取方式名存实亡。
# 修改效果: OPENCV_DIR 为空时回退到 $$PWD/opencv/（即 VideoApi/opencv/），
#           与注释及 README 描述一致；设置了环境变量的行为完全不变。
# 原代码: OPENCV_DIR = $$(OPENCV_DIR)（其后直接使用，无回退）
isEmpty( OPENCV_DIR ) {
    OPENCV_DIR = $$PWD/opencv
}

INCLUDEPATH += \
    $$OPENCV_DIR/include/opencv2 \
    $$OPENCV_DIR/include

LIBS += \
    $$OPENCV_DIR/lib/libopencv_calib3d420.dll.a \
    $$OPENCV_DIR/lib/libopencv_core420.dll.a \
    $$OPENCV_DIR/lib/libopencv_features2d420.dll.a \
    $$OPENCV_DIR/lib/libopencv_flann420.dll.a \
    $$OPENCV_DIR/lib/libopencv_highgui420.dll.a \
    $$OPENCV_DIR/lib/libopencv_imgproc420.dll.a \
    $$OPENCV_DIR/lib/libopencv_ml420.dll.a \
    $$OPENCV_DIR/lib/libopencv_objdetect420.dll.a \
    $$OPENCV_DIR/lib/libopencv_video420.dll.a \
    $$OPENCV_DIR/lib/libopencv_videoio420.dll.a
