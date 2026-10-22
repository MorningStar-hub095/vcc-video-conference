#pragma once

#include<winsock2.h>

//加载动态库的引入库
//#pragma comment(lib, "ws2_32.lib")  vs 特定的
#include<ws2tcpip.h>
#include<process.h>

//#include"packdef.h"
#include<string>
#include<set>


class INetMediator;
class INet
{
public:
	INet(){}
	virtual ~INet(){} //使用时, 父类指针指向子类, 使用虚析构
	//初始化网络
    virtual bool InitNet(const char* szBufIP , unsigned short port) = 0 ;
	//关闭网络
	virtual void UnInitNet() = 0 ; 
	//发送 : 同时兼容tcp udp 
    virtual bool SendData( unsigned int lSendIP , char* buf , int nlen ) = 0;
	
    //ip long型向字符串转换
    static std::string GetIPString( long ip );

    //获取本地的所有网络ip 地址列表  key - value  std::set
    static std::set<long> GetValidIPList();
    //判断当前有没有连接到服务器 -- TCP客户端使用
    virtual bool IsConnected(){ return true;}

protected:
	//接收
	virtual void RecvData() = 0 ;
	INetMediator * m_pMediator; //中介 , 用于将网络数据向类外传输

    //【安全修复-P0：WSACleanup 引用计数失衡】
    // 修改原因: 原代码 static bool m_isLoadlib 全进程只置一次 true（首次 WSAStartup），
    //           但 TcpClient/TcpServer/UdpNet 每个实例的 UnInitNet 都无条件
    //           WSACleanup() —— 第一次 Cleanup 就把引用计数清 0，
    //           其余连接（主连接/音频/视频共 3 条）的 socket 句柄随之全部失效，
    //           表现为"退出房间或断线重连后音视频通道莫名全挂"。
    // 修改效果: 引用计数改为进程级：wsaStartupRef() 首次调用执行 WSAStartup
    //           并计数 +1，之后仅计数；wsaCleanupRef() 计数 -1，归零才 WSACleanup。
    //           各实例用自身 m_wsaHeld 防止 UnInitNet 重复调用导致多减。
    // 原代码: static bool m_isLoadlib;（三个子类各自 if(m_isLoadlib){WSACleanup();...}）
    static bool wsaStartupRef();   // 首次 WSAStartup；成功计数+1 返回 true
    static void wsaCleanupRef();   // 计数-1；归零执行 WSACleanup
    static int  m_wsaRefCount;     // 进程级 Winsock 引用计数
    bool m_wsaHeld;                // 本实例是否持有一次 Winsock 引用
};
