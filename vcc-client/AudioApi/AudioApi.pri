
# [废弃] Qt Multimedia 模块依赖（已废弃，保留注释）
# 修改原因：音频采集已从 QAudioInput 迁移到 SDL，不再需要 Qt Multimedia
# 修改内容：注释掉 QT += multimedia
# QT += multimedia

HEADERS += \
    $$PWD/WebRtcMoudle/analog_agc.h \
    $$PWD/WebRtcMoudle/complex_fft_tables.h \
    $$PWD/WebRtcMoudle/cpu_features_wrapper.h \
    $$PWD/WebRtcMoudle/defines.h \
    $$PWD/WebRtcMoudle/digital_agc.h \
    $$PWD/WebRtcMoudle/fft4g.h \
    $$PWD/WebRtcMoudle/gain_control.h \
    $$PWD/WebRtcMoudle/noise_suppression.h \
    $$PWD/WebRtcMoudle/noise_suppression_x.h \
    $$PWD/WebRtcMoudle/ns_core.h \
    $$PWD/WebRtcMoudle/nsx_core.h \
    $$PWD/WebRtcMoudle/nsx_defines.h \
    $$PWD/WebRtcMoudle/real_fft.h \
    $$PWD/WebRtcMoudle/resample_by_2_internal.h \
    $$PWD/WebRtcMoudle/ring_buffer.h \
    $$PWD/WebRtcMoudle/signal_processing_library.h \
    $$PWD/WebRtcMoudle/spl_inl.h \
    $$PWD/WebRtcMoudle/typedefs.h \
    $$PWD/WebRtcMoudle/windows_private.h \
    $$PWD/WebRtc_Vad/signal_processing_library.h \
    $$PWD/WebRtc_Vad/spl_inl.h \
    $$PWD/WebRtc_Vad/typedefs.h \
    $$PWD/WebRtc_Vad/vad_core.h \
    $$PWD/WebRtc_Vad/vad_filterbank.h \
    $$PWD/WebRtc_Vad/vad_gmm.h \
    $$PWD/WebRtc_Vad/vad_sp.h \
    $$PWD/WebRtc_Vad/vad_unittest.h \
    $$PWD/WebRtc_Vad/webrtc_vad.h \
    $$PWD/World.h \
    $$PWD/audioread.h \
    $$PWD/audiowrite.h

SOURCES += \
    $$PWD/WebRtcMoudle/analog_agc.c \
    $$PWD/WebRtcMoudle/complex_bit_reverse.c \
    $$PWD/WebRtcMoudle/complex_fft.c \
    $$PWD/WebRtcMoudle/copy_set_operations.c \
    $$PWD/WebRtcMoudle/cross_correlation.c \
    $$PWD/WebRtcMoudle/digital_agc.c \
    $$PWD/WebRtcMoudle/division_operations.c \
    $$PWD/WebRtcMoudle/dot_product_with_scale.c \
    $$PWD/WebRtcMoudle/downsample_fast.c \
    $$PWD/WebRtcMoudle/energy.c \
    $$PWD/WebRtcMoudle/fft4g.c \
    $$PWD/WebRtcMoudle/get_scaling_square.c \
    $$PWD/WebRtcMoudle/min_max_operations.c \
    $$PWD/WebRtcMoudle/noise_suppression.c \
    $$PWD/WebRtcMoudle/noise_suppression_x.c \
    $$PWD/WebRtcMoudle/ns_core.c \
    $$PWD/WebRtcMoudle/nsx_core.c \
    $$PWD/WebRtcMoudle/nsx_core_c.c \
    $$PWD/WebRtcMoudle/nsx_core_neon_offsets.c \
    $$PWD/WebRtcMoudle/real_fft.c \
    $$PWD/WebRtcMoudle/resample.c \
    $$PWD/WebRtcMoudle/resample_48khz.c \
    $$PWD/WebRtcMoudle/resample_by_2.c \
    $$PWD/WebRtcMoudle/resample_by_2_internal.c \
    $$PWD/WebRtcMoudle/resample_by_2_mips.c \
    $$PWD/WebRtcMoudle/resample_fractional.c \
    $$PWD/WebRtcMoudle/ring_buffer.c \
    $$PWD/WebRtcMoudle/spl_init.c \
    $$PWD/WebRtcMoudle/spl_sqrt.c \
    $$PWD/WebRtcMoudle/spl_sqrt_floor.c \
    $$PWD/WebRtcMoudle/splitting_filter.c \
    $$PWD/WebRtcMoudle/vector_scaling_operations.c \
    $$PWD/WebRtc_Vad/vad_core.c \
    $$PWD/WebRtc_Vad/vad_filterbank.c \
    $$PWD/WebRtc_Vad/vad_gmm.c \
    $$PWD/WebRtc_Vad/vad_sp.c \
    $$PWD/WebRtc_Vad/webrtc_vad.c \
    $$PWD/audioread.cpp \
    $$PWD/audiowrite.cpp

#【Bug修复】链接库配置
#原代码第二行缺少 LIBS += 前缀，导致 signalProcess.lib 可能未被正确链接
#修复：每行都添加 LIBS += 前缀，确保两个库都被正确链接

# [废弃] Speex 库链接（已废弃，保留注释）
# 修改原因：编码器从 Speex 迁移到 Opus
# 修改内容：注释掉 Speex 库链接，替换为 Opus 库链接
# LIBS += $$PWD/speex/lib/libspeex.lib

# [新增] Opus 库链接
# Opus 是 Speex 的官方继任者（IETF RFC 6716），48kHz 原生优化
# 库文件来源：AudioDemo2/AudioDemo2/OpusApi/opus-1.4/lib/libopus.a
LIBS += $$PWD/opus/lib/libopus.a -lssp

# [新增] Opus 头文件路径
INCLUDEPATH += $$PWD/opus/include

# [保留] WebRTC VAD 静音检测库（与 Opus DTX 双重保障）
LIBS += $$PWD/WebRtc_Vad/signalProcess.lib
