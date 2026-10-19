#include "TcpClient.h"


#include"INetMediator.h"



TcpClient::TcpClient( INetMediator * pMediator ):m_sock( INVALID_SOCKET ),m_isStop(false)
{
	m_pMediator = pMediator;
    m_isConnected = false; //atomic<bool> 支持隐式赋值
	m_hThreadHandle = NULL;
}

TcpClient::~TcpClient()  //使用时, 父类指针指向子类, 使用虚析构
{
    TcpClient::UnInitNet();
}
//初始化网络	//加载库 创建套接字 绑定
bool TcpClient::InitNet(const char *szBufIP, unsigned short port)
{
    if( !m_isLoadlib )
    {
        //1.做个小买卖 炸串 --加载库
        WORD wVersionRequested;
        WSADATA wsaData;
        int err;

    /* Use the MAKEWORD(lowbyte, highbyte) macro declared in Windef.h */
        wVersionRequested = MAKEWORD(2, 2);

        err = WSAStartup(wVersionRequested, &wsaData);
        if (err != 0) {
            return false;
        }

        if (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2) {
            WSACleanup();
            return false;
        }
        m_isLoadlib = true;
    }

    m_isConnected = false;

	//2.雇人-- 创建套接字 进程与外界网络通信需要的接口 决定了与外界通讯的方式(tcp udp)
	m_sock = socket( AF_INET , SOCK_STREAM , IPPROTO_TCP ); // ipv4 udp
	if ( m_sock == INVALID_SOCKET) {
		WSACleanup();
        return false;
	}

	//3. 连接服务器
	sockaddr_in addr;
	addr.sin_family = AF_INET ;
    addr.sin_addr.S_un.S_addr = inet_addr( szBufIP );  //绑定任意网卡
    addr.sin_port = htons( port );  //htons 转换为网络字节序 大端存储  43232

    //  记录连接目标
    printf("[TcpClient::InitNet] connecting to %s:%d, m_sock=%d\n", szBufIP, port, m_sock);

	if( connect( m_sock ,(const sockaddr* ) &addr , sizeof(addr) ) == SOCKET_ERROR )
	{
        printf("[TcpClient::InitNet] connect FAILED, errno=%d\n", WSAGetLastError());
		UnInitNet();
        return false;
	}
    //  连接成功
    printf("[TcpClient::InitNet] connect OK, m_sock=%d\n", m_sock);

    //设置客户端发送缓冲区
    //  缓冲区从256KB扩大到1MB
    // 原值(SD): nSendBuf=256*1024(256KB)
    // 更早的原值: nSendBuf=64*1024(64KB)
    // 修改原因: 1280x720 H264 I帧可达50-150KB，1920x1080桌面共享帧更大，
    //           256KB缓冲区在HD分辨率下易导致发送阻塞
    // 影响范围: 每条TCP连接内存占用增加约768KB，3条连接共增加约2.3MB
    int nSendBuf=1024*1024;  //  256KB → 1MB，适配HD分辨率
    setsockopt(m_sock,SOL_SOCKET,SO_SNDBUF,(const char*)&nSendBuf,sizeof(int));
    //设置客户端接收缓冲区
    //  缓冲区从256KB扩大到1MB
    // 原值(SD): nRecvBuf=256*1024(256KB)
    // 更早的原值: nRecvBuf=64*1024(64KB)
    int nRecvBuf=1024*1024;  //  256KB → 1MB，适配HD分辨率
    setsockopt(m_sock,SOL_SOCKET,SO_RCVBUF,(const char*)&nRecvBuf,sizeof(int));

//    DWORD nNetTimeout= 300;//1秒 - 1000

//    //发送时限 , 超时退出避免长时间阻塞
//    setsockopt(m_sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&nNetTimeout, sizeof(DWORD));

    //  启用发送超时，防止工作线程在 send() 中无限阻塞
    // 原Bug: SO_SNDTIMEO 被注释掉，send() 在发送缓冲区满时无限阻塞。
    //        现在 SendData 在 SendVideoWorker 线程执行，若 send() 阻塞，
    //        worker 线程无法退出，slot_destroy 中 wait(3000) 超时后 terminate()，
    //        可能导致资源泄漏或堆损坏。
    // 修复: 设置 500ms 发送超时。send() 超时后返回 SOCKET_ERROR，
    //        SendData 返回 false，worker 线程可正常处理后续事件和退出。
    DWORD nNetTimeoutMs = 500;  // 500ms 发送超时
    setsockopt(m_sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&nNetTimeoutMs, sizeof(DWORD));

    //禁用 TCP-NODELAY
    int value = 1;
    setsockopt(m_sock, IPPROTO_TCP, TCP_NODELAY, (char *)&value, sizeof(int));


    m_isConnected = true;
	// 收数据 -- 创建线程 CreateThread  WinAPI  strcpy  C/C++ RunTime 库函数 创建内存块
	m_hThreadHandle = (HANDLE)_beginthreadex(  NULL, 0 ,&RecvThread ,this , 0 , NULL );     //( CreateThread 创建内存块 )
	//_endthreadex(); -- 回收内存块      //( ExitThread 不回收内存块 ) --内存泄露

	return true;
}

