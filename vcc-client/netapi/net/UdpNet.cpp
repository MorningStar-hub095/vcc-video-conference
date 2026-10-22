#include"UdpNet.h"

#include"INetMediator.h"



UdpNet::UdpNet( INetMediator * pMediator ):m_sock( INVALID_SOCKET ),m_isStop(false)
{
	m_pMediator = pMediator;
	m_hThreadHandle = NULL;
    //【安全修复-P0：WSACleanup 引用计数失衡】实例持有标志
    m_wsaHeld = false;
}
UdpNet::~UdpNet()  //使用时, 父类指针指向子类, 使用虚析构
{
    UdpNet::UnInitNet();
}
//初始化网络	//加载库 创建套接字 绑定
bool UdpNet::InitNet(const char* szBufIP , unsigned short port)
{
    //【安全修复-P0：WSACleanup 引用计数失衡】进程级引用计数，见 INet.h 说明
    // 原代码: if( !m_isLoadlib ){ ...WSAStartup... m_isLoadlib = true; }
    if( !m_wsaHeld )
    {
        if( !wsaStartupRef() )
            return false;
        m_wsaHeld = true;
    }


	//2.雇人-- 创建套接字 进程与外界网络通信需要的接口 决定了与外界通讯的方式(tcp udp)
	m_sock = socket( AF_INET , SOCK_DGRAM , IPPROTO_UDP ); // ipv4 udp
	if ( m_sock == INVALID_SOCKET) {
		WSACleanup();
        return false;
	}

	//设置有限广播权限
	BOOL bval = TRUE;
	setsockopt( m_sock, SOL_SOCKET/*哪一层*/ , /*什么属性*/ SO_BROADCAST , 
		/*设置的值 设置和取消*/(const char*)&bval , sizeof(bval) );

	//3.找地-- 绑定ip地址 
	sockaddr_in addr;
	addr.sin_family = AF_INET ;
    addr.sin_addr.S_un.S_addr = inet_addr( szBufIP );/*INADDR_ANY*/   //绑定任意网卡
    addr.sin_port = htons( port );  //htons 转换为网络字节序 大端存储  43232
    m_port = port;

	if( bind( m_sock ,(const sockaddr* ) &addr , sizeof(addr) ) == SOCKET_ERROR )
	{
		UnInitNet();
        return false;
	}

	// 收数据 -- 创建线程 CreateThread  WinAPI  strcpy  C/C++ RunTime 库函数 创建内存块
	m_hThreadHandle = (HANDLE)_beginthreadex(  NULL, 0 ,&RecvThread ,this , 0 , NULL );     //( CreateThread 创建内存块 )
	//_endthreadex(); -- 回收内存块      //( ExitThread 不回收内存块 ) --内存泄露

	return true;
}

unsigned int __stdcall UdpNet::RecvThread( void * lpvoid)
{
	UdpNet* pthis = (UdpNet*) lpvoid;
	pthis->RecvData();
	
	return 0;
}

//关闭网络
void UdpNet::UnInitNet()
{
	//【P2修复：TerminateThread 强杀接收线程】
	// 修改原因: 原代码先等 100ms 就 TerminateThread 强杀，而 RecvData 大概率正阻塞在
	//           recvfrom() 上（100ms 内根本等不到自旋退出），强杀时线程持有的 CRT 资源/
	//           栈上对象不释放，且 DealData 可能执行到一半被打断。
	// 修改效果: 先 closesocket 让阻塞中的 recvfrom 立即返回错误、线程自然走到循环条件
	//           退出；等待时间放宽到 1000ms。强杀路径仅作兜底保留（注释保留）。
	// 原代码保留备查：
	//     m_isStop = true;
	//     if( m_hThreadHandle ) {
	//         if( WaitForSingleObject(m_hThreadHandle , 100 ) == WAIT_TIMEOUT )
	//             TerminateThread( m_hThreadHandle , -1 );
	//         CloseHandle(m_hThreadHandle); m_hThreadHandle = NULL;
	//     }
	m_isStop = true ; // 尝试退出线程循环
	if( m_sock && m_sock != INVALID_SOCKET )
	{
		closesocket( m_sock );   // 解除 RecvData 中 recvfrom 的阻塞，让线程自然退出
		m_sock = INVALID_SOCKET;
	}
	if( m_hThreadHandle )
	{
		if( WaitForSingleObject(m_hThreadHandle , 1000 ) == WAIT_TIMEOUT )
		{
			// 兜底：极少数情况下仍退出不了才强杀（保留原路径以备追溯）
			TerminateThread( m_hThreadHandle , -1 );
		}
		CloseHandle(m_hThreadHandle);
		m_hThreadHandle = NULL;
	}
	if( m_sock && m_sock != INVALID_SOCKET )
		closesocket(m_sock);
    //【安全修复-P0：WSACleanup 引用计数失衡】仅释放本实例引用
    // 原代码: if(m_isLoadlib){ WSACleanup(); m_isLoadlib = false; }
    if( m_wsaHeld ){
        wsaCleanupRef();
        m_wsaHeld = false;
    }
}
//发送 : 同时兼容tcp udp 
bool UdpNet::SendData( unsigned int lSendIP , char* buf , int nlen )
{
	SOCKADDR_IN addr;
	addr.sin_family = AF_INET;
    addr.sin_port = htons( m_port );
	addr.sin_addr.S_un.S_addr = lSendIP;

	if( sendto( m_sock , buf , nlen , 0 , (const sockaddr * )&addr ,sizeof(addr) ) <= 0 ) // 目标
		return false;
	
	return true;
}
//接收

void UdpNet::RecvData()
{
    char buf[ 4096 ] = "";
	SOCKADDR_IN addrClient;
	int nSize = sizeof(addrClient);
	m_isStop = false;
	while( !m_isStop )
	{
		int nRes = recvfrom( m_sock , buf , sizeof(buf) , 0 , (sockaddr*)&addrClient , &nSize );
		
		if( nRes > 0 )
		{    //处理 --中介处理
			char * packbuf = new char[nRes];
            memcpy( packbuf  ,buf , nRes);
			if( m_pMediator ){
				m_pMediator->DealData( addrClient.sin_addr.S_un.S_addr , packbuf , nRes );
				//   要回收这个空间 delete packbuf;
			}
		}	
	}
}
