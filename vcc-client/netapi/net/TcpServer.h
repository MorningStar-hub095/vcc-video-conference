#pragma once

#include"INet.h"
#include<iostream>
#include<list>
#include<map>
#include <atomic> //【P2修复】为 m_isStop 提供 std::atomic<bool>

using namespace std;

class TcpServer : public INet
{
public:
	TcpServer(INetMediator * pMediator);
	~TcpServer(void);

	//初始化网络
    bool InitNet(const char* szBufIP , unsigned short port);
	//关闭网络
	void UnInitNet(); 
	//发送 : 同时兼容tcp udp 
    bool SendData( unsigned int lSendIP , char* buf , int nlen );
	
protected:
	//线程函数 : 接收数据
	static unsigned int __stdcall RecvThread( void * lpvoid); // 多个 每一个客户端有一个
	//线程函数 :接收客户端连接
	static unsigned int __stdcall AcceptThread( void * lpvoid); // 1个 
	//接收
	virtual void RecvData();

	SOCKET m_sock; // 主套接字
	//【安全修复-P0：WSACleanup 引用计数失衡】本实例是否持有 Winsock 引用
	bool m_wsaHeld;
	//SOCKET sockWaiter; // 用户和客户端对话的套接字 --> 一组 
	map<unsigned int , SOCKET> m_mapThreadIdToSocket;
	list<HANDLE> m_hThreadHandleLst; // 控制线程

	//【P2修复】同 TcpClient：m_isStop 跨线程读写改为原子量
	// 原代码保留备查： bool m_isStop;
	std::atomic<bool> m_isStop;
};

