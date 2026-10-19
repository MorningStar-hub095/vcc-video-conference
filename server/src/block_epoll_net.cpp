#include "block_epoll_net.h"


bool Block_Epoll_Net::InitNet(int port, void (*recv_callback)(int, char *, int))
{
    m_recv_callback = recv_callback;
    InitThreadPool();

    int flags = 1;
    int ret = 0;
    m_listenEv = new myevent_s(this);

    //creat a tcp socket
    m_listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if ( m_listenfd  == -1 ){
        return false;
    }

    //set REUSERADDR
    ret = setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, (char *)&flags, sizeof(flags));
    if ( ret == -1 ){
        return false;
    }

    //监听套接字m_listenfd 采用 LT 非阻塞模式
    //set NONBLOCK
    setNonBlockFd( m_listenfd );

    struct sockaddr_in local_addr;
    bzero( &local_addr , sizeof(sockaddr_in) );
    //set address
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(port);
    local_addr.sin_addr.s_addr = INADDR_ANY;

    //bind addr
    ret = bind(m_listenfd, (struct sockaddr *)&local_addr, sizeof(struct sockaddr_in));
    if( ret == -1 ) {
        close(m_listenfd);
        return false;
    }

    if (listen(m_listenfd, 128) == -1 ){
        close(m_listenfd);
        return false;
    }

    //create epoll
    m_epoll_fd = epoll_create( MAX_EVENTS );

    m_listenEv->eventset(m_listenfd ,m_epoll_fd );
    //将监听套接字 添加到epoll中 , 模式LT 非阻塞
    m_listenEv->eventadd( EPOLLIN);


    return true;
}

bool Block_Epoll_Net::InitThreadPool()
{

    m_threadpool = new thread_pool;

    //创建拥有10个线程的线程池 最大线程数200 环形队列最大值50000
    if( (m_threadpool->Pool_create(200,10,50000)) == false )
    {
        exit(-1);
    }

    return true;
}

void Block_Epoll_Net::EventLoop()
{
    int  i = 0;
    while (1) {

        /* 等待事件发生 */
        int nfd = epoll_wait( m_epoll_fd, events, MAX_EVENTS+1, 1000);
        if (nfd < 0) {
//            break;
            continue;
        }
        for (i = 0; i < nfd; i++) {
            struct myevent_s *ev = (struct myevent_s *)events[i].data.ptr;
            int fd = ev->fd;
            if ( (events[i].events & EPOLLIN) ) {
                if( fd == m_listenfd )
                    accept_event();
                else
                    recv_event( ev );
            }
            if ((events[i].events & EPOLLOUT) ) {
                epollout_event( ev );
            }
        }
    }
}

void Block_Epoll_Net::accept_event()
{
    struct sockaddr_in caddr;
    socklen_t len = sizeof(caddr);
    int clientfd ;
    if ((clientfd = accept(m_listenfd, (struct sockaddr *)&caddr, &len)) == -1) {
        if (errno != EAGAIN && errno != EINTR) {
            /* 暂时不做出错处理 */
        }
        return;
    }
//    //客户端接收使用阻塞模式 更简单 适合入门
//    //设置非阻塞
//    setNonBlockFd( clientfd );

    //设置接收缓冲区大小
    setRecvBufSize( clientfd );
    //设置发送缓冲区大小
    // [Bug修复] 修复 accept_event 中的复制粘贴Bug
    // 修改原因: 原代码注释写的是"设置发送缓冲区大小"，但实际调用的却是 setRecvBufSize，
    //           导致客户端 socket 的发送缓冲区从未被正确设置（使用系统默认值）。
    //           在高分辨率视频帧转发场景下，系统默认发送缓冲区可能不足，导致 send() 阻塞。
    // 修改内容: setRecvBufSize → setSendBufSize
    // 影响范围: 所有新接入的客户端连接，发送缓冲区将被正确设置为 128KB
    // 原代码: setRecvBufSize( clientfd );
    setSendBufSize( clientfd );
    //设置 无延迟
    setNoDelay( clientfd );

    myevent_s * clientEv = new myevent_s(this);
    clientEv->eventset( clientfd , m_epoll_fd );
    // 使用EPOLLONESHOT epoll监听不会重复检测 避免多线程并发 使得同一个套接字接收是排队的
    clientEv->eventadd(  EPOLLIN/*|EPOLLET*/|EPOLLONESHOT );

    //m_mapSockfdToEvent[clientfd] = clientEv;
    m_mapSockfdToEvent.insert(clientfd , clientEv);

    fflush(stdout);
    return;
}




