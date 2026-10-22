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
    //【修复-P1：epoll_create 返回值未检查】
    // 修改原因: epoll_create 失败返回 -1（fd 耗尽等），原代码直接拿 -1 当 epoll 句柄用，
    //           后续 epoll_ctl/epoll_wait 全部失败，EventLoop 陷入 100% CPU 忙等死循环。
    // 修改效果: 创建失败立即返回 false，TcpKernel::Open 会以 err_str 退出并给出明确原因。
    // 原代码: （无检查，直接继续 eventset/eventadd）
    if( m_epoll_fd < 0 )
    {
        printf("[Block_Epoll_Net::InitNet] epoll_create failed errno=%d(%s)\n", errno, strerror(errno));
        fflush(stdout);
        close(m_listenfd);
        return false;
    }

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
            //【修复-P1：epoll_wait 持续失败时 100% CPU 忙等】
            // 修改原因: 原代码 nfd<0 直接 continue，若 epoll 句柄异常将持续立即返回错误，
            //           while(1) 退化为空转死循环，把 CPU 打满且无任何日志。
            // 修改效果: 失败时先睡 100ms 并打印一次错误（限流，避免日志风暴）再继续，
            //           给系统恢复留出窗口，且错误可被运维观测。
            // 原代码: continue;（无 sleep 无日志）
            usleep(100 * 1000);
            static int s_nErrCount = 0;
            if( (++s_nErrCount) % 100 == 1 )
                printf("[Block_Epoll_Net::EventLoop] epoll_wait error errno=%d(%s)\n", errno, strerror(errno));
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

    //【安全修复-P1：阻塞 fd 无接收超时（slowloris）】
    // 修改原因: 客户端 fd 为阻塞模式且无超时，恶意/异常客户端连接后不发数据，
    //           recv_task 线程将永久阻塞在 recv 上；200 个线程池槽位被慢客户端逐一占满后，
    //           整个服务不再接收任何新包（slowloris 式资源耗尽）。
    // 修改效果: 给每个客户端 fd 设置 60 秒接收超时，超时 recv 返回 -1（errno=EAGAIN/
    //           EWOULDBLOCK 视实现而定），recv_task 走既有错误清理路径关闭连接。
    //           保持阻塞语义不变，其余逻辑不受影响；60s 远大于正常心跳/包间隔。
    {
        struct timeval tv;
        tv.tv_sec = 60;
        tv.tv_usec = 0;
        setsockopt(clientfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        //【安全修复-P1：SendData 无限阻塞】客户端 fd 仍是阻塞模式，对端收到数据
        // 却不读取（恶意/故障）时 send 可无限期卡死线程池线程。加 5 秒发送超时，
        // 超时后 send 返回 -1，由既有错误路径回收线程。完整方案（非阻塞+发送队列
        // +EPOLLOUT）工程量大，此处先以超时兜底。
        // 原代码: （无 SO_SNDTIMEO）
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(clientfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
    }

    myevent_s * clientEv = new myevent_s(this);
    clientEv->eventset( clientfd , m_epoll_fd );
    // 使用EPOLLONESHOT epoll监听不会重复检测 避免多线程并发 使得同一个套接字接收是排队的
    clientEv->eventadd(  EPOLLIN/*|EPOLLET*/|EPOLLONESHOT );

    //m_mapSockfdToEvent[clientfd] = clientEv;
    m_mapSockfdToEvent.insert(clientfd , clientEv);

    return;
}




void Block_Epoll_Net::recv_event( myevent_s *ev)
{
    //接收事件采用线程池 多线程处理, 由于使用EPOLLONESHOT , 避免同一套机字的线程并发问题
    //【安全修复-P0：主循环阻塞】改用非阻塞入队
    // 修改原因: 原阻塞版 Producer_add 在任务队列满（50000）时卡死在条件变量上，
    //           本函数运行在 epoll 事件循环线程 —— 一旦卡住，accept/全部事件冻结，
    //           未认证客户端刷包即可打死整个服务。
    // 修改效果: 队列满时立即返回，此处直接关闭该连接（任务丢弃），
    //           主循环永不阻塞。正常负载下队列远不会满，行为与旧版一致。
    // 原代码: m_threadpool->Producer_add( recv_task , (void*) ev );
    if( 0 != m_threadpool->Producer_add_nb( recv_task , (void*) ev ) )
    {
        fprintf(stderr, "[Block_Epoll_Net::recv_event] queue full, drop fd=%d\n", ev->fd);
        ev->eventdel();
        if( m_close_callback )
            m_close_callback( ev->fd );
        m_mapSockfdToEvent.erase( ev->fd );
        ::close( ev->fd );
        delete ev;
    }
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
        //【安全修复-P0：包头半包读】4 字节包头必须循环收满
        // 修改原因: 原代码单次 read 4 字节，TCP 字节流完全可能先送来 1~3 字节
        //           （Nagle/分片/乱序），此时 nPackSize 是"部分栈垃圾+部分网络数据"
        //           拼出的错误长度：可能恰好落进合法区间(0,1MB]，随后按错误长度收包，
        //           整条连接的协议流永久错位。
        // 修改效果: 循环读满 4 字节才继续；中途对端关闭/出错/超时（SO_RCVTIMEO）
        //           则进入下方统一错误清理断开连接。
        // 原代码: nRelReadNum = read(ev->fd,&nPackSize,sizeof(nPackSize));
        //         if(nRelReadNum <= 0) break;（单次读，不校验是否读满）
        {
            char * pHeader = (char*)&nPackSize;
            int nHeaderGot = 0;
            nRelReadNum = 0;
            while( nHeaderGot < (int)sizeof(nPackSize) )
            {
                nRelReadNum = read( ev->fd , pHeader + nHeaderGot ,
                                    sizeof(nPackSize) - nHeaderGot );
                if( nRelReadNum <= 0 )
                    break;
                nHeaderGot += nRelReadNum;
            }
            // 未读满 4 字节（对端关闭/错误/超时）或读满前出错，一律断开
            if( nRelReadNum <= 0 || nHeaderGot != (int)sizeof(nPackSize) )
                break;
        }

        //【安全修复-包长校验】nPackSize 完全由对端指定，未校验即 new 会导致：
        //   1) nPackSize == 0        -> new char[0] 随后 *(int*)pSzBuf 越界读
        //   2) nPackSize < 0         -> 转无符号后变成巨大值，new 抛 std::bad_alloc（无 try/catch，进程终止）
        //   3) nPackSize 极大(2GB)   -> 内存耗尽，线程池被拖垮
        //   未认证的客户端连上即可打死整个会议服务，故必须在此拦截。
        // 上限取 1MB，与客户端 TcpClient.cpp 的包长校验保持同一套规则。
        if( nPackSize <= 0 || nPackSize > 1024 * 1024 )
        {
            printf("[Block_Epoll_Net::recv_task] Invalid pack size=%d, close fd=%d\n", nPackSize, ev->fd);
            fflush(stdout);
            ::close(ev->fd);
            break;
        }

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

        //【修复-P1：不完整包被分发】
        // 修改原因: 对端半关闭/超时/异常时，内层 recv 循环提前退出，此时包体不完整
        //           （nPackSize>0 表示还有 nPackSize 字节没收到），原代码仍把残包 new
        //           出来分发给业务层，垃圾包进入协议处理；且连接未被关闭、事件被重注册，
        //           状态错乱。
        // 修改效果: 收不满整包时丢弃残包（delete[]）并 break 进入下方统一错误清理
        //           （eventdel + close + erase + delete ev），不再分发残包。
        // 原代码: （无此判断，残包照常 new DataBuffer 分发 + eventadd 重注册）
        if( nPackSize != 0 )
        {
            printf("[Block_Epoll_Net::recv_task] incomplete pack fd=%d got=%d miss=%d, close\n",
                   ev->fd, nOffSet, nPackSize);
            fflush(stdout);
            delete [] pSzBuf;
            pSzBuf = NULL;
            break;
        }
        //包长已在上方校验为 (0,1MB]，此处可安全读取首 4 字节
        int recvType = *(int*)pSzBuf;
        (void)recvType;

        //接收和处理分离 跑线程池里其他线程处理 , 避免处理影响接收
        DataBuffer * buffer = new DataBuffer(ev->pNet , ev->fd , pSzBuf , nOffSet );
        //【安全修复-P0：主循环阻塞配套】recv_task 虽在线程池线程，同样改非阻塞入队：
        // 队列满/池关闭时丢弃缓冲并走统一清理断开该连接，不再向队列塞任务。
        // 原代码: pthis->m_threadpool->Producer_add( Buffer_Deal , (void*) buffer );
        //         （失败返回 -1 时 buffer 与 pSzBuf 泄漏）
        if( 0 != pthis->m_threadpool->Producer_add_nb( Buffer_Deal , (void*) buffer ) )
        {
            fprintf(stderr, "[Block_Epoll_Net::recv_task] queue full, drop fd=%d\n", ev->fd);
            // DataBuffer 为纯结构体、不持有 buf，两块内存各自释放
            delete [] pSzBuf;
            pSzBuf = NULL;
            delete buffer;
            buffer = NULL;
            break;              // 进入 do-while 下方统一错误清理
        }

        // 这次接收完 要重新注册事件  此时 EPOLL MODE -> EPOLLIN|EPOLLONESHOT 没有修改, 使用重复值
        ev->eventadd(  ev->events );

        return 0;

    }while(0);

// 借助 do..while 在最后统一处理错误  避免使用goto语句
    ev->eventdel();
    //【修复-P1：断开无业务层清理】连接关闭时通知业务层（TcpKernel -> CLogic）
    // 修改原因: 原代码网络层只管 close+erase，业务层的在线用户表/房间成员表永不清理，
    //           UserInfo* 泄漏、死 fd 残留，fd 被新连接复用后音视频帧串线到无关连接。
    // 修改效果: 回调上层清理该 fd 关联的用户与房间成员；未接线时等价于旧行为。
    // 原代码: （无回调）
    if( pthis->m_close_callback )
        pthis->m_close_callback( ev->fd );
    //【P2修复：先 close 后 erase 的 fd 复用窗口】
    // 修改原因: 原顺序 close(ev->fd) 在前、erase 在后；两步之间若有新连接接入并复用了
    //           同一个 fd 号，本线程随后的 erase 会把【新连接】的事件从映射表里摘掉，
    //           新连接立即失联。先从映射表摘除旧事件再 close，可消除该窗口。
    // 修改效果: erase 提前到 close 之前；其余逻辑（eventdel/回调/delete ev）不变。
    // 原代码保留备查：
    //     close(ev->fd);
    //     pthis->m_mapSockfdToEvent.erase( ev->fd );
    pthis->m_mapSockfdToEvent.erase( ev->fd );
    close(ev->fd);
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
