#include "TcpClientMediator.h"

#include"TcpClient.h"

TcpClientMediator::TcpClientMediator()
{
	//new 网络对象
	m_pNet = new TcpClient(this);
    //【修复-P1：SendData 内同步重连卡 UI】异步重连状态初始化
    m_reconnecting = false;
}
TcpClientMediator::~TcpClientMediator()  //使用时, 父类指针指向子类, 使用虚析构
{
    //【修复-P1：同步重连异步化配套】析构前必须等重连线程退出，
    // 否则 worker 还在访问 m_pNet 就被析构 —— use-after-free。
    if( m_reconnectThread.joinable() )
        m_reconnectThread.join();
	if( m_pNet )
	{
		delete m_pNet;
		m_pNet = NULL;
	}
}
//初始化网络
bool TcpClientMediator::OpenNet(const char *szBufIP, unsigned short port)
{

    strcpy_s( m_szBufIP, sizeof(m_szBufIP), szBufIP);
    m_port = port;

    return m_pNet->InitNet(szBufIP , port);
} 
//关闭网络
void TcpClientMediator::CloseNet()
{
	m_pNet->UnInitNet();
} 
//发送 : 同时兼容tcp udp
bool TcpClientMediator::SendData( unsigned int lSendIP , char* buf , int nlen )
{
    Q_UNUSED(lSendIP);
    //【修复-P1：SendData 内同步重连卡 UI】
    // 修改原因: 原代码断线时在【调用线程】内联完成 UnInitNet(等2s)+new+阻塞connect，
    //           主连接的 SendData 全部来自 UI 线程，断线期间界面冻结数秒；
    //           音视频帧发送线程也会被同样卡住。
    // 修改效果: 未连接时立即返回 false（调用方按发送失败处理），
    //           同时用 CAS 保证全进程只起一个后台重连线程；
    //           重连成功经 SIG_reconnected 通知上层补发注册包（既有机制）。
    // 原代码: 重连全流程内联在 else 分支中（阻塞调用线程）
    if( IsConnected() )
    {
        std::lock_guard<std::mutex> lck( m_netMutex );
        if( m_pNet )
            return m_pNet->SendData( 0 , buf, nlen);
        return false;
    }

    bool expected = false;
    if( m_reconnecting.compare_exchange_strong( expected , true ) )
    {
        // 上一次重连线程若已结束则先回收，再启动新一轮
        if( m_reconnectThread.joinable() )
            m_reconnectThread.join();
        m_reconnectThread = std::thread( &TcpClientMediator::ReconnectWorker , this );
    }
    return false;
}

//【修复-P1】后台重连线程体：旧连接优雅关闭 -> 建新连接 -> 原子替换 -> 通知上层
void TcpClientMediator::ReconnectWorker()
{
    printf("[TcpClientMediator::ReconnectWorker] reconnecting to %s:%d\n", m_szBufIP, m_port);
    fflush(stdout);

    // 旧对象优雅关闭耗时（等接收线程最多 2s），在锁外执行避免长时间占锁
    m_pNet->UnInitNet();
    TcpClient* pNew = new TcpClient(this);
    bool bOk = pNew->InitNet( m_szBufIP , m_port );

    {
        std::lock_guard<std::mutex> lck( m_netMutex );
        delete m_pNet;      // UnInitNet 已做过，此处仅回收对象（幂等）
        m_pNet = pNew;
    }

    if( bOk )
    {
        printf("[TcpClientMediator::ReconnectWorker] reconnect OK\n");
        fflush(stdout);
        //【P2修复：断线重连不重发音视频注册包】通知上层补发注册包（跨线程队列投递）
        Q_EMIT SIG_reconnected();
    }
    else
    {
        printf("[TcpClientMediator::ReconnectWorker] reconnect FAILED\n");
        fflush(stdout);
    }
    m_reconnecting = false;
}
#include<iostream>
//处理 
void TcpClientMediator::DealData(unsigned int lSendIP , char* buf , int nlen )
{
	//将这几个参数 以信号的形式发送出去  -- IKernel类 的槽函数处理 -- 注意空间回收
    //std::cout<< lSendIP << ":" << buf <<std::endl;
    Q_EMIT SIG_ReadyData( lSendIP , buf , nlen);
}

void TcpClientMediator::disConnect()
{
    Q_EMIT SIG_disConnect();
}

bool TcpClientMediator::IsConnected()
{
    //【修复-P1：同步重连异步化配套】重连线程会替换 m_pNet，读取加锁防悬挂
    std::lock_guard<std::mutex> lck( m_netMutex );
    return m_pNet ? m_pNet->IsConnected() : false;
}

