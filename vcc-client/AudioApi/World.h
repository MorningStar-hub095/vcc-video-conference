#ifndef WORLD_H
#define WORLD_H

// 原代码使用 Qt Multimedia 进行音频采集，现已迁移到 SDL 采集
// 修改原因：统一采集/播放技术栈为 SDL，消除 QAudioInput 定时器轮询的精度问题
// 修改内容：注释掉 QAudioInput/QAudioOutput/QIODevice/QTimer/QMessageBox

/* 原代码（Qt Multimedia 依赖，已废弃）：
#include <QAudioInput>
#include <QAudioOutput>
#include <QIODevice>
#include <QTimer>
#include <QMessageBox>
*/

#include <QDebug>

// 引入SDL头文件，用于音频采集和播放
// 采集：SDL_OpenAudioDevice(iscapture=1) 回调驱动
// 播放：SDL_OpenAudioDevice(iscapture=0) 回调驱动
// 参考AudioDemo2的SDL音频实现
extern "C" {
#include <SDL.h>
#include <SDL_audio.h>
}
// 取消SDL的main宏重定义，避免与Qt的main函数冲突
#undef main

// 引入mutex和deque，用于SDL音频回调缓冲区
#include <mutex>
#include <deque>

// webrtc_vad 静音检测（与 Opus DTX 双重保障）
#define USE_VAD   (1)
#include "WebRtc_Vad/webrtc_vad.h"

enum ENUM_PLAY_STATE {stopped , playing , pausing};

// 原代码使用 Speex 窄带编码（8kHz, 640→76字节, 88%压缩率）
// 修改原因：迁移到 Opus 编码（48kHz, ~60-120字节/帧），Opus 是 Speex 的官方继任者
// 修改内容：Speex 头文件和宏定义替换为 Opus 头文件
// 预期效果：音质从窄带电话级提升到宽带级，压缩率更高，抗丢包能力更强
/* 原代码（Speex 编码器，已废弃）：
#include "speex/include/speex.h"
#define SPEEX_QUALITY    (8)
#define USE_SPEEX   (1)
*/

// Opus 编解码器头文件
// Opus 是 IETF RFC 6716 标准，替代 Speex 的下一代语音编解码器
// 特性：48kHz 原生优化、内置 DTX（不连续传输）、内置 PLC（丢包隐藏）
extern "C" {
#include <opus/opus.h>
}

#endif // WORLD_H
