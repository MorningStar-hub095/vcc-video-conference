#pragma once

#include"INetMediator.h"
#include <atomic>
#include <thread>
#include <mutex>

class TcpClientMediator :public INetMediator
{
//    Q_OBJECT
public:
	TcpClientMediator(void);
	~TcpClientMediator(void);

	//初始化网络
    bool OpenNet(const char* szBufIP = "0.0.0.0"
                    , unsigned short port = _DEF_TCP_PORT );
	//关闭网络
	void CloseNet();
	//发送 : 同时兼容tcp udp
    bool SendData( unsigned int lSendIP , char* buf , int nlen );
	//接收
    void DealData( unsigned int lSendIP , char* buf , int nlen );

    void disConnect();
    bool IsConnected();

private:
    //【修复-P1：SendData 内同步重连卡 UI】异步重连
    // 修改原因: 原代码在 SendData 里同步执行 UnInitNet(等2s)+delete+new+阻塞connect，
    //           主连接的 SendData 全部由 UI 线程调用 —— 断线期间任何一次发送都会
    //           把界面冻结数秒。
    // 修改效果: SendData 断线时只返回 false，并触发一次后台重连线程；
    //           重连成功后经 SIG_reconnected 通知上层补发注册包。
    // 原代码: 重连全流程内联在 SendData 中（阻塞 UI）
    void ReconnectWorker();
    std::atomic<bool> m_reconnecting;     // 重连进行中（保证只起一个重连线程）
    std::thread   m_reconnectThread;      // 重连线程（可 join，随对象销毁等待）
    std::mutex    m_netMutex;             // 保护 m_pNet 的替换与访问
};