void Block_Epoll_Net::recv_event( myevent_s *ev)
{
    //接收事件采用线程池 多线程处理, 由于使用EPOLLONESHOT , 避免同一套机字的线程并发问题
     m_threadpool->Producer_add(  recv_task , (void*) ev );
}

void* Block_Epoll_Net::recv_task(void* arg)
{
    myevent_s * ev = (myevent_s*)arg;
    //利用全局指针 方便操作
    Block_Epoll_Net * pthis = ev->pNet;

    //接收和处理分离
    int nRelReadNum = 0;
    int nPackSize = 0;
    char *pSzBuf = NULL;
    // [回滚] 恢复原始的 do{...return 0;}while(0) 单包处理模式
    // 回滚原因: 上次修改将 do{return 0;}while(0) 改为 while(true){...break;} 是错误的。
    //           原代码使用 EPOLLONESHOT 模式，设计意图是每个线程只处理1个数据包：
    //             1. recv_task 读取1个包 → eventadd 重新注册事件 → return 0（线程退出）
    //             2. epoll 重新触发 → 由另一个线程处理下一个包
    //           这种设计保证同一个 fd 同一时刻只有1个线程在读取，避免并发竞争。
    //           改为 while(true) 后，线程A 在循环中调用 eventadd 重新注册事件后继续循环，
    //           epoll 会立即分派线程B 处理同一个 ev，导致：
    //             - 两个线程并发读取同一个 fd
    //             - 两个线程并发操作同一个 ev 指针
    //             - 线程A break 后 delete ev，线程B 仍在使用 → double free
    //           错误现象: "double free or corruption" + "event add failed [fd=0]"
    // 正确做法: 保持原代码不变。如果视频功能不可用，根因在别处（非 recv_task）。
    // 影响范围: 恢复原始行为，每个线程处理1个包后退出，由 epoll 重新分派。
    do
    {
        // [诊断日志] recv_task入口
        fflush(stdout);
        nRelReadNum = read(ev->fd,&nPackSize,sizeof(nPackSize) );
        if(nRelReadNum <= 0)
            break;

        // [诊断日志] 记录接收到的包大小和fd
        fflush(stdout);

        pSzBuf = new char[nPackSize];
        int nOffSet = 0;
        nRelReadNum = 0;
        //接收包的数据
        while(nPackSize)
        {
            nRelReadNum = recv(ev->fd,pSzBuf+nOffSet,nPackSize,0);
            if(nRelReadNum <= 0)
                break;

            nOffSet += nRelReadNum;
            nPackSize -= nRelReadNum;
        }
        // [诊断日志] 记录包类型（前4字节）
        int recvType = *(int*)pSzBuf;
        fflush(stdout);

        //接收和处理分离 跑线程池里其他线程处理 , 避免处理影响接收
        DataBuffer * buffer = new DataBuffer(ev->pNet , ev->fd , pSzBuf , nOffSet );
        pthis->m_threadpool->Producer_add(  Buffer_Deal , (void*) buffer );

        // 这次接收完 要重新注册事件  此时 EPOLL MODE -> EPOLLIN|EPOLLONESHOT 没有修改, 使用重复值
        ev->eventadd(  ev->events );

        return 0;

    }while(0);

// 借助 do..while 在最后统一处理错误  避免使用goto语句
    // [诊断日志] 记录连接断开事件
    fflush(stdout);
    ev->eventdel();
    close(ev->fd);
    //回收event结构
    //pthis->m_mapSockfdToEvent.erase( ev->fd );
    pthis->m_mapSockfdToEvent.erase( ev->fd );
    delete ev;
    return NULL;

}

void * Block_Epoll_Net::Buffer_Deal( void * arg )
{
    DataBuffer * buffer = (DataBuffer *)arg;
    if( !buffer ) return NULL;

    buffer->pNet->m_recv_callback(buffer->sockfd,buffer->buf,buffer->nlen);

 //   printf("pszbuf = %p \n",buffer->buf);
    if(buffer->buf != NULL)
    {
        delete [] buffer->buf;
        buffer->buf = NULL;
    }
    delete buffer;
    return 0;
}

