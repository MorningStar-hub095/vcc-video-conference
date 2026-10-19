# * 引入 VideoRender 模块，集成 H.264 编解码与 OpenGL 渲染
# * ==================================*/

QT       += core gui

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

include(./netapi/netapi.pri)
INCLUDEPATH += ./netapi

include(./md5/md5.pri)
INCLUDEPATH += ./md5

include(./AudioApi/AudioApi.pri)
INCLUDEPATH += ./AudioApi

include(./VideoApi/videoapi.pri)
INCLUDEPATH += ./VideoApi

# TECH_INTEGRATION: [vcc-client.pro] 新增 VideoRender 模块引入 (新增)
# 包含 H264Encoder/H264Decoder/YuvGLWidget 三个组件
include(./VideoRender/videorender.pri)
INCLUDEPATH += ./VideoRender

# 原有代码: include(./opengl/opengl.pri)
# 原有代码: INCLUDEPATH += ./opengl/
# 原有代码 - 保留兼容性，待废弃
include(./opengl/opengl.pri)
INCLUDEPATH += ./opengl/
# end 原有代码

INCLUDEPATH += $$PWD/ffmpeg-4.2.2/include\
$$PWD/SDL2-2.0.10/include
LIBS += $$PWD/ffmpeg-4.2.2/lib/libavcodec.dll.a\
$$PWD/ffmpeg-4.2.2/lib/libavdevice.dll.a\
$$PWD/ffmpeg-4.2.2/lib/libavfilter.dll.a\
$$PWD/ffmpeg-4.2.2/lib/libavformat.dll.a\
$$PWD/ffmpeg-4.2.2/lib/libavutil.dll.a\
$$PWD/ffmpeg-4.2.2/lib/libpostproc.dll.a\
$$PWD/ffmpeg-4.2.2/lib/libswresample.dll.a\
$$PWD/ffmpeg-4.2.2/lib/libswscale.dll.a\
$$PWD/SDL2-2.0.10/lib/x86/SDL2.lib

SOURCES += \
    ckernel.cpp \
    logindialog.cpp \
    main.cpp \
    roomdialog.cpp \
    usershow.cpp \
    vccdialog.cpp

HEADERS += \
    ckernel.h \
    logindialog.h \
    roomdialog.h \
    usershow.h \
    vccdialog.h

FORMS += \
    logindialog.ui \
    roomdialog.ui \
    usershow.ui \
    vccdialog.ui



# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

RESOURCES += \
    resource.qrc
