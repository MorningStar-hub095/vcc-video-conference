#include <TCPKernel.h>
// [Bug修复] 新增 signal.h 头文件，用于忽略 SIGPIPE 信号
// 修改原因: 服务端 SendData() 中 send() 在对端连接断开时会产生 SIGPIPE 信号，
//           Linux 默认行为是终止进程，导致服务端崩溃。
//           添加 signal(SIGPIPE, SIG_IGN) 后，send() 返回 -1 并设置 errno=EPIPE，
//           不再终止进程，保证服务端稳定运行。
// 修改内容: 新增 #include <signal.h>
#include <signal.h>

int main(int argc,char *argv[])
{
    // [Bug修复] 忽略 SIGPIPE 信号
    // 修改原因: 当服务端向已断开连接的客户端 send() 数据时，内核会产生 SIGPIPE 信号，
    //           默认处理方式是终止整个进程。视频功能中服务端需要向房间内所有成员转发
    //           视频帧，如果某个成员连接已断开但服务端尚未检测到，send() 就会触发
    //           SIGPIPE 导致服务端崩溃，所有客户端断连。
    // 修改内容: 注册 SIGPIPE 信号处理为 SIG_IGN（忽略）
    // 影响范围: 全进程，send() 失败时返回 -1 而非终止进程
    // 注意事项: 此修复与 block_epoll_net.cpp 中 send() 的 MSG_NOSIGNAL 标志形成双重保护
    signal(SIGPIPE, SIG_IGN);

    int port = 8000;
    if( argc >= 2 )
    {
        port = atoi(argv[1]);
    }
    TcpKernel * pKernel =  TcpKernel::GetInstance();

    //开启服务 给定端口, 可以使用输入的port
    pKernel->Open( port );
    // 事件循环 : 循环监听事件
    pKernel->EventLoop();

    pKernel->Close();

    return 0;
}