void Block_Epoll_Net::epollout_event( myevent_s *ev )
{
    // epoll LT模式 阻塞模式 发送阻塞 , 不用监听EPOLLOUT事件
}



int Block_Epoll_Net::SendData(int fd, char *szbuf, int nlen)
{
    // 发送不可分割 避免多线程并发
    // 先包大小, 再数据包 , 一次放入缓冲区

    /*
     +--------------+------------------+---------------------+
     |<-- 4bytes -->|<-- 4bytes协议头 ->|<--  协议其他内容    ->|
     +--------------+------------------+---------------------+
     |<- packsize ->|<------------ 数据包 struct ------------>|
     * */

    int nPackSize = nlen + 4;
    vector<char> vecbuf( nPackSize , 0);
    //vecbuf.resize( nPackSize );

    char* buf = &* vecbuf.begin();
    char* tmp = buf;
    *(int*)tmp = nlen;//按四个字节int写入
    tmp += sizeof(int );
    memcpy( tmp , szbuf , nlen );

    // [Bug修复] send() 添加 MSG_NOSIGNAL 标志
    // 修改原因: 当对端连接已断开时，send() 会产生 SIGPIPE 信号，
    //           默认行为是终止整个服务端进程。
    //           添加 MSG_NOSIGNAL 后，send() 返回 -1 并设置 errno=EPIPE，
    //           不会产生信号，进程继续运行。
    // 修改内容: 第4个参数 0 → MSG_NOSIGNAL
    // 影响范围: 所有通过 SendData 发送的数据包
    // 注意事项: 与 main.cpp 中 signal(SIGPIPE, SIG_IGN) 形成双重保护，
    //           即使其中一处被误改，另一处仍能防止崩溃
    // 原代码: int res = send( fd,(const char *)buf, nPackSize ,0);

    // [修复] 添加循环发送，处理 send() 部分写入问题
    // 原Bug: send() 只调用一次，不检查返回值是否等于 nPackSize。
    //        当内核发送缓冲区满时，send() 可能只发送部分数据（返回值 < nPackSize），
    //        或返回 -1（errno=EAGAIN/EWOULDBLOCK）。
    //        数据静默丢失后，接收端收到不完整的包，后续所有包的长度解析全部错位，
    //        导致连接永久损坏，视频数据无法正确传输。
    // 修复: 循环发送直到所有数据发送完毕或发生真正错误。
    int totalSent = 0;
    while( totalSent < nPackSize )
    {
        int res = send( fd, buf + totalSent, nPackSize - totalSent, MSG_NOSIGNAL);
        if( res > 0 )
        {
            totalSent += res;
        }
        else if( res == -1 )
        {
            if( errno == EAGAIN || errno == EWOULDBLOCK )
            {
                // 发送缓冲区满，短暂等待后重试
                usleep(1000);  // 1ms
                continue;
            }
            // 真正的错误（连接断开等）
            return res;
        }
        else  // res == 0，连接已关闭
        {
            return res;
        }
    }

    return totalSent;
}

void Block_Epoll_Net::setNonBlockFd(int fd)
{
    int flags = 0;
    flags = fcntl(fd, F_GETFL, 0);
    int ret = fcntl(fd, F_SETFL, flags|O_NONBLOCK);
}

void Block_Epoll_Net::setRecvBufSize( int fd)
{
    //接收缓冲区
    int nRecvBuf = 256*1024;//设置为 256 K
    setsockopt(fd,SOL_SOCKET,SO_RCVBUF,(const char*)&nRecvBuf,sizeof(int));
}

void Block_Epoll_Net::setSendBufSize( int fd)
{
    //发送缓冲区
    int nSendBuf=128*1024;//设置为 128 K
    setsockopt(fd,SOL_SOCKET,SO_SNDBUF,(const char*)&nSendBuf,sizeof(int));
}

#include<netinet/tcp.h>
void Block_Epoll_Net::setNoDelay( int fd)
{
    //nodelay
    int value = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY ,(char *)&value, sizeof(int));
}
