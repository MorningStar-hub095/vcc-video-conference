#pragma once

#include"INet.h"
#include <atomic> //【P2修复】为 m_isStop 提供 std::atomic<bool>

class UdpNet:public INet
{
public:
	UdpNet( INetMediator * pMediator );
	~UdpNet(); //使用时, 父类指针指向子类, 使用虚析构
	//初始化网络
    bool InitNet(const char* szBufIP , unsigned short port);
	//关闭网络
	void UnInitNet();
	//发送 : 同时兼容tcp udp
    bool SendData(unsigned int lSendIP , char* buf , int nlen );

protected:
	static unsigned int __stdcall RecvThread( void * lpvoid);

	//接收
	virtual void RecvData();
	SOCKET m_sock;
	HANDLE m_hThreadHandle; // 控制接收线程
    //【安全修复-P0：WSACleanup 引用计数失衡】本实例是否持有 Winsock 引用
    bool m_wsaHeld;
	//【P2修复】同 TcpClient：m_isStop 跨线程读写改为原子量
	// 原代码保留备查： bool m_isStop;
	std::atomic<bool> m_isStop;
    int m_port;
};
