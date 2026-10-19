#include "TcpClientMediator.h"

#include"TcpClient.h"

TcpClientMediator::TcpClientMediator()
{
	//new 网络对象
	m_pNet = new TcpClient(this);
}
TcpClientMediator::~TcpClientMediator()  //使用时, 父类指针指向子类, 使用虚析构
{
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
    if( IsConnected() )
    {
        //  连接正常，直接发送
        return m_pNet->SendData( 0 , buf, nlen);
    }
    else
    {
        //  连接断开，尝试重连
        printf("[TcpClientMediator::SendData] NOT connected, trying reconnect, IP=%s, port=%d\n", m_szBufIP, m_port);
        //【Bug修复】断线重连逻辑
        //原代码问题：重连失败时 return -1;
        //由于函数返回类型为 bool，-1 会被隐式转换为 true，
        //导致调用方认为发送成功，实际上数据并未发出，引发数据丢失。
        //修复：重连失败时返回 false，明确告知调用方发送失败
        m_pNet->UnInitNet();
        delete m_pNet;
        m_pNet = new TcpClient(this);
        if( this->OpenNet( m_szBufIP , m_port ) )
        {
            printf("[TcpClientMediator::SendData] reconnect OK, resending\n");
            return m_pNet->SendData( 0,buf,nlen);
        }
        else
        {
            printf("[TcpClientMediator::SendData] reconnect FAILED\n");
            return false; //【Bug修复】原为 return -1; (会被转为true)
        }
    }
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
    return m_pNet->IsConnected();
}

