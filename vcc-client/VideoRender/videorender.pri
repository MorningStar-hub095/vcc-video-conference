#/*=== 变更日志 (TECH_INTEGRATION) ===
# * 日期: 2026-07-20
# * 变更人: AI 实施助手
# * 模块: VideoRender
# * 变更类型: 新增
# * 描述: 创建 VideoRender 模块 pri 文件
# *       整合 H264Encoder/H264Decoder/YuvGLWidget 三个组件
# * ==================================*/

# TECH_INTEGRATION: [VideoRender] 启用 Qt OpenGL 模块 (新增)
QT += opengl

# TECH_INTEGRATION: [VideoRender] FFmpeg 库引用
# [修复] 使用 MinGW 格式的 .dll.a 导入库替代 MSVC 格式的 .lib
FFMPEG_PATH = $$PWD/../ffmpeg-4.2.2
INCLUDEPATH += $$FFMPEG_PATH/include
LIBS += $$FFMPEG_PATH/lib/libavcodec.dll.a \
        $$FFMPEG_PATH/lib/libavutil.dll.a \
        $$FFMPEG_PATH/lib/libswscale.dll.a

# TECH_INTEGRATION: [VideoRender] 头文件列表 (新增)
HEADERS += \
    $$PWD/h264encoder.h \
    $$PWD/h264decoder.h \
    $$PWD/yuvglwidget.h

# TECH_INTEGRATION: [VideoRender] 源文件列表 (新增)
SOURCES += \
    $$PWD/h264encoder.cpp \
    $$PWD/h264decoder.cpp \
    $$PWD/yuvglwidget.cpp
