#pragma once

#include"INet.h"
#include <atomic> //线程安全优化:为 m_isConnected 提供 std::atomic<bool>


class TcpClient : public INet
{
public:
	TcpClient( INetMediator * pMediator);
	~TcpClient(void);

	//初始化网络
    bool InitNet(const char* szBufIP , unsigned short port);
	//关闭网络
	void UnInitNet(); 
	//发送 : 同时兼容tcp udp 
    bool SendData( unsigned int lSendIP , char* buf , int nlen );

    bool IsConnected();
protected:
	static unsigned int __stdcall RecvThread( void * lpvoid);

	//接收
	virtual void RecvData();
	SOCKET m_sock;
	HANDLE m_hThreadHandle; // 控制接收线程
	bool m_isStop;
    //线程安全优化:m_isConnected 被接收线程(RecvData)和主线程(IsConnected/SendData)同时访问
    //原代码无同步保护，存在数据竞争(data race)，可能导致读取到不一致的状态值
    //使用 std::atomic<bool> 保证读写操作的原子性，避免数据竞争
    std::atomic<bool> m_isConnected;
};

