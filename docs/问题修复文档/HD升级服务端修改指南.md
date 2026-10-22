# WeChat 视频会议系统 - HD升级服务端代码修改指南

> **文档性质：** 服务端代码手动修改操作指南
> **适用范围：** `server\` 下的服务端代码
> **编写日期：** 2026-07-31
> **前置条件：** 客户端 HD 升级已完成（1280×720 摄像头 + 1920×1080 桌面共享）
> **代码规范：** 所有修改严禁删除原有代码，原值通过注释保留，修改处标注 `[HD升级]`

---

## 目录

- [一、修改总览](#一修改总览)
- [二、修改项1：同步 H264 分片参数（packdef.h）](#二修改项1同步-h264-分片参数packdefh)
- [三、修改项2：升级 TCP 接收缓冲区（block_epoll_net.cpp）](#三修改项2升级-tcp-接收缓冲区block_epoll_netcpp)
- [四、修改项3：升级 TCP 发送缓冲区（block_epoll_net.cpp）](#四修改项3升级-tcp-发送缓冲区block_epoll_netcpp)
- [五、编译验证](#五编译验证)
- [六、运行时验证](#六运行时验证)
- [七、回退方案](#七回退方案)

---

## 一、修改总览

### 1.1 背景

客户端已完成 HD 级别升级（摄像头 640×480 → 1280×720，桌面共享 1280×720 → 1920×1080），H264 分片大小从 4096 字节提升到 8192 字节，TCP 缓冲区从 256KB 提升到 1MB。

服务端代码作为视频帧的**转发中枢**，虽然不参与 H264 编解码和分片重组，但需要保证：
1. **协议参数一致性**：`packdef.h` 中的 H264 分片常量应与客户端同步，避免代码维护混乱
2. **缓冲区充足性**：TCP 接收/发送缓冲区需容纳 HD 大帧（I 帧最大 150KB），避免发送阻塞导致卡顿

### 1.2 修改文件清单

| 序号 | 文件路径 | 修改内容 | SD原值 | HD新值 | 优先级 |
|------|----------|----------|--------|--------|--------|
| 1 | `server\include\packdef.h` | H264分片大小 | 4096 | 8192 | 中 |
| 2 | `server\include\packdef.h` | 分片重组超时 | 100ms | 150ms | 中 |
| 3 | `server\PlayerHallServer\include\packdef.h` | H264分片大小 | 4096 | 8192 | 中 |
| 4 | `server\PlayerHallServer\include\packdef.h` | 分片重组超时 | 100ms | 150ms | 中 |
| 5 | `server\PlayerHallServer\src\block_epoll_net.cpp` | TCP接收缓冲区 | 256KB | 1MB | **高** |
| 6 | `server\PlayerHallServer\src\block_epoll_net.cpp` | TCP发送缓冲区 | 128KB | 1MB | **高** |

> **说明：** 服务端代码存在两份副本（外层 `include\` 和内层 `PlayerHallServer\include\`），两份都需修改以保持一致。`block_epoll_net.cpp` 仅内层副本有 `src\` 实现，外层是头文件副本。

### 1.3 修改原因详解

#### 为什么服务端需要同步 H264 分片参数？

| 维度 | 说明 |
|------|------|
| **功能影响** | 服务端仅转发 H264 分片包，不使用 `H264_FRAGMENT_SIZE` 做分片/重组逻辑，功能上无影响 |
| **维护性影响** | 客户端已升级到 8192，服务端仍为 4096，参数不一致会造成代码审查和维护混乱 |
| **协议一致性** | 两端 `packdef.h` 应保持完全一致，便于后续维护和排查问题 |

#### 为什么服务端需要升级 TCP 缓冲区？（关键）

| 维度 | 说明 |
|------|------|
| **发送缓冲区** | 原 128KB，HD 的 I 帧最大 150KB，单帧就超过缓冲区。`send()` 是阻塞模式，会分批发送，导致发送线程阻塞时间延长，影响其他连接的转发实时性 |
| **接收缓冲区** | 原 256KB，HD 单包（8KB分片+包头）虽然小于 256KB，但高帧率下多个包堆积可能溢出，升级到 1MB 留足余量 |
| **与客户端对齐** | 客户端 TCP 缓冲区已升级到 1MB，服务端应保持一致 |

---

## 二、修改项1：同步 H264 分片参数（packdef.h）

### 2.1 涉及文件（两份副本）

**副本A（外层）：** `server\include\packdef.h`
**副本B（内层）：** `server\PlayerHallServer\include\packdef.h`

> 两份文件内容相同，修改方式完全一致。以下以副本A为例，副本B按相同步骤操作。

### 2.2 修改位置

文件第 95-99 行（两份副本行号相同）

### 2.3 修改前代码

```cpp
// TECH_INTEGRATION: [packdef] H.264 分片大小常量 (新增)
//单片4KB，加上包头约4.1KB，远低于1MB安全阈值
#define H264_FRAGMENT_SIZE       (4096)
//H.264分片重组超时（毫秒）
#define H264_REASSEMBLE_TIMEOUT  (100)
```

### 2.4 修改后代码

```cpp
// TECH_INTEGRATION: [packdef] H.264 分片大小常量 (新增)
// [HD升级] 分片大小从4KB提升到8KB，重组超时从100ms提升到150ms
// 原值(SD): H264_FRAGMENT_SIZE=4096, H264_REASSEMBLE_TIMEOUT=100
// 修改原因: 1280x720 H264 I帧可达50-150KB，4KB分片需要13-38片，
//           增大到8KB后仅需7-19片，减少分片数量和包头开销
//           重组超时从100ms增至150ms，适应更大帧的传输延迟
// 影响范围: 服务端仅转发分片包，不参与重组逻辑，功能无影响，仅保持与客户端参数一致
// [HD升级保留原值注释]
// 原代码(SD):
// #define H264_FRAGMENT_SIZE       (4096)
// #define H264_REASSEMBLE_TIMEOUT  (100)
#define H264_FRAGMENT_SIZE       (8192)   // [HD升级] 4096 → 8192
//H.264分片重组超时（毫秒）
#define H264_REASSEMBLE_TIMEOUT  (150)    // [HD升级] 100 → 150
```

### 2.5 分步操作指南

**步骤1：打开副本A文件**

使用文本编辑器（VSCode / Notepad++ / Vim）打开文件：
```
server\include\packdef.h
```

**步骤2：定位修改位置**

搜索关键字 `H264_FRAGMENT_SIZE`，定位到第 95-99 行附近。

**步骤3：替换代码**

将第 2.3 节的"修改前代码"替换为第 2.4 节的"修改后代码"。

**步骤4：保存文件**

保存副本A。

**步骤5：对副本B重复操作**

打开文件：
```
server\PlayerHallServer\include\packdef.h
```
执行相同的替换操作并保存。

### 2.6 验证方法

**方法1：搜索验证**

在服务端代码目录下搜索 `H264_FRAGMENT_SIZE`，确认两份副本的值均为 `8192`：
```bash
grep -rn "H264_FRAGMENT_SIZE" "服务端代码\"
```
预期输出：两份 packdef.h 中 `H264_FRAGMENT_SIZE (8192)`，注释中保留 `4096` 原值。

**方法2：对比验证**

对比两份 packdef.h 的修改处是否完全一致。

---

## 三、修改项2：升级 TCP 接收缓冲区（block_epoll_net.cpp）

### 3.1 涉及文件

`server\PlayerHallServer\src\block_epoll_net.cpp`

### 3.2 修改位置

文件第 266-271 行，`setRecvBufSize` 函数

### 3.3 修改前代码

```cpp
void Block_Epoll_Net::setRecvBufSize( int fd)
{
    //接收缓冲区
    int nRecvBuf = 256*1024;//设置为 256 K
    setsockopt(fd,SOL_SOCKET,SO_RCVBUF,(const char*)&nRecvBuf,sizeof(int));
}
```

### 3.4 修改后代码

```cpp
void Block_Epoll_Net::setRecvBufSize( int fd)
{
    //接收缓冲区
    // [HD升级] 接收缓冲区从256KB提升到1MB
    // 原值(SD): 256*1024 (256KB)
    // 修改原因: HD升级后H264分片增大到8KB，高帧率下多个分片包堆积可能超过256KB，
    //           升级到1MB与客户端保持一致，留足缓冲余量
    // 影响范围: 每个客户端连接增加768KB内存占用，100连接共增加约75MB（可接受）
    // [HD升级保留原值注释]
    // 原代码(SD): int nRecvBuf = 256*1024;//设置为 256 K
    int nRecvBuf = 1024*1024;// [HD升级] 256KB → 1MB
    setsockopt(fd,SOL_SOCKET,SO_RCVBUF,(const char*)&nRecvBuf,sizeof(int));
}
```

### 3.5 分步操作指南

**步骤1：打开文件**

```
server\PlayerHallServer\src\block_epoll_net.cpp
```

**步骤2：定位修改位置**

搜索关键字 `setRecvBufSize`，定位到第 266 行。

**步骤3：替换代码**

将第 3.3 节的"修改前代码"替换为第 3.4 节的"修改后代码"。

**步骤4：保存文件**

---

## 四、修改项3：升级 TCP 发送缓冲区（block_epoll_net.cpp）

### 4.1 涉及文件

`server\PlayerHallServer\src\block_epoll_net.cpp`

### 4.2 修改位置

文件第 273-278 行，`setSendBufSize` 函数

### 4.3 修改前代码

```cpp
void Block_Epoll_Net::setSendBufSize( int fd)
{
    //发送缓冲区
    int nSendBuf=128*1024;//设置为 128 K
    setsockopt(fd,SOL_SOCKET,SO_SNDBUF,(const char*)&nSendBuf,sizeof(int));
}
```

### 4.4 修改后代码

```cpp
void Block_Epoll_Net::setSendBufSize( int fd)
{
    //发送缓冲区
    // [HD升级] 发送缓冲区从128KB提升到1MB
    // 原值(SD): 128*1024 (128KB)
    // 修改原因: HD升级后H264 I帧最大可达150KB，超过原128KB发送缓冲区，
    //           send()阻塞模式下会分批发送导致线程阻塞时间延长，
    //           影响其他连接的转发实时性。升级到1MB确保单帧完整放入缓冲区。
    // 影响范围: 每个客户端连接增加896KB内存占用，100连接共增加约87MB（可接受）
    // [HD升级保留原值注释]
    // 原代码(SD): int nSendBuf=128*1024;//设置为 128 K
    int nSendBuf=1024*1024;// [HD升级] 128KB → 1MB
    setsockopt(fd,SOL_SOCKET,SO_SNDBUF,(const char*)&nSendBuf,sizeof(int));
}
```

### 4.5 分步操作指南

**步骤1：打开文件**（与修改项2同一文件）

**步骤2：定位修改位置**

搜索关键字 `setSendBufSize`，定位到第 273 行。

**步骤3：替换代码**

将第 4.3 节的"修改前代码"替换为第 4.4 节的"修改后代码"。

**步骤4：保存文件**

---

## 五、编译验证

### 5.1 编译环境

服务端代码基于 Linux epoll + pthread，需在 Linux 环境（或 WSL）下使用 g++ 编译。

### 5.2 编译命令

```bash
cd /mnt/d/vcc-video-conference/server/PlayerHallServer
qmake PlayerHallServer.pro
make
```

### 5.3 预期结果

- 编译无错误
- 生成 `PlayerHallServer` 可执行文件
- 若有 MySQL 头文件缺失，需安装 `libmysqlclient-dev`

### 5.4 语法检查（无法完整编译时）

若环境不具备完整编译条件，可对修改的文件单独做语法检查：

```bash
g++ -fsyntax-only -I include src/block_epoll_net.cpp
```

预期：无错误输出（可能有预存在的警告）。

---

## 六、运行时验证

### 6.1 启动服务端

```bash
./PlayerHallServer
```

预期输出：
```
MySql Connect Success...
EventLoop:server running
```

### 6.2 缓冲区验证

服务端运行后，客户端连接时观察服务端日志。新连接输出：
```
new connect [192.168.x.x:xxxxx][time:xxxx]
```

可通过以下命令查看 socket 缓冲区大小（需知道客户端 fd）：
```bash
# 假设服务端进程 PID 为 1234
ss -tnp | grep 1234
# 或
cat /proc/1234/net/sockstat
```

### 6.3 HD 视频转发验证

1. 启动两个客户端，登录并加入同一房间
2. 客户端A开启摄像头（1280×720）
3. 客户端B应能看到A的视频画面
4. 观察服务端日志，应有 `VideoFrameH264Rq` 转发记录
5. 视频应无卡顿、无马赛克（I 帧完整转发）

### 6.4 大帧压力测试

1. 4人同时加入同一房间
2. 4人同时开启摄像头
3. 观察服务端 CPU 和内存占用
4. 观察视频流畅度（应 ≥25fps）
5. 持续 5 分钟，无崩溃、无内存泄漏

---

## 七、回退方案

### 7.1 回退条件

- 服务端内存不足（100连接需额外 ~160MB）
- 编译错误无法解决
- 运行时出现新问题

### 7.2 回退步骤

将上述 4 处修改的值改回原值：

| 文件 | 参数 | HD值 | 回退值 |
|------|------|------|--------|
| packdef.h（两份） | H264_FRAGMENT_SIZE | 8192 | 4096 |
| packdef.h（两份） | H264_REASSEMBLE_TIMEOUT | 150 | 100 |
| block_epoll_net.cpp | nRecvBuf | 1024*1024 | 256*1024 |
| block_epoll_net.cpp | nSendBuf | 1024*1024 | 128*1024 |

### 7.3 回退验证

重新编译并启动服务端，确认服务端正常运行。

---

## 附录：修改检查清单

修改完成后，请逐项确认：

- [ ] 副本A `packdef.h` 的 `H264_FRAGMENT_SIZE` 已改为 8192
- [ ] 副本A `packdef.h` 的 `H264_REASSEMBLE_TIMEOUT` 已改为 150
- [ ] 副本B `packdef.h` 的 `H264_FRAGMENT_SIZE` 已改为 8192
- [ ] 副本B `packdef.h` 的 `H264_REASSEMBLE_TIMEOUT` 已改为 150
- [ ] `block_epoll_net.cpp` 的 `nRecvBuf` 已改为 1024*1024
- [ ] `block_epoll_net.cpp` 的 `nSendBuf` 已改为 1024*1024
- [ ] 所有修改处均保留原值注释
- [ ] 所有修改处均标注 `[HD升级]`
- [ ] 两份 `packdef.h` 修改内容完全一致
- [ ] 服务端编译通过
- [ ] 服务端启动正常
- [ ] HD 视频转发正常

---

> **文档状态：** 完成，待手动执行
> **修改文件数：** 3个文件（packdef.h ×2 + block_epoll_net.cpp ×1），共6处参数修改
> **预计操作时间：** 15-20分钟
> **风险等级：** 低（参数调整，不改变业务逻辑）
