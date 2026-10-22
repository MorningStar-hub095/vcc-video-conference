#include "TcpServer.h"
#include"INetMediator.h"


TcpServer::TcpServer( INetMediator * pMediator ):m_sock( INVALID_SOCKET ),m_isStop(false)
{
	m_pMediator = pMediator;
    //【安全修复-P0：WSACleanup 引用计数失衡】实例持有标志
    m_wsaHeld = false;
}

TcpServer::~TcpServer()  //使用时, 父类指针指向子类, 使用虚析构
{
    TcpServer::UnInitNet();
}

//初始化网络	//加载库 创建套接字 绑定
bool TcpServer::InitNet(const char *szBufIP, unsigned short port)
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
	m_sock = socket( AF_INET , SOCK_STREAM , IPPROTO_TCP ); // ipv4 udp
	if ( m_sock == INVALID_SOCKET) {
		WSACleanup();
        return false;
	}

	//3.bind
	sockaddr_in addr;
	addr.sin_family = AF_INET ;
    addr.sin_addr.S_un.S_addr = inet_addr(szBufIP); /*INADDR_ANY*/ ;  //绑定任意网卡
    addr.sin_port = htons( port );  //htons 转换为网络字节序 大端存储  43232

	if( bind( m_sock ,(const sockaddr* ) &addr , sizeof(addr) ) == SOCKET_ERROR )
	{
		UnInitNet();
        return false;
	}

	//4. listen
	if( listen( m_sock , 10 ) ==  SOCKET_ERROR )
	{
		UnInitNet();
        return false;
	}

	//5. 创建接收连接线程 -- 等待客户端连接

	// 收数据 -- 创建线程 CreateThread  WinAPI  strcpy  C/C++ RunTime 库函数 创建内存块
	HANDLE hThreadHandle = (HANDLE)_beginthreadex(  NULL, 0 ,&AcceptThread ,this , 0 , NULL );     //( CreateThread 创建内存块 )
	//_endthreadex(); -- 回收内存块      //( ExitThread 不回收内存块 ) --内存泄露
	if( hThreadHandle )
	{
		m_hThreadHandleLst .push_back( hThreadHandle );
	}

	return true;
}

unsigned int __stdcall TcpServer::RecvThread( void * lpvoid)
{
	TcpServer* pthis = (TcpServer*) lpvoid;
	pthis->RecvData();
	
	return 0;
}

unsigned int __stdcall TcpServer::AcceptThread( void * lpvoid)
{
	//网络IO模型 接收连接 之后, 每一个客户端需要一个线程
	TcpServer* pthis = (TcpServer*) lpvoid;
	sockaddr_in addrClient;
	int nSize = sizeof(addrClient);
	while( !pthis->m_isStop )
	{
		SOCKET sockWaiter = accept( pthis-> m_sock , (sockaddr*)&addrClient , &nSize );
		cout << "IP:"<< inet_ntoa(addrClient.sin_addr) <<" connect" <<endl;

		//每一个客户端都对应一个套接字
		//每一个客户端 会对应 一个线程 一个套接字 --> 线程和套接字也是对应的
		//可以在创建线程的时候 把线程id 和套接字 绑定在一起 形成映射 m_mapThreadIdToSocket
		unsigned int threadID = 0;
		HANDLE handle = (HANDLE)_beginthreadex( NULL , 0 , RecvThread ,pthis , 0 , &threadID);
		//m_mapThreadIdToSocket
		pthis->m_hThreadHandleLst.push_back( handle );

		pthis->m_mapThreadIdToSocket[threadID] = sockWaiter;
	}
	return 0;
}

//关闭网络
void TcpServer::UnInitNet()
{
	m_isStop = true ; // 尝试退出线程循环 

	for( auto ite = m_hThreadHandleLst.begin() ;ite != m_hThreadHandleLst.end() ; ++ite  )
	{
		if( *ite )
		{
			if( WaitForSingleObject(*ite , 100 ) == WAIT_TIMEOUT )
			{
				TerminateThread( *ite , -1 );
			}
			CloseHandle(*ite);
			*ite = NULL;
		}
	}
	
	if( m_sock && m_sock != INVALID_SOCKET )
		closesocket(m_sock);

	//每一个客户的套接字的回收 
	for( auto ite = m_mapThreadIdToSocket.begin() ; ite !=m_mapThreadIdToSocket.end(); ++ite )
	{
		if( ite->second && ite->second != INVALID_SOCKET )
			closesocket(ite->second);
	}

    //【安全修复-P0：WSACleanup 引用计数失衡】仅释放本实例引用
    // 原代码: if(m_isLoadlib){ WSACleanup(); m_isLoadlib = false; }
    if( m_wsaHeld ){
        wsaCleanupRef();
        m_wsaHeld = false;
    }
} 


//发送 : 同时兼容tcp udp 
bool TcpServer::SendData(unsigned int lSendIP , char* szbuf , int nlen )
{
    if( !szbuf|| nlen <= 0 ) return false;

	//防止粘包  策略: 先发包大小 再发数据包
	// m_sock  <--> lSendIP
/*
	send( lSendIP , (char*)&nlen , sizeof(int) , 0  );

	if( send( lSendIP , buf , nlen , 0  ) <= 0 )
		return false;
	
	return true;
*/
    int DataLen = nlen + 4;
    std::vector<char> vecbuf;
    vecbuf.resize( DataLen );

 //   char* buf = new char;
    char* buf = &*vecbuf.begin();
    char* tmp = buf;
    *(int*) tmp = nlen;
    tmp+= sizeof(int);

    memcpy( tmp , szbuf , nlen);

    int res = send( lSendIP ,buf,DataLen , 0);
//    delete buf;
    return res;

}
//接收
void TcpServer::RecvData()
{
	//取得套接字 线程id --> 套接字 
	//获取线程的id  GetCurrentThreadId();
	Sleep(100);
	SOCKET sockWaiter = m_mapThreadIdToSocket[GetCurrentThreadId()];

	if( !sockWaiter || sockWaiter == INVALID_SOCKET  ) return;
	int nPackSize = 0; // 存储包大小 --> 未来要读取的字节数
	int nRes = 0;
	while( !m_isStop )
	{
		//接收先接收包大小 在接受数据包
		nRes = recv( sockWaiter , (char*)&nPackSize , sizeof(int) , 0   ); 
		//从接收缓冲区拷贝包大小
		if( nRes <= 0 )
		{
            closesocket(sockWaiter);
			break;
		}
		int offset = 0; // 从buf 起始位置偏移多少
		char * buf = new char[nPackSize];
		while( nPackSize )
		{
			nRes = recv( sockWaiter , buf + offset , nPackSize  , 0   );
			if( nRes > 0 )
			{
				nPackSize -= nRes;
				offset += nRes;
			}
		}
		this->m_pMediator->DealData( sockWaiter , buf , offset  );  //需要在这个函数回收 堆区 空间
	}
}