unsigned int __stdcall TcpClient::RecvThread( void * lpvoid)
{
	TcpClient* pthis = (TcpClient*) lpvoid;
	pthis->RecvData();
	
	return 0;
}

//关闭网络
// 使用 shutdown() 优雅关闭连接，替代直接 TerminateThread + closesocket
// 修改原因: 原代码在 RecvData 线程阻塞在 recv() 时，直接调用 TerminateThread 强制终止线程，
//           然后 closesocket()。这会导致：
//           1. 发送缓冲区中未发送的数据(如视频帧19483字节)被丢弃
//           2. Windows 发送 RST 而非 FIN，服务端收到 ECONNRESET (errno=104)
//           3. 服务端内核丢弃所有已接收但未 read() 的数据，VideoFrameRq 永远收不到
//           4. 视频连接被重置，视频功能完全不可用
// 修改内容: 先调用 shutdown(SD_BOTH)，让 Windows 内核：
//           a) 先把发送缓冲区的数据全部发送出去，再发 FIN（保证数据不丢失）
//           b) 打断阻塞的 recv()，使其返回0或-1，RecvData 线程能检测到并正常退出
//           c) 等待线程退出后再 closesocket()，避免资源泄漏
// 影响范围: 所有 TCP 连接的关闭流程（主连接、音频连接、视频连接）
// 原代码:
//   m_isStop = true;
//   if( m_hThreadHandle ) {
//       if( WaitForSingleObject(m_hThreadHandle, 100) == WAIT_TIMEOUT )
//           TerminateThread(m_hThreadHandle, -1);
//       CloseHandle(m_hThreadHandle);
//       m_hThreadHandle = NULL;
//   }
//   if( m_sock && m_sock != INVALID_SOCKET )
//       closesocket(m_sock);
void TcpClient::UnInitNet()
{
	m_isStop = true ; // 尝试退出线程循环

    // 步骤1: 调用 shutdown() 优雅关闭连接
    // SD_BOTH 会: 1) 发送 FIN 前先flush发送缓冲区  2) 打断阻塞的 recv()
    if( m_sock != INVALID_SOCKET && m_sock != 0 )
    {
        shutdown( m_sock, SD_BOTH );
    }

    // 步骤2: 等待 RecvData 线程退出
    // shutdown() 会让阻塞的 recv() 立即返回0或-1，线程检测后退出
    // 给 2000ms 足够时间让线程退出并处理 disConnect 信号
	if( m_hThreadHandle )
	{
		if( WaitForSingleObject(m_hThreadHandle , 2000 ) == WAIT_TIMEOUT )
		{
            printf("[TcpClient::UnInitNet] WARNING: RecvData thread did not exit after 2s, force terminating, m_sock=%d\n", m_sock);
            fflush(stdout);
			TerminateThread( m_hThreadHandle , -1 );
		}
		CloseHandle(m_hThreadHandle);
		m_hThreadHandle = NULL;
	}

    // 步骤3: 关闭 socket（如果 RecvData 线程还没关的话）
    // RecvData 线程在 m_isStop=true 时不会调用 closesocket，由这里统一关闭
	if( m_sock && m_sock != INVALID_SOCKET )
		closesocket(m_sock);
    m_sock = INVALID_SOCKET;
    m_isConnected = false;

    if( m_isLoadlib )
    {
        WSACleanup();
        m_isLoadlib = false;
    }
}
//发送 : 同时兼容tcp udp 
bool TcpClient::SendData(unsigned int lSendIP , char* szbuf , int nlen )
{
    if( !szbuf|| nlen <= 0 ) return false;

	//防止粘包  策略: 先发包大小 再发数据包
	// m_sock  <--> lSendIP
/*	send( m_sock , (char*)&nlen , sizeof(int) , 0  );

	if( send( m_sock , buf , nlen , 0  ) <= 0 )
		return false;

	return true;
*/

    lSendIP = m_sock;

    //  记录发送socket和数据大小
    int sendType = *(int*)szbuf;
    printf("[TcpClient::SendData] m_sock=%d, nlen=%d, packType=%d\n", m_sock, nlen, sendType);
    fflush(stdout);

    int DataLen = nlen + 4;
    std::vector<char> vecbuf;
    vecbuf.resize( DataLen );

 //   char* buf = new char;
    char* buf = &*vecbuf.begin();
    char* tmp = buf;
    *(int*) tmp = nlen;
    tmp+= sizeof(int);

    memcpy( tmp , szbuf , nlen);

    // 处理 send() 部分发送的情况
    // 修改原因: 原代码只调用一次 send()。当发送缓冲区空间不足时，send() 可能只发送
    //           部分数据就返回，剩余数据被静默丢弃。视频帧(19483字节)较大，
    //           在发送缓冲区接近满时极易触发此问题，导致服务端收到不完整的数据包。
    // 修改内容: 使用循环确保所有数据都被发送，send() 失败时返回 false
    int totalSent = 0;
    while( totalSent < DataLen )
    {
        int res = send( lSendIP , buf + totalSent , DataLen - totalSent , 0);
        if( res == SOCKET_ERROR )
        {
            printf("[TcpClient::SendData] send() FAILED, wsaErr=%d, sent=%d/%d, packType=%d\n",
                   WSAGetLastError(), totalSent, DataLen, sendType);
            fflush(stdout);
            return false;
        }
        if( res == 0 )
        {
            // send() 返回0通常表示连接已关闭
            printf("[TcpClient::SendData] send() returned 0, connection closed, sent=%d/%d\n", totalSent, DataLen);
            fflush(stdout);
            return false;
        }
        totalSent += res;
    }

    return true;
}
//接收

