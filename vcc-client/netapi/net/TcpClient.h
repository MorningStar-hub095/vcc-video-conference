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
	//【修复-P1：m_sock 跨线程无保护】接收线程在断线时改写 m_sock（closesocket 后置
	// INVALID_SOCKET），发送线程同时读它做 send —— 普通 SOCKET 是数据竞争（UB）。
	// 改为 std::atomic<SOCKET>：读写原子化，发送线程要么拿到有效句柄要么拿到
	// INVALID_SOCKET（send 返回错误走既有失败路径），不再有撕裂读。
	// 原代码保留备查： SOCKET m_sock;
	std::atomic<SOCKET> m_sock;
	HANDLE m_hThreadHandle; // 控制接收线程
    //【安全修复-P0：WSACleanup 引用计数失衡】本实例是否持有 Winsock 引用
    bool m_wsaHeld;
	//【P2修复】m_isStop 被接收线程与主线程同时读写，普通 bool 是数据竞争（UB），
	// 原代码保留备查： bool m_isStop;
	// 改为 std::atomic<bool>：读写原子化，退出flag不再可能被优化进寄存器
	std::atomic<bool> m_isStop;
    //线程安全优化:m_isConnected 被接收线程(RecvData)和主线程(IsConnected/SendData)同时访问
    //原代码无同步保护，存在数据竞争(data race)，可能导致读取到不一致的状态值
    //使用 std::atomic<bool> 保证读写操作的原子性，避免数据竞争
    std::atomic<bool> m_isConnected;
};