void TcpClient::RecvData()
{
	int nPackSize = 0; // 存储包大小
	int nRes = 0;
	while( !m_isStop )
	{
		//接收先接收包大小 在接受数据包
		nRes = recv( m_sock , (char*)&nPackSize , sizeof(int) , 0   );
		//从接收缓冲区拷贝包大小
        if( nRes == 0)
        {
            //对端正常关闭连接
            printf("[TcpClient::RecvData] Connection closed by peer (recv=0), m_sock=%d\n", m_sock);
            fflush(stdout);
            // m_isStop=true 时由 UnInitNet 统一关闭 socket，这里不重复关闭
            if( !m_isStop ) {
                this->m_isConnected = false;
                this->m_pMediator->disConnect();
                closesocket(m_sock);
                m_sock = INVALID_SOCKET;
            }
            break;
        }
        if( nRes < 0 )
        {
            // Windows 下使用 WSAGetLastError() 替代 errno
            // 修改原因: Windows Winsock 的错误码通过 WSAGetLastError() 获取，
            //           而 errno 是 POSIX 概念，在 Windows 上不能正确反映 socket 错误。
            //           原代码检查 errno == EINTR/EWOULDBLOCK/EAGAIN（Linux值4/11/11），
            //           但 Windows socket 错误码是 WSAEINTR=10004, WSAEWOULDBLOCK=10035 等，
            //           永远不会匹配，导致所有可恢复错误被当作致命错误处理，socket 被关闭。
            int wsaErr = WSAGetLastError();
            printf("[TcpClient::RecvData] recv() failed, wsaErr=%d, m_sock=%d, m_isStop=%d\n",
                   wsaErr, m_sock, (int)m_isStop);
            fflush(stdout);

            // m_isStop=true 说明是 UnInitNet 调用了 shutdown()，正常退出
            if( m_isStop ) break;

            // WSAEINTR: 操作被 WSACancelBlockingCall 中断，可重试
            if( wsaErr == WSAEINTR )
            {
                continue;
            }
            // 其他错误（WSAECONNRESET=10054, WSAECONNABORTED=10053, WSAESHUTDOWN=10058 等）
            // 均为不可恢复错误，关闭连接并退出
            this->m_isConnected = false;
            this->m_pMediator->disConnect();
            closesocket(m_sock);
            m_sock = INVALID_SOCKET;
            break;
        }
        //校验包大小合法性
        //原代码未校验 nPackSize，恶意服务器或网络错误可能发送极大值导致
        //new char 分配超大内存或负值(转无符号后变大)导致OOM
        //限制单个数据包最大 1MB，超过则视为非法包，跳过并继续
        if( nPackSize <= 0 || nPackSize > 1024 * 1024 )
        {
            printf("[TcpClient::RecvData] Invalid pack size=%d, skipping\n", nPackSize);
            fflush(stdout);
            continue; //非法包大小，跳过
        }
		int offset = 0;
		char * buf = new char[nPackSize];
        //原内层 while(nPackSize) 循环未处理 recv 返回 0 或负值的情况：
        //修复：增加对 recv 返回值的判断，异常时释放 buf 并退出循环
		while( nPackSize > 0 )
		{
			nRes = recv( m_sock , buf + offset , nPackSize  , 0   );
			if( nRes > 0 )
			{
				nPackSize -= nRes;
				offset += nRes;
			}
            else if( nRes == 0 )
            {
                //连接已关闭，释放缓冲区避免泄漏，并退出外层循环
                delete[] buf;
                printf("[TcpClient::RecvData] Inner recv: connection closed (recv=0), m_sock=%d\n", m_sock);
                fflush(stdout);
                if( !m_isStop ) {
                    this->m_isConnected = false;
                    this->m_pMediator->disConnect();
                    closesocket(m_sock);
                    m_sock = INVALID_SOCKET;
                }
                goto recv_exit; //跳出外层while循环
            }
            else // nRes < 0
            {
                int wsaErr2 = WSAGetLastError();
                // Windows: 使用 WSAGetLastError 替代 errno
                if( wsaErr2 == WSAEINTR )
                {
                    continue; //可恢复错误，重试
                }
                //不可恢复错误，释放缓冲区并退出
                delete[] buf;
                printf("[TcpClient::RecvData] Inner recv() failed, wsaErr=%d, m_sock=%d\n", wsaErr2, m_sock);
                fflush(stdout);
                if( !m_isStop ) {
                    this->m_isConnected = false;
                    closesocket(m_sock);
                    m_sock = INVALID_SOCKET;
                }
                goto recv_exit;
            }
		}
		this->m_pMediator->DealData( m_sock , buf , offset  );  //需要在这个函数回收 堆区 空间
	}
recv_exit:
    printf("[TcpClient::RecvData] Thread exiting, m_sock=%d, m_isStop=%d\n", m_sock, (int)m_isStop);
    fflush(stdout);
}

bool TcpClient::IsConnected()
{
    return  m_isConnected;
}

